#include "ZoneDetect.h"
#include <string.h>

// ============================================================
//  ZoneDetect binary file format (BertoldVdb/ZoneDetect, version 0)
//
//  Bytes 0-2   : "PLB" magic
//  Byte  3     : tableType
//  Byte  4     : version  (0 = supported; 1 = unsupported)
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
//      bbMinLat, bbMinLon, bbMaxLat, bbMaxLon : signed ZigZag varints
//      metadataIndexDelta                     : signed ZigZag varint
//      polygonIndexDelta                      : unsigned varint
//    Cumulative sums give byte offsets into metadata / data sections.
//
//  ZDParseString encoding:
//    - Read unsigned varint strLength
//    - If strLength >= 256 : back-reference; seek to metaOffset + strLength - 256,
//      read new strLength varint, read strLength bytes XOR 0x80
//    - Else : read strLength bytes XOR 0x80 from current position (advance pos)
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

// ZigZag decode: 0→0, 1→-1, 2→1, 3→-2, 4→2, …
int64_t ZoneDetect::readSV(uint32_t &pos)
{
    uint64_t u = readUV(pos);
    return (u & 1) ? -(int64_t)(u >> 1) - 1 : (int64_t)(u >> 1);
}

// ---- string parser ----------------------------------------------------------

bool ZoneDetect::parseString(uint32_t &pos, char *buf, size_t bufLen)
{
    uint64_t strLen = readUV(pos);

    uint32_t strPos;
    if (strLen >= 256) {
        // Back-reference: string bytes live in the metadata section.
        strPos = _metaOffset + (uint32_t)(strLen - 256);
        seekTo(strPos);
        // Read the actual length from the back-reference location.
        uint64_t refLen = 0;
        uint8_t  shift  = 0, b;
        do {
            b = rb(); strPos++;
            refLen |= (uint64_t)(b & 0x7F) << shift;
            shift += 7;
        } while (b & 0x80);
        strLen = refLen;
        // pos is NOT advanced past string bytes; they live elsewhere.
    } else {
        strPos = pos;
        pos   += (uint32_t)strLen;   // advance caller's position past string
    }

    // Copy strLen XOR-0x80 bytes into buf
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

    // Parse numFields field names (ZDParseString each)
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

// ---- point-in-polygon (winding number) --------------------------------------

int ZoneDetect::pointInPolygon(uint32_t polyBase, int32_t latFP, int32_t lonFP)
{
    uint32_t pos = polyBase;
    uint64_t numVerts = readUV(pos);
    if (numVerts == 0) return 0;

    int64_t curLat = 0, curLon = 0;
    int64_t prevLat = 0, prevLon = 0;
    int     winding = 0;

    for (uint64_t v = 0; v < numVerts; v++) {
        int64_t dLat = readSV(pos);
        int64_t dLon = readSV(pos);

        prevLat = curLat;
        prevLon = curLon;
        curLat += dLat;
        curLon += dLon;

        if (v == 0) {
            // First vertex is absolute; no edge to test yet.
            prevLat = curLat;
            prevLon = curLon;
            continue;
        }

        // Winding-number edge contribution
        if (prevLat <= (int64_t)latFP && curLat > (int64_t)latFP) {
            // Upward crossing
            int64_t cross = (curLon - prevLon) * ((int64_t)latFP - prevLat)
                          - ((int64_t)lonFP   - prevLon) * (curLat - prevLat);
            if (cross > 0)      winding += 2;
            else if (cross == 0) return 2;   // on edge
        } else if (curLat <= (int64_t)latFP && prevLat > (int64_t)latFP) {
            // Downward crossing
            int64_t cross = (curLon - prevLon) * ((int64_t)latFP - prevLat)
                          - ((int64_t)lonFP   - prevLon) * (curLat - prevLat);
            if (cross < 0)      winding -= 2;
            else if (cross == 0) return 2;   // on edge
        }
    }

    return (winding != 0) ? 1 : 0;
}

// ---- metadata extraction ----------------------------------------------------

bool ZoneDetect::fillMatch(uint32_t metaIndex, ZDMatch &m)
{
    m.tzId[0]      = 0;
    m.countryA2[0] = 0;

    uint32_t pos = _metaOffset + metaIndex;

    for (uint8_t f = 0; f < _numFields; f++) {
        char val[64];
        parseString(pos, val, sizeof(val));

        const char *name = _fieldName[f];
        if (strcmp(name, "TimezoneId") == 0 || strcmp(name, "timezone_id") == 0) {
            strncpy(m.tzId, val, sizeof(m.tzId) - 1);
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
    int64_t  metaIdx  = 0;   // cumulative byte offset into metadata section
    int64_t  polyIdx  = 0;   // cumulative byte offset into polygon data section
    int      found    = 0;

    while (found < maxResults && pos < _metaOffset) {
        int64_t  bbMinLat  = readSV(pos);
        int64_t  bbMinLon  = readSV(pos);
        int64_t  bbMaxLat  = readSV(pos);
        int64_t  bbMaxLon  = readSV(pos);
        int64_t  metaDelta = readSV(pos);
        uint64_t polyDelta = readUV(pos);

        metaIdx += metaDelta;
        polyIdx += (int64_t)polyDelta;

        // Bbox section is sorted ascending by bbMinLat; early exit is safe.
        if ((int64_t)latFP < bbMinLat) break;

        // Bounding-box reject
        if ((int64_t)latFP > bbMaxLat) continue;
        if ((int64_t)lonFP < bbMinLon || (int64_t)lonFP > bbMaxLon) continue;

        // Full point-in-polygon test
        uint32_t polyBase = _dataOffset + (uint32_t)polyIdx;
        if (polyBase >= _rdr->fileSize()) { _error = "polygon offset out of range"; return -1; }

        int pip = pointInPolygon(polyBase, latFP, lonFP);

        if (pip > 0) {
            ZDMatch m;
            if (fillMatch((uint32_t)metaIdx, m)) {
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
