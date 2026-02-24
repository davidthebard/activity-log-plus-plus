#include "product_code_db.h"

const char *product_code_db_lookup(u64 title_id)
{
    int lo = 0, hi = product_code_db_count - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        if (product_code_db[mid].title_id == title_id) return product_code_db[mid].code;
        if (product_code_db[mid].title_id < title_id)  lo = mid + 1;
        else                                            hi = mid - 1;
    }
    return NULL;
}
