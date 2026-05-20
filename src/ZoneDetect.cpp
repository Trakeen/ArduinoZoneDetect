#include "ZoneDetect.h"
#include <string.h>
#include <math.h>

// ============================================================
//  ZoneDetect binary file format (BertoldVdb/ZoneDetect, version 0)
//
//  Bytes 0-2   : "PLB" magic
//  Byte  3     : tableType
//  Byte  4     : version  (0 = supported)
//  Byte  5     : precision (e.g. 21 or 16)
//  Byte  6     : numFields
//  Then numFields ZDParseString field names
//  Then one ZDParseString copyright notice
//  Then three unsigned varints: s0, s1, s2  (section byte lengths)
//  bboxOffset  = <index after above>
//  metaOffset  = bboxOffset + s0
//  dataOffset  = metaOffset + s1
//  fileSize    = dataOffset + s2
//
//  Bbox section [bboxOffset, metaOffset):
//    Entries sorted ascending by bbMinLat. Per entry:
//      bbMinLat, bbMinLon, bbMaxLat, bbMaxLon : signed ZDDecodeUnsignedToSigned varints
//      metadataIndexDelta                     : signed ZDDecodeUnsignedToSigned varint
//      polygonIndexDelta                      : unsigned varint
//    Cumulative sums give byte offsets into metadata / data sections.
//
//  Signed varint encoding (ZDDecodeUnsignedToSigned):
//    Even value v → +(v/2)   (positive)
//    Odd  value v → -(v/2)   (negative, integer division)
//    Examples: 0→0, 2→1, 3→-1, 4→2, 5→-2, 6→3, 7→-3 …
//    Note: this is NOT standard ZigZag (which maps 1→-1); here 1→0 (unused code).
//
//  ZDParseString encoding:
//    - Read unsigned varint strLength
//    - If strLength >= 256 : back-reference; seek to metaOffset + strLength - 256,
//      read new strLength varint, read strLength bytes XOR 0x80
//    - Else : read strLength bytes XOR 0x80 from current position (advance pos)
//
//  Polygon data (version 0) at dataOffset + polygonIndex:
//    - numVertices (unsigned varint)
//    - numVertices pairs of (diffLat, diffLon) signed varints, delta-accumulated
//    - (0,0) pairs are degenerate and skipped
//    - Closing vertex (back to first) is implicit
//    - Winding +4 = excluded zone, -4 = included zone
// ============================================================

ZoneDetect::ZoneDetect()
    : _rdr(nullptr), _bboxOffset(0), _metaOffset(0), _dataOffset(0),
      _version(0), _precision(0), _numFields(0), _error("not initialised") {}

// ---- low-level I/O ----------------------------------------------------------

bool ZoneDetect::seekTo(uint32_t pos) { return _rdr->seek(pos); }

uint8_t ZoneDetect::rb() {
    uint8_t b = 0;
    _rdr->readBytes(&b, 1);
    return b;
}

// ---- varint helpers ---------------------------------------------------------

uint64_t ZoneDetect::readUV(uint32_t &pos)
{
    seekTo(pos);
    uint64_t v = 0;
    uint8_t  shift = 0, b;
    do {
        b = rb();
        pos++;
        v |= (uint64_t)(b & 0x7F) << shift;
        shift += 7;
    } while (b & 0x80);
    return v;
}

// ZDDecodeUnsignedToSigned: even → +(v/2), odd → -(v/2)  (integer division)
// This is NOT standard ZigZag. For v=3: -(3/2)=-1; for v=5: -(5/2)=-2. Etc.
int64_t ZoneDetect::readSV(uint32_t &pos)
{
    uint64_t u = readUV(pos);
    return (u & 1) ? -(int64_t)(u >> 1) : (int64_t)(u >> 1);
}

// ---- string parser ----------------------------------------------------------

