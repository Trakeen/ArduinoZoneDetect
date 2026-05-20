#pragma once
#include <Arduino.h>
#include "ZoneDetect.h"  // for ZDReader

// -------------------------------------------------------------------------
// DST – Daylight Saving Time offset calculator
//
// Uses a compact binary file (dst_rules.bin) that maps IANA timezone IDs
// to POSIX TZ strings (e.g. "CET-1CEST,M3.5.0,M10.5.0/3").
// The library then parses those strings to compute the UTC offset for
// any given Unix timestamp.
//
// File format (dst_rules.bin):
//   4 bytes  – magic "DSTR"
//   2 bytes  – uint16 LE – number of entries N
//   per entry (N times):
//     uint8  – length of tzId string (not null-terminated in file)
//     char[] – tzId bytes
//     uint8  – length of POSIX TZ string
//     char[] – POSIX TZ bytes
//
// UTC offset range: UTC-12 … UTC+14  →  -43 200 … +50 400 seconds.
// These values exceed int16_t (±32 767), so int32_t is the smallest
// standard type that covers all legal timezone offsets in seconds.
//
// RTClib compatibility:
//   uint32_t local = dst.toLocalTime(tzId, utcUnix);
//   if (local) { DateTime dt(local); /* use dt */ }
// -------------------------------------------------------------------------

struct DSTEntry {
    char tzId[48];
    char posixTz[64];
};

class DST {
public:
    DST();

    // Load the dst_rules.bin file via a ZDReader.
    // Returns false on bad magic or read error.
    bool begin(ZDReader *reader);

    // Find the POSIX TZ string for a given IANA timezone ID.
    // Returns false if not found.
    bool getPosixTz(const char *tzId, char *posixTz, size_t bufLen);

    // Compute the UTC offset in seconds for tzId at the given UTC Unix
    // timestamp utcUnix.  Returns true and writes to offsetSec on success.
    // Returns false if tzId is not found in dst_rules.bin.
    // Typical values: +3600 (UTC+1 winter), +7200 (UTC+2 summer), -18000 (UTC-5), …
    bool getUtcOffset(const char *tzId, uint32_t utcUnix, int32_t &offsetSec);

    // Compute the local Unix timestamp (= utcUnix + UTC offset).
    // Returns 0 on error (tzId not found or not loaded).
    // Compatible with RTClib: DateTime dt(dst.toLocalTime(tzId, utcUnix));
    uint32_t toLocalTime(const char *tzId, uint32_t utcUnix);

    // Parse a POSIX TZ string directly and return the UTC offset in seconds.
    // Can be called without begin() if the POSIX TZ string is already known.
    static int32_t parsePosixTz(const char *posixTz, uint32_t utcUnix);

    // Calendar helpers (public so free functions in DST.cpp can call them)
    static bool isLeapYear(int y);
    static int  daysInMonth(int m, int y);
    static int  dayOfYear(int m, int d, int y);
    static int  weekdayOfJan1(int y);

    const char *lastError() const { return _error; }

private:
    ZDReader   *_rdr;
    uint16_t    _count;
    uint32_t    _dataStart;
    const char *_error;

    bool readEntry(uint16_t idx, DSTEntry &e);

    static int32_t parseOffset(const char *&p);
    static bool    parseTransitionRule(const char *&p, int year,
                                       int &monthOut, int &dayOut,
                                       int32_t &secOut);
};
