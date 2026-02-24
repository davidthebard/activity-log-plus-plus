#include "title_names.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/*
 * ARCHIVE_SAVEDATA_AND_CONTENT gives access to a title's ExeFS content,
 * which includes /icon.bin (the SMDH file containing the title's name and
 * icon).  Archive path is {programID_low, programID_high, mediaType, 0}.
 * Accessible from homebrew running under CFW (Luma3DS etc.).
 */
#define TITLE_CONTENT_ARCHIVE ((FS_ArchiveID)0x2345678A)

/* ── In-memory store (sorted by title_id ascending) ─────────────── */

static TitleNameEntry s_entries[TITLE_NAMES_MAX];
static int            s_count = 0;

/* Binary search: returns index of title_id if found (>= 0),
 * or -(insertion_point + 1) if not found. */
static int bsearch_id(u64 title_id)
{
    int lo = 0, hi = s_count - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        if (s_entries[mid].title_id == title_id) return mid;
        if (s_entries[mid].title_id < title_id)  lo = mid + 1;
        else                                       hi = mid - 1;
    }
    return -(lo + 1);
}

/* Insert entry in sorted order; returns true if added, false if duplicate or full. */
static bool insert_entry(u64 title_id, const char *name)
{
    int idx = bsearch_id(title_id);
    if (idx >= 0) return false;            /* already present */
    if (s_count >= TITLE_NAMES_MAX) return false;

    int ins = -(idx + 1);
    memmove(&s_entries[ins + 1], &s_entries[ins],
            (size_t)(s_count - ins) * sizeof(TitleNameEntry));
    s_entries[ins].title_id = title_id;
    memset(s_entries[ins].name, 0, TITLE_NAME_LEN);
    strncpy(s_entries[ins].name, name, TITLE_NAME_LEN - 1);
    s_count++;
    return true;
}

/* ── UTF-16LE → UTF-8 ──────────────────────────────────────────── */

