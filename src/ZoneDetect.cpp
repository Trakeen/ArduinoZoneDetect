#include "ZoneDetect.h"
#include <string.h>

// ============================================================
//  ZoneDetect binary file format (BertoldVdb/ZoneDetect)
//
//  Offset 0   : uint32 LE – tableEnd    (end of lookup table)
//  Offset 4   : uint32 LE – metaOffset  (start of metadata section)
//  Offset 8   : uint32 LE – dataOffset  (start of polygon data)
//  Offset 12+ : lookup table  [tableEnd-12 bytes, array of uint32]
//  dataOffset : polygon records (variable-length encoded)
//  metaOffset : field-name list, then string pool
// ============================================================

ZoneDetect::ZoneDetect()
    : _rdr(nullptr), _tableEnd(0), _metaOffset(0), _dataOffset(0),
      _precision(0), _numFields(0), _error("not initialised") {}

// ---- helpers ----------------------------------------------------------------

bool ZoneDetect::seekTo(uint32_t pos)
{
    return _rdr->seek(pos);
}

uint8_t ZoneDetect::rb()
{
    uint8_t b = 0;
    _rdr->readBytes(&b, 1);
    return b;
}

uint32_t ZoneDetect::readU32(uint32_t pos)
{
    seekTo(pos);
    uint8_t buf[4];
    if (_rdr->readBytes(buf, 4) != 4) return 0;
    return (uint32_t)buf[0] | ((uint32_t)buf[1] << 8) |
           ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);
}

uint8_t ZoneDetect::readU8(uint32_t pos)
{
    seekTo(pos);
    return rb();
}

uint64_t ZoneDetect::readVarUint(uint32_t &pos)
{
    seekTo(pos);
    uint64_t result = 0;
    uint8_t  shift  = 0;
    uint8_t  b;
    do {
        b = rb();
        pos++;
        result |= (uint64_t)(b & 0x7F) << shift;
        shift  += 7;
    } while (b & 0x80);
    return result;
}

int64_t ZoneDetect::readVarInt(uint32_t &pos)
{
    uint64_t u = readVarUint(pos);
    // ZigZag decode
    return (int64_t)((u >> 1) ^ (-(int64_t)(u & 1)));
}

bool ZoneDetect::readStr(uint32_t &pos, char *buf, size_t bufLen)
{
    seekTo(pos);
    size_t i = 0;
    while (i < bufLen - 1) {
        uint8_t c = rb();
        pos++;
        buf[i++] = (char)c;
        if (c == 0) return true;
    }
    buf[i] = 0;
    // skip rest
    while (rb() != 0) pos++;
    pos++;
    return true;
}

int32_t ZoneDetect::toFixed(float deg) const
{
    // Map [-90,90] or [-180,180] to fixed-point with _precision bits
    // latFP = lat_deg * 2^precision / 90   (for lat)
    // lonFP = lon_deg * 2^precision / 180  (for lon, same scale)
    // The file uses lat/lon in the same coordinate space:
    //   value = degrees * (1<<precision) / 90
    return (int32_t)(deg * (float)(1UL << _precision) / 90.0f);
}

// ---- initialisation ---------------------------------------------------------

bool ZoneDetect::begin(ZDReader *reader)
{
    _rdr   = reader;
    _error = nullptr;

    _tableEnd   = readU32(0);
    _metaOffset = readU32(4);
    _dataOffset = readU32(8);

    if (_tableEnd < 12 || _metaOffset < _tableEnd || _dataOffset < _metaOffset) {
        _error = "bad header";
        return false;
    }

    // Read field names from metadata section
    uint32_t pos = _metaOffset;
    _numFields = 0;
    while (_numFields < 6) {
        char name[24];
        readStr(pos, name, sizeof(name));
        if (name[0] == 0) break;
        strncpy(_fieldName[_numFields], name, sizeof(_fieldName[0]) - 1);
        _numFields++;
        // skip type char (one more null-term string)
        char type[4];
        readStr(pos, type, sizeof(type));
    }

    // Determine precision from file size / data encoding
    // timezone16.bin uses 16-bit, timezone21.bin uses 21-bit.
    // The precision is stored implicitly via the table size heuristic:
    // numEntries = (tableEnd - 12) / 4 = 2 * 2^precision / (unit)
    // In practice: 16-bit file has 512 table entries (2*256), 21-bit has 16384.
    uint32_t numTableEntries = (_tableEnd - 12) / 4;
    if (numTableEntries <= 512) {
        _precision = 16;
    } else {
        _precision = 21;
    }

    _error = nullptr;
    return true;
}

// ---- point-in-polygon -------------------------------------------------------

