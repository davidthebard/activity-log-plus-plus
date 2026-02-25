# Refactor: Extract modal views from main.c

## Problem

`main.c` is 882 lines. Four modal view loops (detail, settings, restore chooser, reset confirmation) are inlined inside the menu `switch` at 4-5 levels of nesting. Each one runs its own `while (aptMainLoop())` sub-loop, directly mutates 10+ local variables from `main()`, and duplicates the same "rebuild valid list + reset selection" post-action pattern. Adding any new modal view (favorites editor, search, date-range picker, etc.) will make this worse.

## Approach

1. Introduce an `AppCtx` struct that bundles the shared mutable state these views need.
2. Extract a `app_ctx_rebuild` helper that encapsulates the repeated "collect_valid + sort/rank + reset selection" pattern (currently duplicated 7 times).
3. Move each modal view loop into its own function in a new `modal_views.c`, each taking `AppCtx *`.
4. `main()` becomes a thin orchestrator: startup, main loop, menu dispatch (one-line calls), and rendering.

## Shared state inventory

Every modal view reads or writes some subset of these locals currently scattered across `main()`:

| Variable | Used by | Mutable? |
|---|---|---|
| `pld` | restore, reset, sync | replaced wholesale |
| `sessions` | restore, reset, sync, detail | replaced wholesale |
| `app_settings` | settings | written |
| `hidden` | detail | toggled |
| `valid[]`, `n` | all (rebuilt after changes) | rebuilt |
| `show_system`, `show_unknown` | rebuild | read |
| `view_mode` | restore, reset (reset to LAST_PLAYED) | written |
| `sync_count` | reset (zeroed) | written |
| `status_msg` | all (status feedback) | written |
| `sel`, `scroll_top`, `scroll_y` | rebuild (reset to 0) | written |
| `rank_sel`, `rank_scroll`, `rank_count` | rebuild (reset to 0) | written |
| `ranked[]`, `rank_metric[]` | rebuild | rebuilt |
| `list_anim_frame`, `rank_anim_frame` | rebuild (reset to 0) | written |
| `region_ids` | reset | read |

## File changes

### 1. New: `include/app_ctx.h`

```c
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
```

### 2. New: `source/app_ctx.c`

Implements the single rebuild helper that currently appears as ~12 duplicated lines in 7 locations:

```c
#include "app_ctx.h"

void app_ctx_rebuild(AppCtx *ctx)
{
    ctx->n = collect_valid(&ctx->pld, ctx->valid,
                           ctx->show_system, ctx->show_unknown,
                           ctx->settings.min_play_secs, &ctx->hidden);
    if (view_is_rank(ctx->view_mode)) {
        ctx->rank_count = build_rankings(
            ctx->valid, ctx->n, ctx->view_mode,
            &ctx->sessions, &ctx->pld,
            ctx->ranked, ctx->rank_metric);
        ctx->rank_sel    = 0;
        ctx->rank_scroll = 0;
        ctx->rank_anim_frame = 0;
    } else {
        sort_valid(ctx->valid, ctx->n, ctx->view_mode,
                   &ctx->sessions, &ctx->pld);
        ctx->sel        = 0;
        ctx->scroll_top = 0;
        ctx->scroll_y   = 0.0f;
        ctx->list_anim_frame = 0;
    }
}
```

### 3. New: `include/modal_views.h` + `source/modal_views.c`

Each modal view becomes a self-contained function:

```c
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
```

Each function is a direct lift of the existing inline code, with local variables replaced by `ctx->` accesses. For example, `run_detail_view` would contain:

- The session-index building loop (currently lines 749-766)
- The `while (!detail_done && aptMainLoop())` input/render loop (lines 771-805)
- KEY_X hide toggle with `hidden_toggle` + `hidden_save` + `app_ctx_rebuild`
- `free(det_indices)` cleanup

The rendering functions (`render_detail_top`, `render_detail_bot`, `render_settings_top`, etc.) stay in `render_views.c` since they are pure draw calls. Only the input loops and state mutation move.

### 4. Modify: `source/main.c`

**Before (882 lines):**
- Lines 1-156: Worker structs + startup helpers (keep as-is)
- Lines 160-265: `main()` startup sequence (keep, but init `AppCtx` instead of bare locals)
- Lines 266-296: Local state declarations (move into `AppCtx` init)
- Lines 297-871: Main input loop (simplify menu dispatch)
- Lines 872-882: Cleanup (unchanged)

**After (~450 lines estimated):**

Startup initializes `AppCtx` on the stack:

```c
AppCtx ctx;
memset(&ctx, 0, sizeof(ctx));
ctx.region_ids    = region_ids;
ctx.region_count  = 4;
/* ... copy pld, sessions into ctx after loading ... */
settings_load(&ctx.settings);
hidden_load(&ctx.hidden);
ctx.view_mode = (ViewMode)ctx.settings.starting_view;
if (ctx.view_mode >= VIEW_COUNT) ctx.view_mode = VIEW_LAST_PLAYED;
app_ctx_rebuild(&ctx);
```

Menu dispatch becomes:

```c
case 4: /* Restore */
    run_restore_view(&ctx);
    menu_open = false;
    break;

case 5: /* Reset */
    run_reset_view(&ctx);
    menu_open = false;
    break;

case 6: /* Settings */
    run_settings_view(&ctx);
    menu_open = false;
    break;
```

Detail view becomes:

```c
if (det_s) {
    run_detail_view(&ctx, det_s);
}
```

The main-loop rendering section (lines 826-871) accesses `ctx.` fields instead of bare locals but is otherwise unchanged.

All 7 occurrences of the rebuild pattern (after sync, restore, reset, settings save, hide toggle, filter change, view-mode change) become a single `app_ctx_rebuild(&ctx)` call.

### 5. No Makefile changes

Both `app_ctx.c` and `modal_views.c` in `source/` are auto-discovered by the wildcard build rules.

## Line count impact

| File | Before | After (est.) |
|---|---|---|
| `main.c` | 882 | ~450 |
| `app_ctx.c` | -- | ~30 |
| `modal_views.c` | -- | ~320 |
| `render_views.c` | 765 | 765 (unchanged) |

Total code increases by ~35 lines (the `AppCtx` struct + rebuild helper), but `main.c` drops by ~430 lines and max nesting goes from 7 to 3.

## Migration order

1. Create `app_ctx.h` / `app_ctx.c` with the struct and `app_ctx_rebuild`.
2. Refactor `main.c` to use `AppCtx` for all state (no functional change yet, just rename locals to `ctx.field`). Verify it compiles.
3. Extract `run_restore_view` -- simplest modal, no render function changes needed.
4. Extract `run_reset_view` -- similar pattern.
5. Extract `run_settings_view`.
6. Extract `run_detail_view`.
7. Replace all remaining inline rebuild blocks in `main.c` with `app_ctx_rebuild(&ctx)`.
8. Final review: verify all `ctx.` field accesses, confirm no dangling local references.

## What stays in main.c

- Worker arg structs and thread functions (startup/reset I/O) -- these are only used during init and the reset flow, not shared across views.
- The main `while (aptMainLoop())` loop with charts-view rendering, menu overlay rendering, and game-list/rankings rendering. These are not modal sub-loops and remain naturally part of the main loop.
- Startup sequence (7-step spinner chain).
- Cleanup (free sessions, icons, names, ui_fini, etc.).