bool ZoneDetect::parseString(uint32_t &pos, char *buf, size_t bufLen)
{
    uint64_t strLen = readUV(pos);

    uint32_t strPos;
    if (strLen >= 256) {
        // Back-reference: string data lives in the metadata section.
        strPos = _metaOffset + (uint32_t)(strLen - 256);
        seekTo(strPos);
        // Read actual length from the back-reference location.
        uint64_t refLen = 0;
        uint8_t  shift  = 0, b;
        do {
            b = rb(); strPos++;
            refLen |= (uint64_t)(b & 0x7F) << shift;
            shift += 7;
        } while (b & 0x80);
        strLen = refLen;
        // pos is NOT advanced past the string bytes (they live elsewhere).
    } else {
        strPos = pos;
        pos   += (uint32_t)strLen;
    }

    seekTo(strPos);
    size_t i = 0;
    for (uint64_t j = 0; j < strLen; j++) {
        uint8_t c = rb();
        if (i < bufLen - 1) buf[i++] = (char)(c ^ 0x80);
    }
    buf[i] = 0;
    return true;
}

// ---- coordinate conversion --------------------------------------------------

int32_t ZoneDetect::toFixed(float coord, float scale) const
{
    // ZDFloatToFixedPoint: coord/scale * (1 << (precision-1))
    return (int32_t)(coord / scale * (float)(1UL << (_precision - 1)));
}

// ---- initialisation ---------------------------------------------------------

bool ZoneDetect::begin(ZDReader *reader)
{
    _rdr   = reader;
    _error = nullptr;

    uint32_t fileSize = reader->fileSize();
    if (fileSize < 12) { _error = "file too small"; return false; }

    // Verify "PLB" magic
    seekTo(0);
    uint8_t magic[3];
    reader->readBytes(magic, 3);
    if (magic[0] != 'P' || magic[1] != 'L' || magic[2] != 'B') {
        _error = "bad magic (expected PLB)";
        return false;
    }

    // Fixed header: tableType, version, precision, numFields
    uint8_t hdr[4];
    reader->readBytes(hdr, 4);
    _version   = hdr[1];
    _precision = hdr[2];
    _numFields = hdr[3];
    if (_numFields > 6) _numFields = 6;

    if (_version != 0) {
        _error = "unsupported version (use files from database/out/, not out_v1/)";
        return false;
    }

    uint32_t pos = 7;  // after "PLB" + 4-byte fixed header

    // Parse field names
    for (uint8_t i = 0; i < _numFields; i++) {
        parseString(pos, _fieldName[i], sizeof(_fieldName[0]));
    }

    // Skip copyright notice
    {
        char tmp[4];
        parseString(pos, tmp, sizeof(tmp));
    }

    // Three section-size varints
    uint64_t s0 = readUV(pos);
    uint64_t s1 = readUV(pos);
    uint64_t s2 = readUV(pos);

    _bboxOffset = pos;
    _metaOffset = (uint32_t)(_bboxOffset + s0);
    _dataOffset = (uint32_t)(_metaOffset + s1);

    if (_metaOffset > fileSize || _dataOffset > fileSize) {
        _error = "bad offsets";
        return false;
    }
    if ((uint32_t)(_dataOffset + s2) != fileSize) {
        _error = "section sizes do not match file size";
        return false;
    }

    _error = nullptr;
    return true;
}

// ---- point-in-polygon (quadrant winding number) -----------------------------
// Replicates ZDPointInPolygon from BertoldVdb/ZoneDetect.
// Returns: 1=in zone, 2=in excluded zone, 0=not in zone, -1=parse error

static bool inRange(int32_t a, int32_t b, int32_t c)
{
    return (a <= b && b <= c) || (c <= b && b <= a);
}

