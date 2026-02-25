#pragma once
#include <3ds.h>
#include "pld.h"

typedef struct {
    const PldFile       *pld;
    const PldSessionLog *sessions;
    Result               rc;
} ExportArgs;

Result export_data(const PldFile *pld, const PldSessionLog *sessions);
void   export_work(void *raw);
