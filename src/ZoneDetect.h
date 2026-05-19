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
// ZoneDetect – reads timezone21.bin or timezone16.bin (version 0) from a
// ZDReader.  Use the files from the BertoldVdb/ZoneDetect database/out/
// directory (not out_v1 – version 1 polygon compression is not supported).
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
    uint32_t    _bboxOffset;   // start of bbox (sorted polygon index) section
    uint32_t    _metaOffset;   // start of metadata section
    uint32_t    _dataOffset;   // start of polygon vertex data section
    uint8_t     _version;
    uint8_t     _precision;
    uint8_t     _numFields;
    char        _fieldName[6][24];
    const char *_error;

    // ---- low-level I/O --------------------------------------------------
    bool    seekTo(uint32_t pos);
    uint8_t rb();                          // read one byte at current position

    // ---- varint coding (LEB128) -----------------------------------------
    uint64_t readUV(uint32_t &pos);        // unsigned varint, advances pos
    int64_t  readSV(uint32_t &pos);        // ZigZag signed varint, advances pos

    // ---- string (ZDParseString format) ----------------------------------
    // Reads length-prefixed XOR-0x80 string; handles back-references.
    bool parseString(uint32_t &pos, char *buf, size_t bufLen);

    // ---- fixed-point conversion -----------------------------------------
    // scale = 90.0 for latitude, 180.0 for longitude
    int32_t toFixed(float coord, float scale) const;

    // ---- polygon engine -------------------------------------------------
    // Returns: 1 = inside, 0 = outside, 2 = on border
    int pointInPolygon(uint32_t polyBase, int32_t latFP, int32_t lonFP);

    // Fill one ZDMatch from the metadata at byte offset metaIndex
    bool fillMatch(uint32_t metaIndex, ZDMatch &m);
};
