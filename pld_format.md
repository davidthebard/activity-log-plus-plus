# pld.dat Format Specification

Activity Log play data file from 3DS system save `00020212` (US region).

## File Layout

| Offset | Size | Description |
|--------|------|-------------|
| `0x00000` | 16 bytes | File header |
| `0x00010` | 800,000 bytes | Table 1: Session Log (50,000 × 16-byte records) |
| `0xC3510` | 6,144 bytes | Table 2: App Summary (256 × 24-byte records) |

**Total file size:** 806,160 bytes (787.3 KB)

---

## File Header (16 bytes)

| Offset | Type | Description |
|--------|------|-------------|
| 0x00 | u32 | Unknown (observed: 0) |
| 0x04 | u32 | Possibly related to session log (observed: 232) |
| 0x08 | u32 | Unknown (observed: 2) |
| 0x0C | u32 | Unknown (observed: 0) |

---

## Table 1: Session Log

- **Offset:** `0x10`
- **Record size:** 16 bytes
- **Max capacity:** 50,000 records
- **Empty marker:** `FF FF FF FF FF FF FF FF FF FF FF FF 00 00 00 00`

Each record represents the play time for one title during one clock hour.

### Session Log Record (16 bytes)

| Offset | Type | Description |
|--------|------|-------------|
| 0x00 | u64 (LE) | Title ID |
| 0x08 | u32 (LE) | Timestamp — seconds since January 1, 2000 00:00:00, rounded down to the hour |
| 0x0C | u32 (LE) | Play time during that hour, in seconds (max 3600) |

### Notes
- Records are chronologically ordered.
- Timestamps are always aligned to the start of a clock hour (multiples of 3600 from epoch).
- Multiple records can share the same timestamp if multiple titles were played in the same hour.
- A single play session spanning multiple hours produces one record per hour.

---

## Table 2: App Summary

- **Offset:** `0xC3510`
- **Record size:** 24 bytes
- **Max capacity:** 256 records (3dbrew notes a 112-app display limit in the Activity Log UI)
- **Empty marker:** pattern of `FFFFFFFF` and `00000000` bytes

Each record stores the aggregated stats for one title.

### App Summary Record (24 bytes)

| Offset | Type | Description |
|--------|------|-------------|
| 0x00 | u64 (LE) | Title ID |
| 0x08 | u32 (LE) | Total play time, in seconds |
| 0x0C | u16 (LE) | Launch count |
| 0x0E | u16 (LE) | Unknown (observed values: 1 or 2) |
| 0x10 | u16 (LE) | First played date — days since January 1, 2000 |
| 0x12 | u16 (LE) | Last played date — days since January 1, 2000 |
| 0x14 | u32 (LE) | Unknown (observed: always 0) |

### Notes
- The summary table can be fully reconstructed from the session log.
- Total play time = sum of all session log entries for that title.
- First played = earliest session timestamp for that title.
- Last played = latest session timestamp for that title.

---

## Timestamps and Dates

- **Session log timestamps:** Seconds since Jan 1, 2000 00:00:00 UTC, always rounded to the start of the clock hour.
- **Summary date fields:** Days since Jan 1, 2000 (e.g., 9524 = January 28, 2026).

## Endianness

All multi-byte values are **little-endian**, consistent with the ARM11 architecture of the 3DS.
