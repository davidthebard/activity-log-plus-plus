#pragma once
#include <3ds.h>
#include "pld.h"

/* ── View modes ─────────────────────────────────────────────────── */

typedef enum {
    VIEW_LAST_PLAYED,
    VIEW_PLAYTIME,
    VIEW_LAUNCHES,
    VIEW_AVG_SESSION,
    VIEW_FIRST_PLAYED,
    VIEW_NAME,
    VIEW_COUNT
} ViewMode;

extern const char *view_labels[VIEW_COUNT];

static inline bool view_is_rank(ViewMode m) {
    return m == VIEW_PLAYTIME || m == VIEW_LAUNCHES || m == VIEW_AVG_SESSION;
}

#define RANK_MAX 10

/* ── Navigation ─────────────────────────────────────────────────── */

u32   nav_tick(u32 keys_down, u32 keys_held);
void  nav_reset(void);
float lerpf(float a, float b, float t);

/* ── Data helpers ───────────────────────────────────────────────── */

bool should_show(u64 title_id, bool show_system);
int  collect_valid(const PldFile *pld,
                   const PldSummary *valid[PLD_SUMMARY_COUNT],
                   bool show_system, bool show_unknown);
void sort_valid(const PldSummary *valid[], int n,
                ViewMode mode, const PldSessionLog *sessions,
                const PldFile *pld);
int  build_rankings(const PldSummary *valid[], int n,
                    ViewMode mode, const PldSessionLog *sessions,
                    const PldFile *pld,
                    const PldSummary *ranked[RANK_MAX],
                    u32 rank_metric_out[RANK_MAX]);

void fmt_backup_label(const char *name, char *out, size_t len);

/* ── Detail screen constants ────────────────────────────────────── */

#define DETAIL_ROW_H   16
#define DETAIL_LIST_Y  170
#define DETAIL_VISIBLE ((240 - DETAIL_LIST_Y) / DETAIL_ROW_H)

/* ── Rendering ──────────────────────────────────────────────────── */

void render_game_list(const PldSummary *const valid[], int n,
                      int sel, float scroll_y,
                      const PldSessionLog *sessions,
                      const char *status_msg,
                      bool show_system, bool show_unknown,
                      ViewMode mode, float anim_t,
                      float sel_pop);

void render_bottom_stats(const PldSummary *valid[], int n,
                         const PldSessionLog *sessions,
                         u32 sync_count,
                         const char *status_msg,
                         bool show_system, bool show_unknown);

void render_rankings_top(const PldSummary *ranked[], int rank_count,
                         int rank_sel, int rank_scroll,
                         const u32 rank_metric[], ViewMode mode,
                         float anim_t, float sel_pop);

void render_detail_top(const PldSummary *s, const char *name,
                       const PldSessionLog *sessions,
                       const int *sess_indices, int sess_count,
                       int detail_scroll);
void render_detail_bot(void);

void render_menu(int sel);
