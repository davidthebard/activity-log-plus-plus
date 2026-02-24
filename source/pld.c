#include "pld.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>
#include <errno.h>

/* ── Archive ────────────────────────────────────────────────────── */

Result pld_open_archive(FS_Archive *archive_out, u32 save_id)
{
    /*
     * Binary low-path for ARCHIVE_SYSTEM_SAVEDATA:
     *   u32 mediaType  (MEDIATYPE_NAND = 0)
     *   u32 saveId
     */
    u32 path_data[2] = { MEDIATYPE_NAND, save_id };
    FS_Path archive_path = { PATH_BINARY, sizeof(path_data), path_data };

    return FSUSER_OpenArchive(archive_out, ARCHIVE_SYSTEM_SAVEDATA,
                              archive_path);
}

/* ── File parsing ───────────────────────────────────────────────── */

Result pld_read_summary(FS_Archive archive, PldFile *out)
{
    memset(out, 0, sizeof(*out));

    Handle    file = 0;
    u32       bytes_read = 0;
    FS_Path   file_path  = fsMakePath(PATH_ASCII, "/pld.dat");

    Result rc = FSUSER_OpenFile(&file, archive, file_path,
                                FS_OPEN_READ, 0);
    if (R_FAILED(rc)) return rc;

    /* Header */
    rc = FSFILE_Read(file, &bytes_read, PLD_HEADER_OFFSET,
                     &out->header, sizeof(out->header));
    if (R_FAILED(rc)) goto done;

    /* Summary table — seek directly to its absolute offset */
    rc = FSFILE_Read(file, &bytes_read, PLD_SUMMARY_OFFSET,
                     out->summaries, sizeof(out->summaries));
    if (R_FAILED(rc)) goto done;

    /* Count live entries */
    out->summary_count = 0;
    for (int i = 0; i < PLD_SUMMARY_COUNT; i++) {
        if (!pld_summary_is_empty(&out->summaries[i]))
            out->summary_count++;
    }

done:
    FSFILE_Close(file);
    return rc;
}

bool pld_summary_is_empty(const PldSummary *s)
{
    /*
     * Empty slots have title_id = 0xFFFFFFFFFFFFFFFF (matching the
     * session-log empty marker pattern from the spec).
     * Also treat a fully-zeroed slot as empty.
     */
    return (s->title_id == 0xFFFFFFFFFFFFFFFFULL) || (s->title_id == 0ULL);
}

bool pld_session_is_empty(const PldSession *s)
{
    return s->title_id == 0xFFFFFFFFFFFFFFFFULL;
}

Result pld_read_sessions(FS_Archive archive, PldSessionLog *out)
{
    out->entries = NULL;
    out->count   = 0;

    PldSession *buf = malloc(PLD_SESSION_COUNT * sizeof(PldSession));
    if (!buf) return -1;

    Handle  file = 0;
    u32     bytes_read = 0;
    FS_Path file_path  = fsMakePath(PATH_ASCII, "/pld.dat");

    Result rc = FSUSER_OpenFile(&file, archive, file_path,
                                FS_OPEN_READ, 0);
    if (R_FAILED(rc)) { free(buf); return rc; }

    rc = FSFILE_Read(file, &bytes_read, PLD_SESSION_OFFSET,
                     buf, PLD_SESSION_COUNT * sizeof(PldSession));
    FSFILE_Close(file);

    if (R_FAILED(rc)) { free(buf); return rc; }

    /* Compact: move valid entries to the front in-place */
    int count = 0;
    for (int i = 0; i < PLD_SESSION_COUNT; i++) {
        if (!pld_session_is_empty(&buf[i]))
            buf[count++] = buf[i];
    }

    out->entries = buf;
    out->count   = count;
    return 0;
}

void pld_sessions_free(PldSessionLog *log)
{
    free(log->entries);
    log->entries = NULL;
    log->count   = 0;
}

static int cmp_session_key(const void *a, const void *b)
{
    const PldSession *sa = (const PldSession *)a;
    const PldSession *sb = (const PldSession *)b;
    if (sa->title_id < sb->title_id) return -1;
    if (sa->title_id > sb->title_id) return  1;
    if (sa->timestamp < sb->timestamp) return -1;
    if (sa->timestamp > sb->timestamp) return  1;
    return 0;
}