int ZoneDetect::pointInPolygon(uint32_t polyBase, int32_t latFP, int32_t lonFP)
{
    uint32_t pos = polyBase;
    uint64_t numVerts = readUV(pos);
    if (numVerts == 0) return 0;

    int32_t curLat = 0, curLon = 0;
    int32_t firstLat = 0, firstLon = 0;
    int32_t prevLat  = 0, prevLon = 0;
    int     prevQuadrant = 0;
    int     winding = 0;
    bool    first   = true;

    // Iterate over all encoded vertices + the implicit closing vertex
    for (uint64_t v = 0; v <= numVerts; v++) {
        int32_t pointLat, pointLon;

        if (v < numVerts) {
            int32_t dLat = (int32_t)readSV(pos);
            int32_t dLon = (int32_t)readSV(pos);

            // Skip degenerate (zero-length) edges
            if (dLat == 0 && dLon == 0) continue;

            curLat += dLat;
            curLon += dLon;
            pointLat = curLat;
            pointLon = curLon;
            if (first) { firstLat = curLat; firstLon = curLon; }
        } else {
            // Closing vertex: implicit return to first point
            if (first) break;
            pointLat = firstLat;
            pointLon = firstLon;
        }

        // Exactly on vertex?
        if (pointLat == latFP && pointLon == lonFP) return 2;

        // Determine quadrant of current vertex relative to test point
        //   Q0: lat>=latFP, lon>=lonFP  (upper-right)
        //   Q1: lat>=latFP, lon< lonFP  (upper-left)
        //   Q2: lat< latFP, lon< lonFP  (lower-left)
        //   Q3: lat< latFP, lon>=lonFP  (lower-right)
        int quadrant;
        if (pointLat >= latFP) quadrant = (pointLon >= lonFP) ? 0 : 1;
        else                   quadrant = (pointLon >= lonFP) ? 3 : 2;

        if (!first) {
            bool lineIsStraight = (pointLon == prevLon || pointLat == prevLat);
            int  windingNeedCompare = 0;

            if (quadrant == prevQuadrant) {
                // Same quadrant – no contribution
            } else if (quadrant == (prevQuadrant + 1) % 4) {
                winding++;
            } else if ((quadrant + 1) % 4 == prevQuadrant) {
                winding--;
            } else {
                // Opposite quadrant (diagonal transition) – need line test
                windingNeedCompare = 1;
            }

            // Check for border crossing on straight (horizontal/vertical) segments
            if (lineIsStraight) {
                bool onSeg = inRange(pointLat, latFP, prevLat) &&
                             inRange(pointLon, lonFP, prevLon);
                if (onSeg || windingNeedCompare) return 2; // on border
            } else if (windingNeedCompare) {
                // Compute lon of the edge at lat = latFP
                float a = (float)(pointLat - prevLat) / (float)(pointLon - prevLon);
                float b = (float)pointLat - a * (float)pointLon;
                int32_t intersectLon = (int32_t)(((float)latFP - b) / a);

                if (intersectLon >= lonFP - 1 && intersectLon <= lonFP + 1) {
                    return 2; // on border
                }

                int sign = (intersectLon < lonFP) ? 2 : -2;
                if (quadrant == 2 || quadrant == 3) winding += sign;
                else                                winding -= sign;
            }
        }

        prevQuadrant = quadrant;
        prevLat      = pointLat;
        prevLon      = pointLon;
        first        = false;
    }

    // winding -4 = counter-clockwise (inclusive polygon)
    // winding +4 = clockwise (exclusion/hole polygon)
    if (winding == -4) return 1;
    if (winding ==  4) return 2;  // in excluded zone
    return 0;
}

// ---- metadata extraction ----------------------------------------------------

