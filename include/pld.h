#pragma once

#include <3ds.h>
#include <stdbool.h>
#include <stddef.h>

/*
 * pld.h — Activity Log save-file (pld.dat) layout and API
 *
 * File lives in system save archive 0x00020212 (US region) at /pld.dat.
 *
 * Layout:
 *   0x00000  16 bytes   File header
 *   0x00010  800000 B   Table 1: Session Log  (50000 × 16-byte records)
 *   0xC3510  6144 B     Table 2: App Summary  (256 × 24-byte records)
 *
 * All multi-byte fields are little-endian.
 * Epoch: seconds / days since 2000-01-01 00:00:00 UTC.
 */

/* ── File layout constants ──────────────────────────────────────── */

#define PLD_SESSION_COUNT   50000
#define PLD_SUMMARY_COUNT   256

#define PLD_HEADER_OFFSET   0x00000u
#define PLD_SESSION_OFFSET  0x00010u
#define PLD_SUMMARY_OFFSET  0xC3510u

#define PLD_FILE_SIZE       806160u

/* Save-archive IDs for each region's Activity Log title */
#define ACTIVITY_SAVE_ID_JPN  0x00020202u
#define ACTIVITY_SAVE_ID_USA  0x00020212u
#define ACTIVITY_SAVE_ID_EUR  0x00020222u
#define ACTIVITY_SAVE_ID_KOR  0x00020272u

/* ── Data structures ────────────────────────────────────────────── */

/* 16-byte file header (field meanings are partially unknown) */
typedef struct {
    u32 unknown0;
    u32 field04;    /* observed: 232; possibly related to session count */
    u32 unknown8;
    u32 unknownC;
} PldHeader;        /* 16 bytes */

/* One hour-granularity play session for one title */
typedef struct {
    u64 title_id;   /* 64-bit title ID, little-endian                      */
    u32 timestamp;  /* seconds since 2000-01-01 00:00 UTC, hour-aligned    */
    u32 play_secs;  /* seconds played during that hour (0–3600)            */
} PldSession;       /* 16 bytes */

/* Per-title aggregated stats */
typedef struct {
    u64 title_id;           /* 64-bit title ID                             */
    u32 total_secs;         /* total play time in seconds                  */
    u16 launch_count;       /* number of times launched                    */
    u16 unknown_e;          /* observed: 1 or 2                            */
    u16 first_played_days;  /* days since 2000-01-01                       */
    u16 last_played_days;   /* days since 2000-01-01                       */
    u32 unknown_14;         /* observed: always 0                          */
} PldSummary;       /* 24 bytes */

/* Parsed result from pld_read_summary() — summary table only */
typedef struct {
    PldHeader  header;
    PldSummary summaries[PLD_SUMMARY_COUNT];
    int        summary_count;   /* number of non-empty entries             */
} PldFile;

/* Compacted in-memory session log (only valid, non-empty entries) */
typedef struct {
    PldSession *entries;  /* malloc'd; entries[0..count-1] are valid */
    int         count;    /* number of valid sessions                */
} PldSessionLog;

/* ── API ────────────────────────────────────────────────────────── */

/**
 * Open the Activity Log system save archive.
 * save_id: one of ACTIVITY_SAVE_ID_* for the target region.
 * On success the caller owns *archive_out and must close it with
 * FSUSER_CloseArchive() when done.
 */
Result pld_open_archive(FS_Archive *archive_out, u32 save_id);

/**
 * Read and parse the header and summary table from an already-opened
 * archive.  Returns a non-zero Result on I/O failure.
 */
Result pld_read_summary(FS_Archive archive, PldFile *out);

/**
 * Return true if the summary record is an empty (unused) slot.
 */
bool pld_summary_is_empty(const PldSummary *s);

bool   pld_session_is_empty(const PldSession *s);
Result pld_read_sessions(FS_Archive archive, PldSessionLog *out);
void   pld_sessions_free(PldSessionLog *log);
int    pld_count_sessions_for(const PldSessionLog *log, u64 title_id);