static int session_find(const PldSession *entries, int count,
                        u64 title_id, u32 timestamp)
{
    int lo = 0, hi = count - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        const PldSession *m = &entries[mid];
        if (m->title_id == title_id && m->timestamp == timestamp) return mid;
        if (m->title_id < title_id ||
            (m->title_id == title_id && m->timestamp < timestamp))
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return -1;
}

int pld_merge_sessions(PldSessionLog *local, const PldSessionLog *remote, bool add_only)
{
    if (local->count > 1)
        qsort(local->entries, (size_t)local->count,
              sizeof(PldSession), cmp_session_key);

    int added = 0;
    for (int i = 0; i < remote->count; i++) {
        const PldSession *r = &remote->entries[i];
        if (r->title_id == 0 || r->title_id == 0xFFFFFFFFFFFFFFFFULL)
            continue;
        int idx = session_find(local->entries, local->count,
                               r->title_id, r->timestamp);
        if (idx >= 0) {
            if (!add_only) {
                u32 sum = local->entries[idx].play_secs + r->play_secs;
                local->entries[idx].play_secs = sum > 3600 ? 3600 : sum;
            }
            /* add_only: existing entry preserved as-is */
        } else {
            if (local->count >= PLD_SESSION_COUNT) return -1;
            local->entries[local->count++] = *r;
            added++;
        }
    }

    if (added > 0 && local->count > 1)
        qsort(local->entries, (size_t)local->count,
              sizeof(PldSession), cmp_session_key);

    return added;
}

int pld_merge_summaries(PldFile *local, const PldSummary *remote, int remote_count, bool add_only)
{
    int added = 0;
    for (int i = 0; i < remote_count; i++) {
        const PldSummary *r = &remote[i];
        if (pld_summary_is_empty(r)) continue;

        int found = -1;
        for (int j = 0; j < PLD_SUMMARY_COUNT; j++) {
            if (local->summaries[j].title_id == r->title_id) {
                found = j; break;
            }
        }

        if (found >= 0) {
            if (!add_only) {
                PldSummary *l = &local->summaries[found];
                l->total_secs += r->total_secs;
                u32 lc = (u32)l->launch_count + r->launch_count;
                l->launch_count = lc > 0xFFFF ? 0xFFFF : (u16)lc;
                if (r->first_played_days < l->first_played_days)
                    l->first_played_days = r->first_played_days;
                if (r->last_played_days > l->last_played_days)
                    l->last_played_days = r->last_played_days;
            }
            /* add_only: existing entry preserved as-is */
        } else {
            int slot = -1;
            for (int j = 0; j < PLD_SUMMARY_COUNT; j++) {
                if (pld_summary_is_empty(&local->summaries[j])) {
                    slot = j; break;
                }
            }
            if (slot < 0) return -1;
            local->summaries[slot] = *r;
            local->summary_count++;
            added++;
        }
    }
    return added;
}

static int cmp_names_desc(const void *a, const void *b);

Result pld_read_sd(const char *path, PldFile *pld_out, PldSessionLog *sessions_out)
{
    memset(pld_out, 0, sizeof(*pld_out));
    sessions_out->entries = NULL;
    sessions_out->count   = 0;

    FILE *f = fopen(path, "rb");
    if (!f) return (Result)-1;

    if (fread(&pld_out->header, sizeof(PldHeader), 1, f) != 1)
        { fclose(f); return (Result)-1; }

    PldSession *buf = malloc(PLD_SESSION_COUNT * sizeof(PldSession));
    if (!buf) { fclose(f); return (Result)-1; }

    if (fseek(f, PLD_SESSION_OFFSET, SEEK_SET) != 0 ||
        fread(buf, sizeof(PldSession), PLD_SESSION_COUNT, f) != PLD_SESSION_COUNT)
        { free(buf); fclose(f); return (Result)-1; }

    if (fseek(f, PLD_SUMMARY_OFFSET, SEEK_SET) != 0 ||
        fread(pld_out->summaries, sizeof(PldSummary), PLD_SUMMARY_COUNT, f) != PLD_SUMMARY_COUNT)
        { free(buf); fclose(f); return (Result)-1; }
    fclose(f);

    int count = 0;
    for (int i = 0; i < PLD_SESSION_COUNT; i++)
        if (!pld_session_is_empty(&buf[i]))
            buf[count++] = buf[i];
    sessions_out->entries = buf;
    sessions_out->count   = count;

    pld_out->summary_count = 0;
    for (int i = 0; i < PLD_SUMMARY_COUNT; i++)
        if (!pld_summary_is_empty(&pld_out->summaries[i]))
            pld_out->summary_count++;
    return 0;
}