bool ZoneDetect::fillMatch(uint32_t metaIndex, ZDMatch &m)
{
    m.tzId[0]      = 0;
    m.countryA2[0] = 0;

    uint32_t pos = _metaOffset + metaIndex;
    char prefix[32] = "";

    for (uint8_t f = 0; f < _numFields; f++) {
        char val[64];
        parseString(pos, val, sizeof(val));

        const char *name = _fieldName[f];

        if (strcmp(name, "TimezoneIdPrefix") == 0) {
            strncpy(prefix, val, sizeof(prefix) - 1);
            prefix[sizeof(prefix) - 1] = 0;

        } else if (strcmp(name, "TimezoneId") == 0 ||
                   strcmp(name, "timezone_id") == 0) {
            // Full ID = prefix + suffix
            size_t plen = strlen(prefix);
            strncpy(m.tzId, prefix, sizeof(m.tzId) - 1);
            strncpy(m.tzId + plen, val, sizeof(m.tzId) - 1 - plen);
            m.tzId[sizeof(m.tzId) - 1] = 0;

        } else if (strcmp(name, "CountryAlpha2") == 0 ||
                   strcmp(name, "country_a2")    == 0) {
            strncpy(m.countryA2, val, sizeof(m.countryA2) - 1);
            m.countryA2[sizeof(m.countryA2) - 1] = 0;
        }
    }

    return m.tzId[0] != 0;
}

// ---- main lookup ------------------------------------------------------------

int ZoneDetect::lookup(float lat, float lon, ZDMatch *out, uint8_t maxResults)
{
    if (!_rdr || _error) return -1;

    int32_t latFP = toFixed(lat,  90.0f);
    int32_t lonFP = toFixed(lon, 180.0f);

    uint32_t pos      = _bboxOffset;
    int64_t  metaIdx  = 0;
    int64_t  polyIdx  = 0;

    // Per-metaId accumulator (inside polygon count minus excluded zone count).
    // A point is inside a timezone when its net count is positive.
    struct { uint32_t id; int sum; } acc[8];
    int nAcc = 0;

    while (pos < _metaOffset) {
        int64_t  bbMinLat  = readSV(pos);
        int64_t  bbMinLon  = readSV(pos);
        int64_t  bbMaxLat  = readSV(pos);
        int64_t  bbMaxLon  = readSV(pos);
        int64_t  metaDelta = readSV(pos);
        uint64_t polyDelta = readUV(pos);

        metaIdx += metaDelta;
        polyIdx += (int64_t)polyDelta;

        // Bbox section is sorted ascending by bbMinLat → early exit
        if ((int64_t)latFP < bbMinLat) break;

        // Bounding-box reject
        if ((int64_t)latFP > bbMaxLat) continue;
        if ((int64_t)lonFP < bbMinLon || (int64_t)lonFP > bbMaxLon) continue;

        uint32_t polyBase = _dataOffset + (uint32_t)polyIdx;
        if (polyBase >= _rdr->fileSize()) { _error = "polygon offset out of range"; return -1; }

        int pip = pointInPolygon(polyBase, latFP, lonFP);
        if (pip < 0) { _error = "polygon parse error"; return -1; }

        if (pip > 0) {
            uint32_t mId = (uint32_t)metaIdx;
            int delta = (pip == 1) ? 1 : -1;   // 1=inside, 2=excluded→subtract

            bool found = false;
            for (int i = 0; i < nAcc; i++) {
                if (acc[i].id == mId) { acc[i].sum += delta; found = true; break; }
            }
            if (!found && nAcc < (int)(sizeof(acc)/sizeof(acc[0]))) {
                acc[nAcc++] = { mId, delta };
            }
        }
    }

    // Collect results: metaIds whose net count is positive
    int found = 0;
    for (int i = 0; i < nAcc && found < maxResults; i++) {
        if (acc[i].sum > 0) {
            ZDMatch m;
            if (fillMatch(acc[i].id, m)) {
                out[found++] = m;
            }
        }
    }

    return found;
}

bool ZoneDetect::getTimezoneId(float lat, float lon, char *tzId, size_t bufLen)
{
    ZDMatch m;
    int n = lookup(lat, lon, &m, 1);
    if (n <= 0) return false;
    strncpy(tzId, m.tzId, bufLen - 1);
    tzId[bufLen - 1] = 0;
    return true;
}
