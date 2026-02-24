#pragma once
#include <3ds.h>
#include <stdbool.h>

#define TITLE_NAMES_PATH  "sdmc:/3ds/activity-log-pp/title_names.dat"
#define TITLE_NAMES_MAX   1024   /* max entries in the in-memory store */
#define TITLE_NAME_LEN    64     /* max UTF-8 bytes incl. null terminator */

typedef struct {
    u64  title_id;
    char name[TITLE_NAME_LEN];
} TitleNameEntry;   /* 72 bytes */

/* Load persisted title names from SD into the in-memory store.
 * Safe to call even if the file does not exist. */
void        title_names_load(void);

/* Enumerate installed NAND/SD/cartridge titles via AM service,
 * read each title's SMDH, and add new names to the store.
 * Returns the number of new entries added. */
int         title_names_scan_installed(void);

/* Binary-search the in-memory store for a title ID.
 * Returns a pointer to the stored name, or NULL if not found.
 * Does NOT fall back to the embedded title_db. */
const char *title_name_lookup(u64 title_id);

/* Merge an external array of entries into the in-memory store (add-only).
 * Returns the number of new entries added. */
int         title_names_merge(const TitleNameEntry *entries, int count);

/* Return a pointer to the start of the internal sorted array and its size.
 * The pointer is valid until the next call to merge or scan. */
void        title_names_get_all(const TitleNameEntry **out, int *count);

/* Write the current in-memory store to TITLE_NAMES_PATH on SD. */
Result      title_names_save(void);

/* Reset the in-memory store (static storage — nothing to free). */
void        title_names_free(void);