Result pld_write_sd(const char *path, const PldFile *pld,
                    const PldSessionLog *sessions)
{
    u8 *buf = malloc(PLD_FILE_SIZE);
    if (!buf) return (Result)-1;

    memcpy(buf + PLD_HEADER_OFFSET, &pld->header, sizeof(pld->header));
    memset(buf + PLD_SESSION_OFFSET, 0xFF, PLD_SESSION_COUNT * sizeof(PldSession));
    memcpy(buf + PLD_SESSION_OFFSET, sessions->entries,
           (size_t)sessions->count * sizeof(PldSession));
    memcpy(buf + PLD_SUMMARY_OFFSET, pld->summaries, sizeof(pld->summaries));

    FILE *f = fopen(path, "wb");
    if (!f) { free(buf); return (Result)-1; }
    size_t written = fwrite(buf, 1, PLD_FILE_SIZE, f);
    fclose(f);
    free(buf);
    return (written == PLD_FILE_SIZE) ? 0 : (Result)-1;
}

Result pld_backup_from_path(const char *src_path)
{
    u8 *buf = malloc(PLD_FILE_SIZE);
    if (!buf) return (Result)-1;

    FILE *f = fopen(src_path, "rb");
    if (!f) { free(buf); return (Result)-1; }
    size_t n = fread(buf, 1, PLD_FILE_SIZE, f);
    fclose(f);
    if (n != PLD_FILE_SIZE) { free(buf); return (Result)-1; }

    mkdir(PLD_BACKUP_DIR, 0777);

    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    char path[128];
    snprintf(path, sizeof(path),
             "%s/pld_backup_%04d%02d%02d_%02d%02d%02d.dat",
             PLD_BACKUP_DIR,
             t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
             t->tm_hour, t->tm_min, t->tm_sec);

    FILE *out = fopen(path, "wb");
    if (!out) { free(buf); return (Result)-1; }
    size_t written = fwrite(buf, 1, PLD_FILE_SIZE, out);
    fclose(out);
    free(buf);
    if (written != PLD_FILE_SIZE) return (Result)-1;

    /* Prune oldest (reuses cmp_names_desc) */
    char names[PLD_MAX_BACKUPS + 4][32];
    int  count = 0;
    DIR *dir = opendir(PLD_BACKUP_DIR);
    if (dir) {
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL && count < PLD_MAX_BACKUPS + 4) {
            const char *nm = ent->d_name;
            if (strlen(nm) == 30 && strncmp(nm, "pld_backup_", 11) == 0) {
                snprintf(names[count], sizeof(names[0]), "%s", nm);
                count++;
            }
        }
        closedir(dir);
        if (count > PLD_MAX_BACKUPS) {
            qsort(names, count, sizeof(names[0]), cmp_names_desc);
            for (int i = PLD_MAX_BACKUPS; i < count; i++) {
                char old_path[128];
                snprintf(old_path, sizeof(old_path), "%s/%s",
                         PLD_BACKUP_DIR, names[i]);
                remove(old_path);
            }
        }
    }
    return 0;
}

Result pld_write_pld(FS_Archive archive, const PldFile *pld,
                     const PldSessionLog *sessions)
{
    u8 *buf = malloc(PLD_FILE_SIZE);
    if (!buf) return (Result)-1;

    /* Header */
    memcpy(buf + PLD_HEADER_OFFSET, &pld->header, sizeof(pld->header));

    /* Session log: fill all slots with 0xFF (empty marker), then
     * overwrite the first `count` slots with the valid entries. */
    memset(buf + PLD_SESSION_OFFSET, 0xFF,
           PLD_SESSION_COUNT * sizeof(PldSession));
    memcpy(buf + PLD_SESSION_OFFSET, sessions->entries,
           (size_t)sessions->count * sizeof(PldSession));

    /* Summary table (full 256-slot array, empties already marked) */
    memcpy(buf + PLD_SUMMARY_OFFSET, pld->summaries, sizeof(pld->summaries));

    Handle  file = 0;
    u32     bytes_written = 0;
    FS_Path file_path = fsMakePath(PATH_ASCII, "/pld.dat");

    Result rc = FSUSER_OpenFile(&file, archive, file_path, FS_OPEN_WRITE, 0);
    if (R_FAILED(rc)) { free(buf); return rc; }

    rc = FSFILE_Write(file, &bytes_written, 0, buf, PLD_FILE_SIZE,
                      FS_WRITE_FLUSH);
    FSFILE_Close(file);
    free(buf);
    if (R_SUCCEEDED(rc))
        rc = FSUSER_ControlArchive(archive, ARCHIVE_ACTION_COMMIT_SAVE_DATA,
                                   NULL, 0, NULL, 0);
    return rc;
}

