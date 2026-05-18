#pragma once
#include <Arduino.h>

// -------------------------------------------------------------------------
// Abstract seekable reader – wrap SD File or any other source
// -------------------------------------------------------------------------
class ZDReader {
public:
    virtual ~ZDReader() {}
    virtual bool     seek(uint32_t pos)                    = 0;
    virtual uint32_t position()                             = 0;
    virtual size_t   readBytes(uint8_t *buf, size_t len)   = 0;
    virtual uint32_t fileSize()                             = 0;
};

// -------------------------------------------------------------------------
// Convenience wrapper for the SD library's File (or any class with seek /
// read / position / size members)
// -------------------------------------------------------------------------
template<typename TFile>
class ZDFileReader : public ZDReader {
public:
    explicit ZDFileReader(TFile &f) : _f(f) {}
    bool     seek(uint32_t pos)                  override { return _f.seek(pos); }
    uint32_t position()                          override { return (uint32_t)_f.position(); }
    size_t   readBytes(uint8_t *buf, size_t len) override { return _f.read(buf, len); }
    uint32_t fileSize()                          override { return (uint32_t)_f.size(); }
private:
    TFile &_f;
};

// -------------------------------------------------------------------------
// Result of a single zone match
// -------------------------------------------------------------------------
struct ZDMatch {
    char tzId[48];        // IANA timezone ID, e.g. "Europe/Paris"
    char countryA2[3];    // ISO-3166 alpha-2, e.g. "FR", or "" if absent
};

// -------------------------------------------------------------------------
// ZoneDetect – reads timezone21.bin or timezone16.bin from a ZDReader
// -------------------------------------------------------------------------
class ZoneDetect {
public:
    ZoneDetect();

    // Call once after opening the file. Returns false on header error.
    bool begin(ZDReader *reader);

    // Look up timezone(s) at the given WGS-84 coordinates (degrees).
    // Fills up to maxResults entries in 'out'.
    // Returns the number of results (0 = ocean / no zone, -1 = parse error).
    int lookup(float lat, float lon, ZDMatch *out, uint8_t maxResults = 1);

    // Convenience: fills tzId only; returns false if not found.
    bool getTimezoneId(float lat, float lon, char *tzId, size_t bufLen);

    const char *lastError() const { return _error; }

private:
    ZDReader   *_rdr;
    uint32_t    _tableEnd;
    uint32_t    _metaOffset;
    uint32_t    _dataOffset;
    uint8_t     _precision;
    uint8_t     _numFields;
    char        _fieldName[6][24];  // up to 6 field names
    const char *_error;

    // ---- low-level I/O --------------------------------------------------
    bool    seekTo(uint32_t pos);
    uint8_t  rb();                      // read one byte (returns 0xFF on fail)
    uint32_t readU32(uint32_t pos);
    uint8_t  readU8(uint32_t pos);

    // ---- varint coding (ZoneDetect wire format) -------------------------
    uint64_t readVarUint(uint32_t &pos);
    int64_t  readVarInt(uint32_t &pos);

    // ---- string (null-terminated) at position pos -----------------------
    bool readStr(uint32_t &pos, char *buf, size_t bufLen);

    // ---- fixed-point conversion -----------------------------------------
    int32_t toFixed(float deg) const;

    // ---- polygon engine -------------------------------------------------
    // Returns: 1 = inside, 0 = outside, 2 = on border, -1 = parse error
    int  pointInPolygon(uint32_t polyBase, int32_t latFP, int32_t lonFP,
                        uint32_t &nextPoly);

    // Fill one ZDMatch from the metadata for a matched polygon
    bool fillMatch(uint32_t metaRef, ZDMatch &m);
};