static void utf16le_to_utf8(const u16 *src, int src_len,
                             char *dst,       int dst_max)
{
    int di = 0;
    for (int i = 0; i < src_len && di < dst_max - 1; i++) {
        u32 cp = src[i];
        if (cp == 0) break;

        if (cp < 0x80) {
            dst[di++] = (char)cp;
        } else if (cp < 0x800) {
            if (di + 2 > dst_max - 1) break;
            dst[di++] = (char)(0xC0 | (cp >> 6));
            dst[di++] = (char)(0x80 | (cp & 0x3F));
        } else if (cp >= 0xD800 && cp <= 0xDFFF) {
            /* surrogate pair — skip */
        } else {
            if (di + 3 > dst_max - 1) break;
            dst[di++] = (char)(0xE0 | (cp >> 12));
            dst[di++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            dst[di++] = (char)(0x80 | (cp & 0x3F));
        }
    }
    dst[di] = '\0';
}

/* ── SMDH reading ──────────────────────────────────────────────── */

/*
 * SMDH layout (offsets from start of file):
 *   0x000  4 B   magic "SMDH"
 *   0x004  2 B   version
 *   0x006  2 B   reserved
 *   0x008  start of applicationTitles[16], each 0x200 bytes:
 *            +0x000  64 × u16  short description (UTF-16LE)
 *            +0x080  128 × u16 long description
 *            +0x180  64 × u16  publisher
 *   Language index: 0=JPN, 1=ENG, 2=FRE, ...
 *
 * We read the first 8 + 2*0x200 = 1032 bytes, covering JPN and ENG entries.
 */
#define SMDH_READ_SIZE  1032u   /* 8 + 2 × 0x200 */

static bool read_title_name(FS_MediaType media, u64 title_id, char *name_out)
{
    u32 path_data[4] = {
        (u32)(title_id & 0xFFFFFFFF),
        (u32)(title_id >> 32),
        (u32)media,
        0
    };
    FS_Path arch_path = { PATH_BINARY, sizeof(path_data), path_data };

    Handle fh = 0;
    Result rc = FSUSER_OpenFileDirectly(
        &fh,
        TITLE_CONTENT_ARCHIVE,
        arch_path,
        fsMakePath(PATH_ASCII, "/icon.bin"),
        FS_OPEN_READ, 0
    );
    if (R_FAILED(rc)) return false;

    u8 buf[SMDH_READ_SIZE];
    u32 bytes_read = 0;
    rc = FSFILE_Read(fh, &bytes_read, 0, buf, sizeof(buf));
    FSFILE_Close(fh);

    if (R_FAILED(rc) || bytes_read < SMDH_READ_SIZE) return false;

    /* Verify SMDH magic */
    if (buf[0] != 'S' || buf[1] != 'M' || buf[2] != 'D' || buf[3] != 'H')
        return false;

    /* Short description: offset 0x008 for JPN, 0x208 for ENG */
    const u16 *eng = (const u16 *)(buf + 8 + 1 * 0x200);   /* 0x208 */
    const u16 *jpn = (const u16 *)(buf + 8 + 0 * 0x200);   /* 0x008 */
    const u16 *chosen = (eng[0] != 0) ? eng : jpn;
    if (chosen[0] == 0) return false;

    utf16le_to_utf8(chosen, 64, name_out, TITLE_NAME_LEN);
    return name_out[0] != '\0';
}

/* ── Public API ────────────────────────────────────────────────── */

void title_names_load(void)
{
    FILE *f = fopen(TITLE_NAMES_PATH, "rb");
    if (!f) return;

    u32 file_count = 0;
    if (fread(&file_count, sizeof(file_count), 1, f) != 1) {
        fclose(f);
        return;
    }
    if (file_count > (u32)TITLE_NAMES_MAX)
        file_count = (u32)TITLE_NAMES_MAX;

    TitleNameEntry tmp;
    for (u32 i = 0; i < file_count; i++) {
        if (fread(&tmp, sizeof(tmp), 1, f) != 1) break;
        tmp.name[TITLE_NAME_LEN - 1] = '\0';   /* safety */
        insert_entry(tmp.title_id, tmp.name);
    }
    fclose(f);
}

int title_names_scan_installed(void)
{
    int added = 0;
    Result rc = amInit();
    if (R_FAILED(rc)) return 0;

    static const FS_MediaType media_types[] = {
        MEDIATYPE_NAND, MEDIATYPE_SD, MEDIATYPE_GAME_CARD
    };
    for (int m = 0; m < 3; m++) {
        FS_MediaType media = media_types[m];
        u32 count = 0;
        rc = AM_GetTitleCount(media, &count);
        if (R_FAILED(rc) || count == 0) continue;

        u64 *ids = (u64 *)malloc(count * sizeof(u64));
        if (!ids) continue;

        u32 read_count = 0;
        rc = AM_GetTitleList(&read_count, media, count, ids);
        if (R_SUCCEEDED(rc)) {
            for (u32 i = 0; i < read_count; i++) {
                if (title_name_lookup(ids[i])) continue;   /* already known */
                char name[TITLE_NAME_LEN];
                if (read_title_name(media, ids[i], name))
                    if (insert_entry(ids[i], name)) added++;
            }
        }
        free(ids);
    }

    amExit();
    return added;
}

const char *title_name_lookup(u64 title_id)
{
    int idx = bsearch_id(title_id);
    return (idx >= 0) ? s_entries[idx].name : NULL;
}

int title_names_merge(const TitleNameEntry *entries, int count)
{
    int added = 0;
    for (int i = 0; i < count; i++) {
        TitleNameEntry tmp = entries[i];
        tmp.name[TITLE_NAME_LEN - 1] = '\0';   /* safety */
        if (insert_entry(tmp.title_id, tmp.name)) added++;
    }
    return added;
}

void title_names_get_all(const TitleNameEntry **out, int *count)
{
    *out   = s_entries;
    *count = s_count;
}

Result title_names_save(void)
{
    FILE *f = fopen(TITLE_NAMES_PATH, "wb");
    if (!f) return -1;

    u32 cnt = (u32)s_count;
    if (fwrite(&cnt, sizeof(cnt), 1, f) != 1) {
        fclose(f);
        return -1;
    }
    if (s_count > 0 &&
        (int)fwrite(s_entries, sizeof(TitleNameEntry), s_count, f) != s_count) {
        fclose(f);
        return -1;
    }
    fclose(f);
    return 0;
}

void title_names_free(void)
{
    s_count = 0;
}