/* Compute the longest streak of consecutive calendar days played for one title.
 * indices[0..count-1] are indexes into sessions->entries, sorted descending by timestamp.
 * Returns 0 if count==0, else >= 1. */
int    pld_longest_streak(const PldSessionLog *sessions,
                          const int *indices, int count);

/* Merge remote sessions into *local in-place.
 * Matching (title_id, timestamp): sum play_secs, cap at 3600 (unless add_only).
 * add_only=true: skip summing for existing entries; only append new ones.
 * Unique remote records are appended to the pre-allocated buffer.
 * Returns number of new records appended, or -1 if buffer would overflow. */
int pld_merge_sessions(PldSessionLog *local, const PldSessionLog *remote, bool add_only);

/* Merge remote compact summary array into local->summaries in-place.
 * Matching title_id: sum total_secs and launch_count (capped at UINT16_MAX),
 * take earliest first_played_days and latest last_played_days (unless add_only).
 * add_only=true: skip updating existing entries; only insert new ones.
 * Unique remote records are placed into empty local slots.
 * Returns number of new titles added, or -1 if summary table is full. */
int pld_merge_summaries(PldFile *local, const PldSummary *remote, int remote_count, bool add_only);

/* Read a merged.dat from SD into *pld_out and *sessions_out.
 * sessions_out->entries is malloc'd (PLD_SESSION_COUNT capacity, compacted).
 * Returns 0 on success, non-zero on I/O failure. */
Result pld_read_sd(const char *path, PldFile *pld_out, PldSessionLog *sessions_out);

/* Write header + sessions (expanded to 50000 slots) + summaries to SD path.
 * Returns 0 on success, non-zero on I/O failure. */
Result pld_write_sd(const char *path, const PldFile *pld,
                    const PldSessionLog *sessions);

/* Read src_path (must be PLD_FILE_SIZE bytes) and write a new timestamped
 * backup; prunes oldest if over PLD_MAX_BACKUPS. */
Result pld_backup_from_path(const char *src_path);

/* Expand compacted sessions back to the full 50000-slot array, combine with
 * the updated header and summary table, and write all 806160 bytes to NAND.
 * Empty session slots are filled with the 0xFF empty marker.
 * Returns 0 on success, non-zero on I/O failure. */
Result pld_write_pld(FS_Archive archive, const PldFile *pld,
                     const PldSessionLog *sessions);

/* ── Backup / Restore ───────────────────────────────────────────── */

#define PLD_BACKUP_DIR   "sdmc:/3ds/activity-log-pp"
#define PLD_MERGED_PATH  "sdmc:/3ds/activity-log-pp/merged.dat"
#define PLD_MAX_BACKUPS 10

/* Filenames only (not full paths), sorted most-recent-first. */
typedef struct {
    char names[PLD_MAX_BACKUPS][32];
    int  count;
} PldBackupList;

/* Write a new timestamped backup and prune oldest if over limit. */
Result pld_backup(FS_Archive archive);

/* Populate *out with existing backup filenames, most-recent-first. */
Result pld_list_backups(PldBackupList *out);

/* Read the app (summary) count from a backup file on SD without writing to
 * NAND.  Only the 6 144-byte summary table is read.  Sets *app_count on
 * success.  Returns 0 on success, non-zero on I/O failure. */
Result pld_backup_app_count(const char *path, int *app_count);

/* Restore from the given full SD path into the open archive. */
Result pld_restore(FS_Archive archive, const char *path);

/* ── Formatting helpers ─────────────────────────────────────────── */

/** Write "HHHh MMm SSs" into buf (null-terminated, len includes NUL). */
void pld_fmt_time(u32 seconds, char *buf, size_t len);

/** Write "YYYY-MM-DD" (days since 2000-01-01) into buf. */
void pld_fmt_date(u16 days, char *buf, size_t len);

/** Write "YYYY-MM-DD HH:00" (seconds since 2000-01-01, hour-aligned) into buf. */
void pld_fmt_timestamp(u32 timestamp, char *buf, size_t len);
