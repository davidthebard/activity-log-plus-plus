#pragma once
#include <3ds.h>
#include "pld.h"

u32  load_sync_count(void);
void save_sync_count(u32 n);

void run_sync_flow(PldFile *pld, PldSessionLog *sessions,
                   u32 *sync_count, char *status_msg, int status_msg_len);
