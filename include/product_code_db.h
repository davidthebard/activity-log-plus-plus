#pragma once
#include <3ds.h>

/* One entry in the compiled product-code database */
typedef struct {
    u64  title_id;
    char code[5]; /* 4-char GameTDB product code, null-terminated */
} ProductCodeEntry;

/* Sorted array defined in source/product_code_db_data.c (generated). */
extern const ProductCodeEntry product_code_db[];
extern const int              product_code_db_count;

/* Binary-search lookup by title_id.
 * Returns a pointer to the 4-char code string (read-only, null-terminated),
 * or NULL if the title_id is not in the database. */
const char *product_code_db_lookup(u64 title_id);
