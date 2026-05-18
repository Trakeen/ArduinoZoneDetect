#include "DST.h"
#include <string.h>
#include <limits.h>

// ============================================================
//  POSIX TZ string format:
//    stdoffset[dst[offset][,start[/time],end[/time]]]
//
//  offset  ::= [+|-]hh[:mm[:ss]]
//  start/end ::= Jn | n | Mm.w.d
//    Jn  = Julian day 1..365 (no leap)
//    n   = Julian day 0..365 (with leap)
//    Mm.w.d = month m, week w, weekday d
//  time ::= hh[:mm[:ss]]   (default 02:00:00)
// ============================================================

DST::DST() : _rdr(nullptr), _count(0), _dataStart(0), _error("not initialised") {}

// ---- file reader helpers ----------------------------------------------------

static uint8_t readByte(ZDReader *r) {
    uint8_t b = 0; r->readBytes(&b, 1); return b;
}
static uint16_t readU16(ZDReader *r, uint32_t pos) {
    r->seek(pos);
    uint8_t buf[2]; r->readBytes(buf, 2);
    return (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
}

bool DST::begin(ZDReader *reader)
{
    _rdr = reader;
    _error = nullptr;

    uint8_t magic[4];
    reader->seek(0);
    if (reader->readBytes(magic, 4) != 4 ||
        magic[0] != 'D' || magic[1] != 'S' || magic[2] != 'T' || magic[3] != 'R') {
        _error = "bad magic";
        return false;
    }
    _count     = readU16(reader, 4);
    _dataStart = 6;
    _error     = nullptr;
    return true;
}

bool DST::readEntry(uint16_t idx, DSTEntry &e)
{
    if (!_rdr || idx >= _count) return false;

    uint32_t pos = _dataStart;
    _rdr->seek(pos);

    for (uint16_t i = 0; i <= idx; i++) {
        uint8_t tzLen  = readByte(_rdr);  pos++;
        uint8_t ptzLen = 0;

        if (i == idx) {
            if (tzLen >= sizeof(e.tzId)) tzLen = sizeof(e.tzId) - 1;
            _rdr->readBytes((uint8_t *)e.tzId, tzLen);
            e.tzId[tzLen] = 0;
            pos += tzLen;
            _rdr->seek(pos);
            ptzLen = readByte(_rdr); pos++;
            if (ptzLen >= sizeof(e.posixTz)) ptzLen = sizeof(e.posixTz) - 1;
            _rdr->readBytes((uint8_t *)e.posixTz, ptzLen);
            e.posixTz[ptzLen] = 0;
            return true;
        } else {
            // skip tzId
            pos += tzLen;
            _rdr->seek(pos);
            ptzLen = readByte(_rdr); pos++;
            pos   += ptzLen;
            _rdr->seek(pos);
        }
    }
    return false;
}

bool DST::getPosixTz(const char *tzId, char *posixTz, size_t bufLen)
{
    if (!_rdr) return false;
    DSTEntry e;
    for (uint16_t i = 0; i < _count; i++) {
        if (!readEntry(i, e)) continue;
        if (strcmp(e.tzId, tzId) == 0) {
            strncpy(posixTz, e.posixTz, bufLen - 1);
            posixTz[bufLen - 1] = 0;
            return true;
        }
    }
    return false;
}

int32_t DST::getUtcOffset(const char *tzId, int32_t t)
{
    char posixTz[64];
    if (!getPosixTz(tzId, posixTz, sizeof(posixTz))) return INT32_MIN;
    return parsePosixTz(posixTz, t);
}

int32_t DST::toLocalTime(const char *tzId, int32_t t)
{
    int32_t off = getUtcOffset(tzId, t);
    if (off == INT32_MIN) return INT32_MIN;
    return t + off;
}

// ============================================================
//  POSIX TZ string parser
// ============================================================

bool DST::isLeapYear(int y) {
    return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
}

int DST::daysInMonth(int m, int y) {
    static const int8_t d[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    if (m == 2 && isLeapYear(y)) return 29;
    return d[m - 1];
}

int DST::dayOfYear(int m, int d, int y) {
    int doy = 0;
    for (int i = 1; i < m; i++) doy += daysInMonth(i, y);
    return doy + d;  // 1-based
}

// Zeller-style: weekday of Jan 1 for year y (0=Sun)
int DST::weekdayOfJan1(int y) {
    int d = 1, m = 1;
    int yy = (m < 3) ? y - 1 : y;
    int k  = yy % 100, j = yy / 100;
    int h  = (d + (13 * (((m < 3) ? m + 12 : m) + 1)) / 5 + k + k/4 + j/4 - 2*j) % 7;
    return (h + 5) % 7;  // 0=Sun
}

// Skip std/dst name (letters or <...>)
static void skipName(const char *&p) {
    if (*p == '<') {
        while (*p && *p != '>') p++;
        if (*p) p++;
    } else {
        while (*p && (*p < '0' || *p > '9') && *p != ',' && *p != '+' && *p != '-') p++;
    }
}

// Parse [+|-]hh[:mm[:ss]]  – returns seconds (positive = east of UTC)
// The POSIX convention is std-offset, so "CET-1" means UTC+1 (west negative)
int32_t DST::parseOffset(const char *&p)
{
    int sign = 1;
    if (*p == '-') { sign = -1; p++; }
    else if (*p == '+') { p++; }

    int32_t h = 0, m = 0, s = 0;
    while (*p >= '0' && *p <= '9') h = h * 10 + (*p++ - '0');
    if (*p == ':') { p++; while (*p >= '0' && *p <= '9') m = m * 10 + (*p++ - '0'); }
    if (*p == ':') { p++; while (*p >= '0' && *p <= '9') s = s * 10 + (*p++ - '0'); }
    return sign * (h * 3600 + m * 60 + s);
}

// Convert year y and second-of-year (0-based) to Unix timestamp
static int32_t yearSecToUnix(int y, int32_t secOfYear) {
    // Days from epoch (1970) to Jan 1 of year y
    int32_t days = 0;
    if (y >= 1970) {
        for (int yy = 1970; yy < y; yy++)
            days += DST::isLeapYear(yy) ? 366 : 365;
    } else {
        for (int yy = y; yy < 1970; yy++)
            days -= DST::isLeapYear(yy) ? 366 : 365;
    }
    return days * 86400 + secOfYear;
}

// Parse Jn, n, or Mm.w.d transition rule and return Unix timestamp for year y
// p points to the character after the comma separator
bool DST::parseTransitionRule(const char *&p, int year,
                               int &monthOut, int &dayOut, int32_t &secOut)
{
    int32_t timeOfDay = 2 * 3600;  // default 02:00 wall clock

    if (*p == 'J') {
        // Jn: Julian day 1..365, no leap day
        p++;
        int n = 0;
        while (*p >= '0' && *p <= '9') n = n * 10 + (*p++ - '0');
        // Convert to 1-based day-of-year (ignoring leap day)
        int doy = n;  // already 1-based
        if (isLeapYear(year) && doy >= 60) doy++;  // skip Feb29
        if (*p == '/') { p++; timeOfDay = parseOffset(p); }
        // convert doy to month/day
        int m = 1;
        while (m <= 12 && doy > daysInMonth(m, year)) { doy -= daysInMonth(m, year); m++; }
        monthOut = m; dayOut = doy;
        secOut   = yearSecToUnix(year, (dayOfYear(m, doy, year) - 1) * 86400 + timeOfDay);
        return true;
    }

    if (*p == 'M') {
        // Mm.w.d
        p++;
        int mn = 0, w = 0, d = 0;
        while (*p >= '0' && *p <= '9') mn = mn * 10 + (*p++ - '0');
        if (*p == '.') p++;
        while (*p >= '0' && *p <= '9') w  = w  * 10 + (*p++ - '0');
        if (*p == '.') p++;
        while (*p >= '0' && *p <= '9') d  = d  * 10 + (*p++ - '0');
        if (*p == '/') { p++; timeOfDay = parseOffset(p); }

        // Find the w-th occurrence of weekday d in month mn of year
        // w=5 means the last occurrence
        // d: 0=Sun, 6=Sat
        int wdJan1 = weekdayOfJan1(year);
        // day-of-month for first occurrence of weekday d in month mn
        int firstDom = 1;
        int wdFirst  = (wdJan1 + dayOfYear(mn, 1, year) - 1) % 7;
        int diff     = (d - wdFirst + 7) % 7;
        firstDom    += diff;

        int dom;
        if (w == 5) {
            // last occurrence
            dom = firstDom;
            while (dom + 7 <= daysInMonth(mn, year)) dom += 7;
        } else {
            dom = firstDom + (w - 1) * 7;
            if (dom > daysInMonth(mn, year)) dom -= 7;
        }

        monthOut = mn; dayOut = dom;
        secOut   = yearSecToUnix(year, (dayOfYear(mn, dom, year) - 1) * 86400 + timeOfDay);
        return true;
    }

    // Numeric Julian day (0-based, leap counted)
    int n = 0;
    while (*p >= '0' && *p <= '9') n = n * 10 + (*p++ - '0');
    if (*p == '/') { p++; timeOfDay = parseOffset(p); }
    int doy1 = n + 1;  // convert to 1-based
    int m = 1;
    int rem = doy1;
    while (m <= 12 && rem > daysInMonth(m, year)) { rem -= daysInMonth(m, year); m++; }
    monthOut = m; dayOut = rem;
    secOut   = yearSecToUnix(year, n * 86400 + timeOfDay);
    return true;
}

// Convert Unix timestamp to calendar year
static int unixToYear(int32_t t) {
    int y = 1970;
    while (true) {
        int32_t yLen = (DST::isLeapYear(y) ? 366 : 365) * 86400;
        if (t < yLen) return y;
        t -= yLen;
        y++;
        if (y > 2100) return y;  // safety
    }
}

int32_t DST::parsePosixTz(const char *posixTz, int32_t t)
{
    const char *p = posixTz;
    if (!p || !*p) return 0;

    // std name
    skipName(p);
    if (!*p) return 0;

    // std offset  (POSIX: positive = west, so negate to get UTC+)
    int32_t stdOff = -parseOffset(p);

    if (!*p || *p == '\n') return stdOff;  // no DST rule

    // dst name
    skipName(p);
    if (!*p || *p == ',') {
        // dst offset defaults to stdOff + 1h
    }

    int32_t dstOff = stdOff + 3600;
    if (*p != ',' && *p != '\0') {
        dstOff = -parseOffset(p);
    }

    if (*p != ',') return stdOff;  // no transition rules → std all year
    p++;  // skip ','

    int year = unixToYear(t);

    // Parse start rule
    int startMon, startDay; int32_t startSec;
    if (!parseTransitionRule(p, year, startMon, startDay, startSec)) return stdOff;

    if (*p != ',') return stdOff;
    p++;

    // Parse end rule
    int endMon, endDay; int32_t endSec;
    if (!parseTransitionRule(p, year, endMon, endDay, endSec)) return stdOff;

    (void)startMon; (void)startDay; (void)endMon; (void)endDay;

    // Northern hemisphere: DST active between start and end
    // Southern hemisphere: DST active outside (end < start)
    if (startSec < endSec) {
        // Northern: DST if start <= t < end
        if (t >= startSec && t < endSec) return dstOff;
        return stdOff;
    } else {
        // Southern: DST if t >= start OR t < end
        if (t >= startSec || t < endSec) return dstOff;
        return stdOff;
    }
}
