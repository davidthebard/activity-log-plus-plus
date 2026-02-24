#include "title_db.h"

const char *title_db_lookup(u64 title_id)
{
    int lo = 0, hi = title_db_count - 1;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        if (title_db[mid].title_id == title_id) return title_db[mid].name;
        if (title_db[mid].title_id < title_id)  lo = mid + 1;
        else                                     hi = mid - 1;
    }
    return NULL;
}
