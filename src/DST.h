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
// -------------------------------------------------------------------------

struct DSTEntry {
    char tzId[48];
    char posixTz[64];
};

class DST {
public:
    DST();

    // Load the dst_rules.bin file via a ZDReader.
    // Returns false on bad magic / read error.
    bool begin(ZDReader *reader);

    // Find the POSIX TZ string for a given IANA timezone ID.
    // Returns false if not found.
    bool getPosixTz(const char *tzId, char *posixTz, size_t bufLen);

    // Compute the UTC offset in seconds for a given tzId at Unix timestamp t.
    // t must be UTC Unix time (seconds since 1970-01-01T00:00:00Z).
    // Returns INT32_MIN on error.
    int32_t getUtcOffset(const char *tzId, int32_t t);

    // Convenience: get local Unix time (= t + utcOffset) for tzId.
    // Returns INT32_MIN on error.
    int32_t toLocalTime(const char *tzId, int32_t t);

    // Parse a POSIX TZ string and return the UTC offset in seconds for
    // Unix timestamp t.  Can be called without begin() if you already
    // have the POSIX TZ string.
    static int32_t parsePosixTz(const char *posixTz, int32_t t);

    // These helpers are public so free functions in DST.cpp can call them
    static bool    isLeapYear(int y);
    static int     daysInMonth(int m, int y);
    static int     dayOfYear(int m, int d, int y);
    static int     weekdayOfJan1(int y);

private:
    ZDReader *_rdr;
    uint16_t  _count;
    uint32_t  _dataStart; // offset in file where entry data begins
    const char *_error;

    // Locate the file offset for entry i and read it
    bool readEntry(uint16_t idx, DSTEntry &e);

    // POSIX TZ string parser helpers
    static int32_t parseOffset(const char *&p);
    static bool    parseTransitionRule(const char *&p, int year,
                                       int &monthOut, int &dayOut, int32_t &secOut);
};
