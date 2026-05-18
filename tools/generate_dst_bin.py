#!/usr/bin/env python3
"""
Generate dst_rules.bin from the system's tzdata (via the 'pytz' package).

Usage:
    pip install pytz
    python3 generate_dst_bin.py [-o dst_rules.bin] [-z zone1 zone2 ...]

Without -z, all IANA timezones present in pytz are exported.
With -z, only the listed timezone IDs are exported.

Output binary format (dst_rules.bin):
    4 bytes  magic  "DSTR"
    2 bytes  uint16 LE  number of entries N
    per entry:
        uint8  len_tzId
        bytes  tzId   (no null terminator)
        uint8  len_posixTz
        bytes  posixTz (no null terminator)
"""

import argparse
import struct
import sys
from datetime import datetime, timezone, timedelta


def posix_tz_from_zone(zone_name: str) -> str | None:
    """Return a POSIX TZ string for the given IANA timezone name."""
    try:
        import pytz
        from pytz import timezone as ptz
        from pytz.tzinfo import StaticTzInfo, BaseTzInfo
    except ImportError:
        print("Error: pytz not installed. Run: pip install pytz", file=sys.stderr)
        sys.exit(1)

    try:
        tz = ptz(zone_name)
    except pytz.exceptions.UnknownTimeZoneError:
        return None

    # --- Build POSIX TZ string from pytz transitions --------------------
    # We use the transitions around a reference year to determine std/dst names,
    # offsets, and transition rules.

    REF_YEAR = 2024

    # Find the two transitions in REF_YEAR (if any DST)
    if isinstance(tz, StaticTzInfo):
        off = int(tz.utcoffset(datetime(REF_YEAR, 6, 1)).total_seconds())
        name = tz.tzname(datetime(REF_YEAR, 6, 1))
        h, r = divmod(abs(off), 3600)
        m, s = divmod(r, 60)
        sign = '-' if off > 0 else ('+' if off < 0 else '')
        offset_str = f"{sign}{h}" if m == 0 and s == 0 else \
                     f"{sign}{h}:{m:02d}" if s == 0 else \
                     f"{sign}{h}:{m:02d}:{s:02d}"
        return f"{name}{offset_str}"

    # Walk transitions to find std/dst info
    transitions = []
    if hasattr(tz, '_utc_transition_times'):
        for t in tz._utc_transition_times:
            if t.year == REF_YEAR:
                transitions.append(t)

    if len(transitions) < 2:
        # No DST in reference year – use static offset from summer
        dt_summer = datetime(REF_YEAR, 7, 1, tzinfo=pytz.utc)
        dt_local  = dt_summer.astimezone(tz)
        off  = int(dt_local.utcoffset().total_seconds())
        name = dt_local.tzname()
        h, r = divmod(abs(off), 3600)
        m, s = divmod(r, 60)
        sign = '-' if off > 0 else ('+' if off < 0 else '')
        offset_str = f"{sign}{h}" if m == 0 and s == 0 else f"{sign}{h}:{m:02d}"
        return f"{name}{offset_str}"

    # Find std (winter) and dst (summer) transitions
    def local_info(utc_dt):
        lt = utc_dt.replace(tzinfo=pytz.utc).astimezone(tz)
        return lt.utcoffset().total_seconds(), lt.tzname()

    t1, t2 = transitions[0], transitions[1]
    off1, name1 = local_info(t1)
    off2, name2 = local_info(t2)

    if off1 < off2:
        std_off, std_name = int(off1), name1
        dst_off, dst_name = int(off2), name2
        dst_start, dst_end = t2, t1
    else:
        std_off, std_name = int(off2), name2
        dst_off, dst_name = int(off1), name1
        dst_start, dst_end = t1, t2

    def fmt_offset(off_sec: int) -> str:
        # POSIX convention: positive = west of UTC → negate
        posix = -off_sec
        sign  = '-' if posix < 0 else ''
        posix = abs(posix)
        h, r  = divmod(posix, 3600)
        m, s  = divmod(r, 60)
        if m == 0 and s == 0:
            return f"{sign}{h}"
        if s == 0:
            return f"{sign}{h}:{m:02d}"
        return f"{sign}{h}:{m:02d}:{s:02d}"

    def fmt_rule(utc_dt: datetime, wall_off_sec: int) -> str:
        """Return Mm.w.d[/time] rule for a UTC transition time."""
        # Convert to local wall clock
        local_dt = utc_dt.replace(tzinfo=pytz.utc) + timedelta(seconds=wall_off_sec)
        m  = local_dt.month
        d  = local_dt.weekday()  # 0=Mon in Python → convert to POSIX 0=Sun
        d_posix = (d + 1) % 7
        dom = local_dt.day
        # Which occurrence of this weekday in the month?
        first_occ = dom - ((dom - 1) // 7) * 7
        w = (dom - first_occ) // 7 + 1
        # Check if it could be the last
        if dom + 7 > _days_in_month(m, local_dt.year):
            w = 5
        h, r   = divmod(local_dt.hour * 3600 + local_dt.minute * 60 + local_dt.second, 3600)
        mn, ss = divmod(r, 60)
        time_part = "" if (h == 2 and mn == 0 and ss == 0) else \
                    f"/{h}" if (mn == 0 and ss == 0) else \
                    f"/{h}:{mn:02d}" if ss == 0 else \
                    f"/{h}:{mn:02d}:{ss:02d}"
        return f"M{m}.{w}.{d_posix}{time_part}"

    def _days_in_month(m, y):
        d = [31,28,31,30,31,30,31,31,30,31,30,31]
        if m == 2 and (y % 4 == 0 and (y % 100 != 0 or y % 400 == 0)):
            return 29
        return d[m-1]

    start_rule = fmt_rule(dst_start, std_off)  # clocks go forward: before = std
    end_rule   = fmt_rule(dst_end,   dst_off)  # clocks go back: before = dst

    posix = f"{std_name}{fmt_offset(std_off)}{dst_name}"
    dst_off_str = fmt_offset(dst_off)
    # Only append dst offset if it's not the default (std + 1h)
    if dst_off != std_off + 3600:
        posix += dst_off_str
    posix += f",{start_rule},{end_rule}"
    return posix


def build_binary(zones: list[tuple[str, str]]) -> bytes:
    out = bytearray()
    out += b'DSTR'
    out += struct.pack('<H', len(zones))
    for tz_id, posix_tz in zones:
        tz_bytes  = tz_id.encode('ascii')
        ptz_bytes = posix_tz.encode('ascii')
        assert len(tz_bytes)  <= 255, f"tzId too long: {tz_id}"
        assert len(ptz_bytes) <= 255, f"posixTz too long: {posix_tz}"
        out += struct.pack('B', len(tz_bytes))
        out += tz_bytes
        out += struct.pack('B', len(ptz_bytes))
        out += ptz_bytes
    return bytes(out)


def main():
    ap = argparse.ArgumentParser(description="Generate dst_rules.bin")
    ap.add_argument('-o', '--output', default='dst_rules.bin',
                    help='Output file (default: dst_rules.bin)')
    ap.add_argument('-z', '--zones', nargs='*',
                    help='Specific IANA zone IDs to include (default: all)')
    args = ap.parse_args()

    try:
        import pytz
        zone_list = args.zones if args.zones else sorted(pytz.all_timezones)
    except ImportError:
        print("Error: pytz not installed. Run: pip install pytz", file=sys.stderr)
        sys.exit(1)

    results = []
    skipped = 0
    for z in zone_list:
        ptz = posix_tz_from_zone(z)
        if ptz is None:
            skipped += 1
            continue
        results.append((z, ptz))
        print(f"  {z:40s}  {ptz}")

    data = build_binary(results)
    with open(args.output, 'wb') as f:
        f.write(data)

    print(f"\nWrote {len(results)} entries ({skipped} skipped) → {args.output}")
    print(f"File size: {len(data)} bytes")


if __name__ == '__main__':
    main()