int pld_count_sessions_for(const PldSessionLog *log, u64 title_id)
{
    int n = 0;
    for (int i = 0; i < log->count; i++) {
        if (log->entries[i].title_id == title_id)
            n++;
    }
    return n;
}

int pld_longest_streak(const PldSessionLog *sessions,
                       const int *indices, int count)
{
    if (count == 0) return 0;

    int best = 1, run = 1;
    int prev_day = (int)(sessions->entries[indices[0]].timestamp / 86400u);

    for (int i = 1; i < count; i++) {
        int cur_day = (int)(sessions->entries[indices[i]].timestamp / 86400u);
        if (cur_day == prev_day)
            continue;                   /* same day — skip duplicate */
        if (prev_day - cur_day == 1)
            run++;                      /* consecutive day (descending order) */
        else
            run = 1;                    /* gap — reset streak */
        if (run > best) best = run;
        prev_day = cur_day;
    }
    return best;
}

/* ── Backup / Restore ───────────────────────────────────────────── */

static int cmp_names_desc(const void *a, const void *b)
{
    return strcmp((const char *)b, (const char *)a);
}

Result pld_backup(FS_Archive archive)
{
    u8 *buf = malloc(PLD_FILE_SIZE);
    if (!buf) return (Result)-1;

    Handle  file = 0;
    u32     bytes_read = 0;
    FS_Path file_path = fsMakePath(PATH_ASCII, "/pld.dat");

    Result rc = FSUSER_OpenFile(&file, archive, file_path, FS_OPEN_READ, 0);
    if (R_FAILED(rc)) { free(buf); return rc; }

    rc = FSFILE_Read(file, &bytes_read, 0, buf, PLD_FILE_SIZE);
    FSFILE_Close(file);
    if (R_FAILED(rc)) { free(buf); return rc; }

    mkdir(PLD_BACKUP_DIR, 0777);

    /* Build timestamped filename */
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    char path[128];
    snprintf(path, sizeof(path),
             "%s/pld_backup_%04d%02d%02d_%02d%02d%02d.dat",
             PLD_BACKUP_DIR,
             t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
             t->tm_hour, t->tm_min, t->tm_sec);

    FILE *f = fopen(path, "wb");
    if (!f) { free(buf); return (Result)-1; }
    size_t written = fwrite(buf, 1, PLD_FILE_SIZE, f);
    fclose(f);
    free(buf);
    if (written != PLD_FILE_SIZE) return (Result)-1;

    /* Prune: collect all backup filenames, sort desc, delete oldest */
    char names[PLD_MAX_BACKUPS + 4][32];
    int  count = 0;
    DIR *dir = opendir(PLD_BACKUP_DIR);
    if (dir) {
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL &&
               count < PLD_MAX_BACKUPS + 4) {
            const char *n = ent->d_name;
            if (strlen(n) == 30 && strncmp(n, "pld_backup_", 11) == 0) {
                snprintf(names[count], sizeof(names[0]), "%s", n);
                count++;
            }
        }
        closedir(dir);
        if (count > PLD_MAX_BACKUPS) {
            qsort(names, count, sizeof(names[0]), cmp_names_desc);
            for (int i = PLD_MAX_BACKUPS; i < count; i++) {
                char old_path[128];
                snprintf(old_path, sizeof(old_path), "%s/%s",
                         PLD_BACKUP_DIR, names[i]);
                remove(old_path);
            }
        }
    }

    return 0;
}

