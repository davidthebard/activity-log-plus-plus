#pragma once
#include "app_ctx.h"

/* Game detail screen. Opens a sub-loop; returns when user presses B or X. */
void run_detail_view(AppCtx *ctx, const PldSummary *game);

/* Settings editor. Opens a sub-loop; saves on B. */
void run_settings_view(AppCtx *ctx);

/* Backup restore chooser. Opens a sub-loop; restores selected backup. */
void run_restore_view(AppCtx *ctx);

/* Reset confirmation + execution. Opens a sub-loop; resets to NAND data. */
void run_reset_view(AppCtx *ctx);
