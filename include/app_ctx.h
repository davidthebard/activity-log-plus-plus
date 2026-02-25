#pragma once
#include <3ds.h>
#include "pld.h"
#include "settings.h"
#include "render_views.h"

typedef struct {
    /* Core data (owned, may be replaced by restore/reset) */
    PldFile        pld;
    PldSessionLog  sessions;

    /* User preferences (persisted to SD) */
    AppSettings    settings;
    HiddenGames    hidden;

    /* Filtered/sorted view of pld.summaries */
    const PldSummary *valid[PLD_SUMMARY_COUNT];
    int               n;
    bool              show_system;
    bool              show_unknown;
    ViewMode          view_mode;

    /* Rankings (rebuilt from valid[]) */
    const PldSummary *ranked[RANK_MAX];
    u32               rank_metric[RANK_MAX];
    int               rank_count;

    /* Sync counter */
    u32  sync_count;

    /* Status bar message */
    char status_msg[48];

    /* List selection state */
    int   sel;
    int   scroll_top;
    float scroll_y;

    /* Ranking selection state */
    int   rank_sel;
    int   rank_scroll;

    /* Animation frame counters */
    int   list_anim_frame;
    int   rank_anim_frame;

    /* Region IDs for NAND access (read-only, points to static array) */
    const u32 *region_ids;
    int        region_count;
} AppCtx;

/*
 * Rebuild valid[] from current pld/settings/hidden/filters, then
 * re-sort or rebuild rankings based on view_mode, and reset
 * selection/scroll/animation state to zero.
 */
void app_ctx_rebuild(AppCtx *ctx);