Result pld_list_backups(PldBackupList *out)
{
    memset(out, 0, sizeof(*out));

    DIR *dir = opendir(PLD_BACKUP_DIR);
    if (!dir) return (Result)-1;

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL && out->count < PLD_MAX_BACKUPS) {
        const char *n = ent->d_name;
        if (strlen(n) == 30 && strncmp(n, "pld_backup_", 11) == 0) {
            snprintf(out->names[out->count], sizeof(out->names[0]), "%s", n);
            out->count++;
        }
    }
    closedir(dir);

    qsort(out->names, out->count, sizeof(out->names[0]), cmp_names_desc);
    return 0;
}

Result pld_backup_app_count(const char *path, int *app_count)
{
    *app_count = 0;

    FILE *f = fopen(path, "rb");
    if (!f) return (Result)-1;

    if (fseek(f, PLD_SUMMARY_OFFSET, SEEK_SET) != 0) {
        fclose(f); return (Result)-1;
    }

    PldSummary summaries[PLD_SUMMARY_COUNT];
    size_t n = fread(summaries, sizeof(PldSummary), PLD_SUMMARY_COUNT, f);
    fclose(f);
    if (n != PLD_SUMMARY_COUNT) return (Result)-1;

    int count = 0;
    for (int i = 0; i < PLD_SUMMARY_COUNT; i++) {
        if (!pld_summary_is_empty(&summaries[i]))
            count++;
    }
    *app_count = count;
    return 0;
}

Result pld_restore(FS_Archive archive, const char *path)
{
    u8 *buf = malloc(PLD_FILE_SIZE);
    if (!buf) return (Result)-1;

    FILE *f = fopen(path, "rb");
    if (!f) { free(buf); return (Result)-1; }
    size_t bytes_read = fread(buf, 1, PLD_FILE_SIZE, f);
    fclose(f);
    if (bytes_read != PLD_FILE_SIZE) { free(buf); return (Result)-1; }

    Handle  file = 0;
    u32     bytes_written = 0;
    FS_Path file_path = fsMakePath(PATH_ASCII, "/pld.dat");

    Result rc = FSUSER_OpenFile(&file, archive, file_path, FS_OPEN_WRITE, 0);
    if (R_FAILED(rc)) { free(buf); return rc; }

    rc = FSFILE_Write(file, &bytes_written, 0, buf, PLD_FILE_SIZE,
                      FS_WRITE_FLUSH);
    FSFILE_Close(file);
    free(buf);
    if (R_SUCCEEDED(rc))
        rc = FSUSER_ControlArchive(archive, ARCHIVE_ACTION_COMMIT_SAVE_DATA,
                                   NULL, 0, NULL, 0);
    return rc;
}

/* ── Formatting ─────────────────────────────────────────────────── */

void pld_fmt_time(u32 seconds, char *buf, size_t len)
{
    unsigned h = seconds / 3600;
    unsigned m = (seconds % 3600) / 60;
    snprintf(buf, len, "%uh %02um", h, m);
}

/*
 * Days-since-2000 → Gregorian YYYY-MM-DD.
 * Correct for years 2000–2099 (the only range present in 3DS data).
 * 2000 is a leap year; within 2000–2099 every year divisible by 4 is leap.
 */
void pld_fmt_date(u16 days, char *buf, size_t len)
{
    static const int month_days[12] = {
        31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
    };

    int year = 2000;
    int d    = (int)days;

    /* Consume complete 4-year blocks (1461 days each; 2000 is leap-first) */
    year += (d / 1461) * 4;
    d    %= 1461;

    /* Remaining years within the block */
    for (int y = 0; y < 4; y++) {
        bool leap = (year % 4 == 0);   /* within 2000–2099, div-by-4 = leap */
        int  days_in_year = leap ? 366 : 365;
        if (d < days_in_year) break;
        d -= days_in_year;
        year++;
    }

    /* Month */
    bool leap = (year % 4 == 0);
    int  month = 0;
    for (month = 0; month < 12; month++) {
        int dim = month_days[month];
        if (leap && month == 1) dim = 29;
        if (d < dim) break;
        d -= dim;
    }

    snprintf(buf, len, "%04d-%02d-%02d", year, month + 1, d + 1);
}

void pld_fmt_timestamp(u32 timestamp, char *buf, size_t len)
{
    /* Convert seconds-since-2000 to days + hour */
    u32 total_days = timestamp / 86400;
    u32 hour       = (timestamp % 86400) / 3600;

    char date_buf[12];
    pld_fmt_date((u16)total_days, date_buf, sizeof(date_buf));
    snprintf(buf, len, "%s %02lu:00", date_buf, (unsigned long)hour);
}