// Returns 1=inside, 0=outside, 2=on-border, -1=error
// Updates nextPoly to the byte offset right after this polygon record.
int ZoneDetect::pointInPolygon(uint32_t polyBase, int32_t latFP, int32_t lonFP,
                                uint32_t &nextPoly)
{
    uint32_t pos = polyBase;

    // Polygon header: metaRef (varuint) + bounding-box (4 varints)
    uint64_t metaRef = readVarUint(pos); (void)metaRef;

    int64_t bbLatMin = readVarInt(pos);
    int64_t bbLatMax = bbLatMin + (int64_t)readVarUint(pos);
    int64_t bbLonMin = readVarInt(pos);
    int64_t bbLonMax = bbLonMin + (int64_t)readVarUint(pos);

    uint64_t numVerts = readVarUint(pos);

    // Quick bounding-box reject
    bool inBB = (latFP >= bbLatMin && latFP <= bbLatMax &&
                 lonFP >= bbLonMin && lonFP <= bbLonMax);

    int result = 0;  // outside

    if (inBB) {
        // Ray-casting: count crossings of horizontal ray at lat = latFP
        int64_t prevLat = 0, prevLon = 0;
        int64_t curLat  = 0, curLon  = 0;
        bool onBorder = false;
        int crossings = 0;

        for (uint64_t v = 0; v < numVerts; v++) {
            int64_t dLat = readVarInt(pos);
            int64_t dLon = readVarInt(pos);

            prevLat = curLat;
            prevLon = curLon;
            curLat += dLat;
            curLon += dLon;

            if (v == 0) { prevLat = curLat; prevLon = curLon; continue; }

            // Check if point is exactly on this segment's vertex
            if (curLat == latFP && curLon == lonFP) { onBorder = true; break; }

            // Check crossing
            if ((curLat > latFP) != (prevLat > latFP)) {
                // compute lon of crossing
                int64_t crossLon = prevLon +
                    (int64_t)((int64_t)(latFP - prevLat) * (curLon - prevLon) /
                               (curLat - prevLat));
                if (crossLon == lonFP) { onBorder = true; break; }
                if (crossLon > lonFP)  crossings++;
            }
        }

        if (onBorder) {
            result = 2;
        } else {
            result = (crossings & 1) ? 1 : 0;
        }
    } else {
        // Still need to skip vertex data
        for (uint64_t v = 0; v < numVerts; v++) {
            readVarInt(pos);
            readVarInt(pos);
        }
    }

    nextPoly = pos;
    return result;
}

// ---- metadata extraction ----------------------------------------------------

bool ZoneDetect::fillMatch(uint32_t metaRef, ZDMatch &m)
{
    m.tzId[0]    = 0;
    m.countryA2[0] = 0;

    // metaRef is an offset from metaOffset+fieldListSize into the string pool.
    // In the actual file, metadata records are stored as sequential var-length
    // field values right after the field name list.
    // Each record: one string per field, in field-name order.

    uint32_t pos = _metaOffset;

    // Skip field name list to find start of string pool
    for (uint8_t i = 0; i < _numFields; i++) {
        char tmp[24];
        readStr(pos, tmp, sizeof(tmp));  // name
        readStr(pos, tmp, sizeof(tmp));  // type
    }
    pos++; // skip final empty name

    // Skip 'metaRef' records to reach the one we want
    for (uint32_t r = 0; r < metaRef; r++) {
        for (uint8_t f = 0; f < _numFields; f++) {
            char tmp[64];
            readStr(pos, tmp, sizeof(tmp));
        }
    }

    // Read this record's fields
    for (uint8_t f = 0; f < _numFields; f++) {
        char val[64];
        readStr(pos, val, sizeof(val));

        const char *name = _fieldName[f];
        if (strcmp(name, "TimezoneId") == 0 || strcmp(name, "timezone_id") == 0) {
            strncpy(m.tzId, val, sizeof(m.tzId) - 1);
        } else if (strcmp(name, "CountryAlpha2") == 0 ||
                   strcmp(name, "country_a2") == 0) {
            strncpy(m.countryA2, val, sizeof(m.countryA2) - 1);
        }
    }

    return m.tzId[0] != 0;
}

// ---- main lookup ------------------------------------------------------------

int ZoneDetect::lookup(float lat, float lon, ZDMatch *out, uint8_t maxResults)
{
    if (!_rdr || _error) return -1;

    int32_t latFP = toFixed(lat);
    int32_t lonFP = toFixed(lon);

    uint32_t numTableEntries = (_tableEnd - 12) / 4;
    if (numTableEntries == 0) { _error = "empty table"; return -1; }

    // Map lat to table index
    // latFP range: [-2^precision, +2^precision]  for [-90,+90]
    // table index: linearly mapped to [0, numTableEntries)
    int32_t maxFP = (int32_t)(1UL << _precision);
    int64_t idx64 = ((int64_t)(latFP + maxFP) * numTableEntries) / ((int64_t)maxFP * 2);
    if (idx64 < 0) idx64 = 0;
    if (idx64 >= (int64_t)numTableEntries) idx64 = (int64_t)numTableEntries - 1;

    uint32_t tableIdx = (uint32_t)idx64;

    // The table entry at tableIdx gives the start of the polygon list for this lat band.
    // We search downward from that entry until we get an end-marker or find our match.
    uint32_t dataStart = readU32(12 + tableIdx * 4);
    if (dataStart == 0) return 0;  // empty band

    uint32_t pos = _dataOffset + dataStart;
    int found = 0;

    while (found < maxResults) {
        if (pos >= _rdr->fileSize()) break;

        // Each polygon list entry: type byte, then polygon data
        // type 0 = end of band list
        // type 1 = normal polygon
        // type 2 = excluded polygon (hole)
        seekTo(pos);
        uint8_t type = rb();
        pos++;

        if (type == 0) break;  // end of list for this band

        uint64_t metaRef = readVarUint(pos);

        uint32_t nextPoly = pos;
        int pip = pointInPolygon(pos, latFP, lonFP, nextPoly);
        pos = nextPoly;

        if (pip < 0) { _error = "polygon parse error"; return -1; }

        if (pip > 0) {
            if (type == 1) {
                // Inside a timezone polygon – fetch metadata
                ZDMatch m;
                if (fillMatch((uint32_t)metaRef, m)) {
                    out[found++] = m;
                }
            }
            // type == 2 would be an exclusion zone; we skip it here
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
