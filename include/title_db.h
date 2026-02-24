#pragma once
#include <3ds.h>

/* One entry in the compiled title database */
typedef struct {
    u64  title_id;
    char name[64]; /* UTF-8, null-terminated, max 63 chars */
} TitleDbEntry;

/* Sorted array defined in source/title_db_data.c (generated). */
extern const TitleDbEntry title_db[];
extern const int          title_db_count;

/* Binary-search lookup by title_id.
 * Returns a pointer to a static read-only name string,
 * or NULL if the title_id is not in the database. */
const char *title_db_lookup(u64 title_id);
