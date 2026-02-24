#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <3ds.h>

#include "pld.h"
#include "net.h"
#include "ui.h"
#include "title_db.h"
#include "title_names.h"
#include "title_icons.h"
#include "icon_fetch.h"

#include <math.h>

/* ── Constants ──────────────────────────────────────────────────── */

#define SYNC_COUNT_PATH  "sdmc:/3ds/activity-log-pp/synccount"

/* Region save IDs for Activity Log (used at startup and reset) */
static const u32 region_ids[] = {
    ACTIVITY_SAVE_ID_USA,
    ACTIVITY_SAVE_ID_EUR,
    ACTIVITY_SAVE_ID_JPN,
    ACTIVITY_SAVE_ID_KOR,
};

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ── Rankings tabs ─────────────────────────────────────────────── */

typedef enum {
    RANK_PLAYTIME,   /* "Top Playtime"  — total_secs descending      */
    RANK_LAUNCHES,   /* "Top Launches"  — launch_count descending    */
    RANK_SESSIONS,   /* "Top Sessions"  — session count descending   */
    RANK_RECENT,     /* "Most Recent"   — last_played_days descending */
    RANK_TAB_COUNT
} RankingsTab;

static const char *rank_tab_labels[RANK_TAB_COUNT] = {
    "Top Playtime", "Top Launches", "Top Sessions", "Most Recent"
};

#define RANK_MAX 10

/* ── Sort modes ─────────────────────────────────────────────────── */

typedef enum {
    SORT_LAST_PLAYED,   /* default — most recently played first */
    SORT_PLAYTIME,      /* total_secs descending                */
    SORT_LAUNCHES,      /* launch_count descending              */
    SORT_SESSIONS,      /* session count descending             */
    SORT_FIRST_PLAYED,  /* first_played_days descending (newest first) */
    SORT_NAME,          /* alphabetical A-Z                     */
    SORT_COUNT
} SortMode;

static const char *sort_labels[SORT_COUNT] = {
    "Last Played", "Playtime", "Launches", "Sessions", "First Played", "Name"
};

/* File-static session count cache for the SORT_SESSIONS comparator.
 * Indexed by the summary's position in the full summaries[256] array.
 * sort_summaries_base points to summaries[0] so the comparator can
 * recover the index via pointer arithmetic. */
static int sess_counts_cache[PLD_SUMMARY_COUNT];
static const PldSummary *sort_summaries_base;

/* ── Sync counter helpers ────────────────────────────────────────── */

static u32 load_sync_count(void) {
    u32 n = 0;
    FILE *f = fopen(SYNC_COUNT_PATH, "rb");
    if (f) { fread(&n, sizeof(n), 1, f); fclose(f); }
    return n;
}

static void save_sync_count(u32 n) {
    FILE *f = fopen(SYNC_COUNT_PATH, "wb");
    if (f) { fwrite(&n, sizeof(n), 1, f); fclose(f); }
}

/* ── Sort comparators ──────────────────────────────────────────── */

static int cmp_last_played(const void *a, const void *b) {
    const PldSummary *sa = *(const PldSummary *const *)a;
    const PldSummary *sb = *(const PldSummary *const *)b;
    if (sb->last_played_days != sa->last_played_days)
        return (int)sb->last_played_days - (int)sa->last_played_days;
    return 0;
}

static int cmp_playtime(const void *a, const void *b) {
    const PldSummary *sa = *(const PldSummary *const *)a;
    const PldSummary *sb = *(const PldSummary *const *)b;
    if (sb->total_secs != sa->total_secs)
        return (sb->total_secs > sa->total_secs) ? 1 : -1;
    return 0;
}

static int cmp_launches(const void *a, const void *b) {
    const PldSummary *sa = *(const PldSummary *const *)a;
    const PldSummary *sb = *(const PldSummary *const *)b;
    if (sb->launch_count != sa->launch_count)
        return (int)sb->launch_count - (int)sa->launch_count;
    return 0;
}

static int cmp_sessions(const void *a, const void *b) {
    const PldSummary *sa = *(const PldSummary *const *)a;
    const PldSummary *sb = *(const PldSummary *const *)b;
    int ia = (int)(sa - sort_summaries_base);
    int ib = (int)(sb - sort_summaries_base);
    return sess_counts_cache[ib] - sess_counts_cache[ia];
}

static int cmp_first_played(const void *a, const void *b) {
    const PldSummary *sa = *(const PldSummary *const *)a;
    const PldSummary *sb = *(const PldSummary *const *)b;
    /* Ascending: oldest (smallest days value) first */
    if (sa->first_played_days != sb->first_played_days)
        return (int)sa->first_played_days - (int)sb->first_played_days;
    return 0;
}

static int cmp_name(const void *a, const void *b) {
    const PldSummary *sa = *(const PldSummary *const *)a;
    const PldSummary *sb = *(const PldSummary *const *)b;
    const char *na = title_name_lookup(sa->title_id);
    if (!na) na = title_db_lookup(sa->title_id);
    if (!na) na = "";
    const char *nb = title_name_lookup(sb->title_id);
    if (!nb) nb = title_db_lookup(sb->title_id);
    if (!nb) nb = "";
    return strcasecmp(na, nb);
}

static void sort_valid(const PldSummary *valid[], int n,
                       SortMode mode, const PldSessionLog *sessions,
                       const PldFile *pld)
{
    if (n <= 1) return;

    /* Pre-compute session counts if needed.
     * Cache is indexed by summary slot (offset from pld->summaries[0]),
     * so it survives qsort reordering of valid[]. */
    if (mode == SORT_SESSIONS) {
        sort_summaries_base = pld->summaries;
        for (int i = 0; i < n; i++) {
            int slot = (int)(valid[i] - pld->summaries);
            sess_counts_cache[slot] = pld_count_sessions_for(sessions, valid[i]->title_id);
        }
    }

    typedef int (*cmp_fn)(const void *, const void *);
    static const cmp_fn comparators[SORT_COUNT] = {
        cmp_last_played, cmp_playtime, cmp_launches,
        cmp_sessions, cmp_first_played, cmp_name
    };
    qsort(valid, (size_t)n, sizeof(valid[0]), comparators[mode]);
}

/* ── Rankings builder ──────────────────────────────────────────── */

static int rank_sess_cache[PLD_SUMMARY_COUNT];

static int cmp_rank_playtime(const void *a, const void *b) {
    const PldSummary *sa = *(const PldSummary *const *)a;
    const PldSummary *sb = *(const PldSummary *const *)b;
    if (sb->total_secs != sa->total_secs)
        return (sb->total_secs > sa->total_secs) ? 1 : -1;
    return 0;
}

static int cmp_rank_launches(const void *a, const void *b) {
    const PldSummary *sa = *(const PldSummary *const *)a;
    const PldSummary *sb = *(const PldSummary *const *)b;
    if (sb->launch_count != sa->launch_count)
        return (int)sb->launch_count - (int)sa->launch_count;
    return 0;
}

static int cmp_rank_sessions(const void *a, const void *b) {
    const PldSummary *sa = *(const PldSummary *const *)a;
    const PldSummary *sb = *(const PldSummary *const *)b;
    int ia = (int)(sa - sort_summaries_base);
    int ib = (int)(sb - sort_summaries_base);
    return rank_sess_cache[ib] - rank_sess_cache[ia];
}

static int cmp_rank_recent(const void *a, const void *b) {
    const PldSummary *sa = *(const PldSummary *const *)a;
    const PldSummary *sb = *(const PldSummary *const *)b;
    if (sb->last_played_days != sa->last_played_days)
        return (int)sb->last_played_days - (int)sa->last_played_days;
    return 0;
}

/*
 * Build the top-N rankings from valid[].
 * ranked[] must have RANK_MAX capacity.
 * Returns the number of ranked entries (min(n, RANK_MAX)).
 * rank_sess_out[] receives session counts for each ranked entry (indexed 0..ret-1).
 */
static int build_rankings(const PldSummary *valid[], int n,
                           RankingsTab tab, const PldSessionLog *sessions,
                           const PldFile *pld,
                           const PldSummary *ranked[RANK_MAX],
                           int rank_sess_out[RANK_MAX])
{
    if (n <= 0) return 0;

    /* Copy valid pointers into a temp buffer for sorting */
    const PldSummary **tmp = malloc((size_t)n * sizeof(tmp[0]));
    if (!tmp) return 0;
    memcpy(tmp, valid, (size_t)n * sizeof(tmp[0]));

    /* Pre-compute session counts for RANK_SESSIONS */
    if (tab == RANK_SESSIONS) {
        sort_summaries_base = pld->summaries;
        for (int i = 0; i < n; i++) {
            int slot = (int)(tmp[i] - pld->summaries);
            rank_sess_cache[slot] = pld_count_sessions_for(sessions, tmp[i]->title_id);
        }
    }

    typedef int (*cmp_fn)(const void *, const void *);
    static const cmp_fn rank_cmps[RANK_TAB_COUNT] = {
        cmp_rank_playtime, cmp_rank_launches,
        cmp_rank_sessions, cmp_rank_recent
    };
    qsort(tmp, (size_t)n, sizeof(tmp[0]), rank_cmps[tab]);

    int count = (n < RANK_MAX) ? n : RANK_MAX;
    for (int i = 0; i < count; i++) {
        ranked[i] = tmp[i];
        rank_sess_out[i] = pld_count_sessions_for(sessions, tmp[i]->title_id);
    }
    free(tmp);
    return count;
}

/* ── Pie chart (playtime share) ─────────────────────────────────── */

#define PIE_SLICES 8

typedef struct {
    const PldSummary *s;  /* NULL for "Other" */
    u32 secs;
    float pct;            /* 0.0–1.0 */
} PieSlice;

/* C2D_Color32 is ABGR: 0xFFBBGGRR */
static const u32 pie_colors[PIE_SLICES + 1] = {
    0xFFC8864A,  /* blue   (R=4A G=86 B=C8) */
    0xFF4A6AC8,  /* orange (R=C8 G=6A B=4A) */
    0xFF78C84A,  /* green  (R=4A G=C8 B=78) */
    0xFF8A4AC8,  /* pink   (R=C8 G=4A B=8A) */
    0xFFC84A8A,  /* purple (R=8A G=4A B=C8) */
    0xFF4ABEC8,  /* yellow (R=C8 G=BE B=4A) */
    0xFFC8B44A,  /* teal   (R=4A G=B4 B=C8) */
    0xFF4A4AC8,  /* red    (R=C8 G=4A B=4A) */
    0xFF999999,  /* grey (Other)             */
};

static int cmp_pie_playtime(const void *a, const void *b) {
    const PldSummary *sa = *(const PldSummary *const *)a;
    const PldSummary *sb = *(const PldSummary *const *)b;
    if (sb->total_secs != sa->total_secs)
        return (sb->total_secs > sa->total_secs) ? 1 : -1;
    return 0;
}

/*
 * Build pie chart data from valid[].
 * slices[] must have PIE_SLICES+1 capacity.
 * Returns slice count (including "Other" if present).
 */
static int build_pie_data(const PldSummary *valid[], int n,
                          PieSlice slices[], u32 *total_out)
{
    if (n <= 0) { *total_out = 0; return 0; }

    /* Sort a copy by playtime descending */
    const PldSummary **tmp = malloc((size_t)n * sizeof(tmp[0]));
    if (!tmp) { *total_out = 0; return 0; }
    memcpy(tmp, valid, (size_t)n * sizeof(tmp[0]));
    qsort(tmp, (size_t)n, sizeof(tmp[0]), cmp_pie_playtime);

    u32 total = 0;
    for (int i = 0; i < n; i++) total += tmp[i]->total_secs;
    *total_out = total;
    if (total == 0) { free(tmp); return 0; }

    int take = (n < PIE_SLICES) ? n : PIE_SLICES;
    int sc = 0;
    u32 top_sum = 0;
    for (int i = 0; i < take; i++) {
        slices[sc].s    = tmp[i];
        slices[sc].secs = tmp[i]->total_secs;
        slices[sc].pct  = (float)tmp[i]->total_secs / (float)total;
        top_sum += tmp[i]->total_secs;
        sc++;
    }

    /* "Other" slice for remainder */
    if (n > PIE_SLICES && total > top_sum) {
        slices[sc].s    = NULL;
        slices[sc].secs = total - top_sum;
        slices[sc].pct  = (float)(total - top_sum) / (float)total;
        sc++;
    }

    free(tmp);
    return sc;
}

static void draw_pie_slice(float cx, float cy, float r,
                           float start_rad, float end_rad, u32 color)
{
    float step = 5.0f * (float)M_PI / 180.0f; /* ~5 degrees */
    float angle = start_rad;
    while (angle < end_rad - 0.001f) {
        float next = angle + step;
        if (next > end_rad) next = end_rad;
        float x0 = cx + r * cosf(angle);
        float y0 = cy + r * sinf(angle);
        float x1 = cx + r * cosf(next);
        float y1 = cy + r * sinf(next);
        ui_draw_triangle(cx, cy, x0, y0, x1, y1, color);
        angle = next;
    }
}

static void render_pie_top(const PieSlice slices[], int slice_count, u32 total,
                           float anim_t)
{
    /* Header */
    ui_draw_rect(0, 0, UI_TOP_W, UI_HEADER_H, UI_COL_HEADER);
    ui_draw_text(6, 4, UI_SCALE_HDR, UI_COL_HEADER_TXT, "Charts: Pie");

    if (slice_count == 0 || total == 0) {
        ui_draw_text(8, 36, UI_SCALE_LG, UI_COL_TEXT_DIM, "No playtime data");
        return;
    }

    /* Pie chart on left side — sweep in over anim_t (complete by t=1) */
    float cx = 110.0f, cy = 132.0f, r = 80.0f;
    float angle = -((float)M_PI / 2.0f); /* start at 12 o'clock */
    float pie_t = (anim_t > 1.0f) ? 1.0f : anim_t;
    float max_sweep = pie_t * 2.0f * (float)M_PI;
    float cumulative = 0.0f;
    for (int i = 0; i < slice_count; i++) {
        float sweep = slices[i].pct * 2.0f * (float)M_PI;
        if (sweep < 0.001f) { cumulative += sweep; continue; }
        if (cumulative >= max_sweep) break;
        float draw_sweep = sweep;
        if (cumulative + draw_sweep > max_sweep)
            draw_sweep = max_sweep - cumulative;
        draw_pie_slice(cx, cy, r, angle, angle + draw_sweep, pie_colors[i]);
        angle += draw_sweep;
        cumulative += sweep;
    }

    /* Legend on right side — staggered cascade fade-in + slide */
    float lx = 210.0f, ly_base = 30.0f;
    char fallback[32];
    for (int i = 0; i < slice_count; i++) {
        /* Per-row cascade fade + vertical slide */
        float stagger = 0.24f;
        float fade_len = 0.30f;
        float row_start = (float)i * stagger;
        float reveal = (anim_t <= row_start) ? 0.0f
                     : (anim_t >= row_start + fade_len) ? 1.0f
                     : (anim_t - row_start) / fade_len;
        u8 alpha = (u8)(reveal * 255.0f);
        if (alpha == 0) continue;
        float y_off = (1.0f - reveal) * -8.0f;
        float ly = ly_base + (float)i * 18.0f + y_off;

        u32 col      = pie_colors[i];
        u32 col_a    = (col & 0x00FFFFFF) | ((u32)alpha << 24);
        u32 txt_col  = (UI_COL_TEXT & 0x00FFFFFF) | ((u32)alpha << 24);

        ui_draw_rect(lx, ly + 2, 10, 10, col_a);

        const char *name;
        if (slices[i].s) {
            name = title_name_lookup(slices[i].s->title_id);
            if (!name) name = title_db_lookup(slices[i].s->title_id);
            if (!name) {
                snprintf(fallback, sizeof(fallback), "0x%016llX",
                         (unsigned long long)slices[i].s->title_id);
                name = fallback;
            }
        } else {
            name = "Other";
        }

        int pct_int = (int)(slices[i].pct * 100.0f + 0.5f);
        char pct_str[8];
        snprintf(pct_str, sizeof(pct_str), "%d%%", pct_int);
        float pct_w = ui_text_width(pct_str, UI_SCALE_SM);
        ui_draw_text_right(396, ly, UI_SCALE_SM, txt_col, pct_str);
        ui_draw_text_trunc(lx + 14, ly, UI_SCALE_SM, txt_col, name,
                           396 - pct_w - 4 - (lx + 14));
    }

    /* Status bar */
    ui_draw_rect(0, 220, UI_TOP_W, UI_STATUS_H, UI_COL_STATUS_BG);
    ui_draw_text_right(396, 222, UI_SCALE_SM, UI_COL_STATUS_TXT, "L/R:tab  B:back");
}

static void render_pie_bot(const PieSlice slices[], int slice_count, u32 total,
                           float anim_t)
{
    /* Header */
    ui_draw_rect(0, 0, UI_BOT_W, UI_HEADER_H, UI_COL_HEADER);
    ui_draw_text(6, 4, UI_SCALE_HDR, UI_COL_HEADER_TXT, "Charts");

    if (slice_count == 0 || total == 0) {
        ui_draw_text(8, 36, UI_SCALE_LG, UI_COL_TEXT_DIM, "No data");
        return;
    }

    /* Detailed breakdown — shrink row height to fit up to 9 slices + total */
    float row_h = 18.0f;
    float y_base = 28.0f;
    float y_last = y_base; /* tracks final y for divider placement */
    char fallback[32];
    char t_buf[20];
    for (int i = 0; i < slice_count; i++) {
        /* Per-row cascade fade + vertical slide */
        float stagger = 0.24f;
        float fade_len = 0.30f;
        float row_start = (float)i * stagger;
        float reveal = (anim_t <= row_start) ? 0.0f
                     : (anim_t >= row_start + fade_len) ? 1.0f
                     : (anim_t - row_start) / fade_len;
        u8 alpha = (u8)(reveal * 255.0f);
        y_last = y_base + (float)(i + 1) * row_h;
        if (alpha == 0) continue;
        float y_off = (1.0f - reveal) * -8.0f;
        float y = y_base + (float)i * row_h + y_off;

        u32 col_a    = (pie_colors[i] & 0x00FFFFFF) | ((u32)alpha << 24);
        u32 txt_col  = (UI_COL_TEXT & 0x00FFFFFF) | ((u32)alpha << 24);
        u32 dim_col  = (UI_COL_TEXT_DIM & 0x00FFFFFF) | ((u32)alpha << 24);

        const char *name;
        if (slices[i].s) {
            name = title_name_lookup(slices[i].s->title_id);
            if (!name) name = title_db_lookup(slices[i].s->title_id);
            if (!name) {
                snprintf(fallback, sizeof(fallback), "0x%016llX",
                         (unsigned long long)slices[i].s->title_id);
                name = fallback;
            }
        } else {
            name = "Other";
        }

        pld_fmt_time(slices[i].secs, t_buf, sizeof(t_buf));
        int pct_int = (int)(slices[i].pct * 100.0f + 0.5f);

        /* Colored dot */
        ui_draw_rect(8, y + 2, 8, 8, col_a);

        char detail[32];
        snprintf(detail, sizeof(detail), "%s  %d%%", t_buf, pct_int);
        float detail_w = ui_text_width(detail, UI_SCALE_SM);
        ui_draw_text_trunc(20, y, UI_SCALE_SM, txt_col, name,
                           (UI_BOT_W - 8) - detail_w - 4 - 20);
        ui_draw_text_right(UI_BOT_W - 8, y, UI_SCALE_SM, dim_col, detail);
    }

    /* Divider + Total — placed after last row */
    float y = y_last;
    y += 2.0f;
    ui_draw_rect(0, y, UI_BOT_W, 1, UI_COL_DIVIDER);
    y += 4.0f;
    pld_fmt_time(total, t_buf, sizeof(t_buf));
    ui_draw_text(8, y, UI_SCALE_SM, UI_COL_TEXT, "Total");
    ui_draw_text_right(UI_BOT_W - 8, y, UI_SCALE_SM, UI_COL_TEXT, t_buf);

    /* Controls hint */
    ui_draw_rect(0, 220, UI_BOT_W, UI_STATUS_H, UI_COL_STATUS_BG);
    ui_draw_text_right(UI_BOT_W - 4, 222, UI_SCALE_SM, UI_COL_STATUS_TXT,
                       "L/R:tab  B:back");
}

/* ── Bar chart (top screen) ───────────────────────────────────── */

static void render_bar_top(const PieSlice slices[], int slice_count, u32 total,
                           float anim_t)
{
    /* Header */
    ui_draw_rect(0, 0, UI_TOP_W, UI_HEADER_H, UI_COL_HEADER);
    ui_draw_text(6, 4, UI_SCALE_HDR, UI_COL_HEADER_TXT, "Charts: Bar");

    if (slice_count == 0 || total == 0) {
        ui_draw_text(8, 36, UI_SCALE_LG, UI_COL_TEXT_DIM, "No playtime data");
        ui_draw_rect(0, 220, UI_TOP_W, UI_STATUS_H, UI_COL_STATUS_BG);
        ui_draw_text_right(396, 222, UI_SCALE_SM, UI_COL_STATUS_TXT, "L/R:tab  B:back");
        return;
    }

    /* Find max for proportional scaling */
    u32 max_secs = 0;
    for (int i = 0; i < slice_count; i++)
        if (slices[i].secs > max_secs) max_secs = slices[i].secs;

    float bar_max_w = 200.0f;
    float row_h = 20.0f;
    float y_base = (float)UI_HEADER_H + 4.0f;
    char fallback[32];
    char t_buf[20];

    for (int i = 0; i < slice_count; i++) {
        /* Per-row cascade fade + vertical slide */
        float stagger = 0.24f;
        float fade_len = 0.30f;
        float row_start = (float)i * stagger;
        float reveal = (anim_t <= row_start) ? 0.0f
                     : (anim_t >= row_start + fade_len) ? 1.0f
                     : (anim_t - row_start) / fade_len;
        u8 alpha = (u8)(reveal * 255.0f);
        if (alpha == 0) continue;
        float y_off = (1.0f - reveal) * -8.0f;
        float y = y_base + (float)i * row_h + y_off;

        u32 bar_col = (pie_colors[i] & 0x00FFFFFF) | ((u32)alpha << 24);
        u32 txt_col = (UI_COL_TEXT & 0x00FFFFFF) | ((u32)alpha << 24);
        u32 dim_col = (UI_COL_TEXT_DIM & 0x00FFFFFF) | ((u32)alpha << 24);

        const char *name;
        if (slices[i].s) {
            name = title_name_lookup(slices[i].s->title_id);
            if (!name) name = title_db_lookup(slices[i].s->title_id);
            if (!name) {
                snprintf(fallback, sizeof(fallback), "0x%016llX",
                         (unsigned long long)slices[i].s->title_id);
                name = fallback;
            }
        } else {
            name = "Other";
        }

        /* Bar — grow from zero during animation */
        float bar_w = (max_secs > 0)
                      ? (float)slices[i].secs / (float)max_secs * bar_max_w
                      : 0.0f;
        bar_w *= reveal;
        if (bar_w < 2.0f && slices[i].secs > 0 && reveal > 0.0f) bar_w = 2.0f;
        ui_draw_rect(8, y + 2, bar_w, 14, bar_col);

        /* Time right-aligned */
        pld_fmt_time(slices[i].secs, t_buf, sizeof(t_buf));
        float time_w = ui_text_width(t_buf, UI_SCALE_SM);
        ui_draw_text_right(396, y + 1, UI_SCALE_SM, dim_col, t_buf);

        /* Name to the right of the bar, truncated to avoid overlapping time */
        ui_draw_text_trunc(bar_max_w + 16, y + 1, UI_SCALE_SM, txt_col, name,
                           396 - time_w - 4 - (bar_max_w + 16));
    }

    /* Status bar */
    ui_draw_rect(0, 220, UI_TOP_W, UI_STATUS_H, UI_COL_STATUS_BG);
    ui_draw_text_right(396, 222, UI_SCALE_SM, UI_COL_STATUS_TXT, "L/R:tab  B:back");
}

/* ── Hold-to-repeat navigation ─────────────────────────────────── */

#define NAV_INITIAL_DELAY  18  /* frames before repeat starts (~300ms) */
#define NAV_REPEAT_RATE     4  /* frames between repeats (~67ms)       */

static u32 nav_held_key    = 0;   /* which direction is held          */
static int nav_held_frames = 0;   /* how many frames it's been held   */

/*
 * Call once per frame AFTER hidScanInput().
 * Returns a bitmask of direction keys that should "fire" this frame
 * (initial press OR hold-repeat).
 */
static u32 nav_tick(u32 keys_down, u32 keys_held)
{
    u32 dir_down = keys_down & (KEY_UP | KEY_DOWN);
    u32 dir_held = keys_held & (KEY_UP | KEY_DOWN);
    u32 fire = 0;

    if (dir_down) {
        /* Fresh press — fire immediately, start tracking */
        nav_held_key    = dir_down;
        nav_held_frames = 0;
        fire = dir_down;
    } else if (dir_held && dir_held == nav_held_key) {
        /* Still holding the same direction */
        nav_held_frames++;
        if (nav_held_frames >= NAV_INITIAL_DELAY &&
            ((nav_held_frames - NAV_INITIAL_DELAY) % NAV_REPEAT_RATE == 0))
            fire = nav_held_key;
    } else {
        /* Released or changed */
        nav_held_key    = 0;
        nav_held_frames = 0;
    }
    return fire;
}

static float lerpf(float a, float b, float t) {
    return a + (b - a) * t;
}

/* ── Helpers ────────────────────────────────────────────────────── */

/* "pld_backup_YYYYMMDD_HHMMSS.dat" → "YYYY-MM-DD HH:MM:SS" */
static void fmt_backup_label(const char *name, char *out, size_t len)
{
    if (strlen(name) == 30)
        snprintf(out, len, "%.4s-%.2s-%.2s %.2s:%.2s:%.2s",
                 name+11, name+15, name+17, name+20, name+22, name+24);
    else
        snprintf(out, len, "%s", name);
}

/*
 * Title category filter.
 * Games:  0x00040000 (3DS apps), 0x00048004 (DSiWare)
 * System: 0x00040010 (system apps), 0x00040030 (applets)
 * Updates/DLC (0x0004000E) are always hidden.
 */
static bool should_show(u64 title_id, bool show_system)
{
    u32 hi = (u32)(title_id >> 32);
    if (hi == 0x00040000 || hi == 0x00048004) return true;
    if (show_system && (hi == 0x00040010 || hi == 0x00040030)) return true;
    return false;
}

/* Build a compact array of pointers to non-empty summary entries. */
static int collect_valid(const PldFile *pld,
                         const PldSummary *valid[PLD_SUMMARY_COUNT],
                         bool show_system, bool show_unknown)
{
    int n = 0;
    for (int i = 0; i < PLD_SUMMARY_COUNT; i++) {
        const PldSummary *s = &pld->summaries[i];
        if (pld_summary_is_empty(s)) continue;
        if (!should_show(s->title_id, show_system)) continue;
        if (!show_unknown && !title_name_lookup(s->title_id)
                          && !title_db_lookup(s->title_id)) continue;
        valid[n++] = s;
    }
    return n;
}

/* ── Transient screen helper ─────────────────────────────────────── */

/*
 * Render one frame with title in the header bar and body text
 * (newline-delimited lines) below it on the top screen.
 * The bottom screen shows a simple "Activity Log++" header.
 */
static void draw_spinner(float cx, float cy)
{
    static int spinner_frame = 0;
    spinner_frame++;

    int active = (spinner_frame / 8) % 8;
    float ring_r = 24.0f;
    float dot_r  = 6.0f;

    for (int i = 0; i < 8; i++) {
        float angle = (float)i * 2.0f * (float)M_PI / 8.0f - (float)M_PI / 2.0f;
        float dx = cx + ring_r * cosf(angle);
        float dy = cy + ring_r * sinf(angle);

        /* Distance behind active dot (wrapping) */
        int dist = (active - i + 8) % 8;
        u8 alpha;
        if      (dist == 0) alpha = 255;
        else if (dist == 1) alpha = 192;
        else if (dist == 2) alpha = 128;
        else if (dist == 3) alpha = 64;
        else                alpha = 40;

        u32 color = C2D_Color32(0x66, 0x66, 0x66, alpha);
        ui_draw_circle(dx, dy, dot_r, color);
    }
}

static void draw_message_screen_ex(const char *title, const char *body,
                                   bool show_spinner)
{
    ui_begin_frame();

    ui_target_top();
    ui_draw_rect(0, 0, UI_TOP_W, UI_HEADER_H, UI_COL_HEADER);
    ui_draw_text(6, 4, UI_SCALE_HDR, UI_COL_HEADER_TXT, title);

    /* Draw body lines split on '\n' */
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s", body);
    char *line = tmp;
    float y = 36.0f;
    while (line && *line) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        ui_draw_text(8, y, UI_SCALE_LG, UI_COL_TEXT, line);
        y += 20.0f;
        line = nl ? nl + 1 : NULL;
    }

    if (show_spinner)
        draw_spinner(UI_TOP_W / 2.0f, 180.0f);

    ui_target_bot();
    ui_draw_rect(0, 0, UI_BOT_W, UI_HEADER_H, UI_COL_HEADER);
    ui_draw_text(6, 4, UI_SCALE_HDR, UI_COL_HEADER_TXT, "Activity Log++");

    ui_end_frame();
}

static void draw_message_screen(const char *title, const char *body)
{
    draw_message_screen_ex(title, body, false);
}

static void draw_loading_screen(const char *title, const char *body)
{
    draw_message_screen_ex(title, body, true);
}

static void draw_progress_screen(const char *title, const char *body,
                                 int step, int total_steps)
{
    ui_begin_frame();

    ui_target_top();
    ui_draw_rect(0, 0, UI_TOP_W, UI_HEADER_H, UI_COL_HEADER);
    ui_draw_text(6, 4, UI_SCALE_HDR, UI_COL_HEADER_TXT, title);

    /* Draw body lines split on '\n' */
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s", body);
    char *line = tmp;
    float y = 36.0f;
    while (line && *line) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        ui_draw_text(8, y, UI_SCALE_LG, UI_COL_TEXT, line);
        y += 20.0f;
        line = nl ? nl + 1 : NULL;
    }

    draw_spinner(UI_TOP_W / 2.0f, 180.0f);

    ui_target_bot();
    ui_draw_rect(0, 0, UI_BOT_W, UI_HEADER_H, UI_COL_HEADER);
    ui_draw_text(6, 4, UI_SCALE_HDR, UI_COL_HEADER_TXT, "Activity Log++");

    ui_end_frame();
}

/* ── Background-thread spinner helper ───────────────────────────── */

typedef void (*WorkerFunc)(void *arg);

typedef struct {
    WorkerFunc    func;
    void         *arg;
    volatile bool done;
} WorkerCtx;

static void worker_entry(void *raw) {
    WorkerCtx *ctx = (WorkerCtx *)raw;
    ctx->func(ctx->arg);
    ctx->done = true;
}

static void run_with_spinner(const char *title, const char *body,
                             int step, int total_steps,
                             WorkerFunc func, void *arg)
{
    WorkerCtx ctx = { func, arg, false };
    Thread thread = threadCreate(worker_entry, &ctx, 0x8000, 0x38, 1, false);
    if (!thread)
        thread = threadCreate(worker_entry, &ctx, 0x8000, 0x38, -2, false);
    if (thread) {
        while (!ctx.done && aptMainLoop()) {
            if (step > 0)
                draw_progress_screen(title, body, step, total_steps);
            else
                draw_loading_screen(title, body);
        }
        threadJoin(thread, U64_MAX);
        threadFree(thread);
    } else {
        /* Fallback: synchronous */
        if (step > 0)
            draw_progress_screen(title, body, step, total_steps);
        else
            draw_loading_screen(title, body);
        func(arg);
    }
}

static void run_loading_with_spinner(const char *title, const char *body,
                                     WorkerFunc func, void *arg)
{
    run_with_spinner(title, body, 0, 0, func, arg);
}

/* ── Worker arg structs and functions ──────────────────────────── */

/* Step 1: Open archive */
typedef struct {
    const u32 *region_ids;
    int        region_count;
    FS_Archive archive;
    Result     rc;
} OpenArchiveArgs;

static void open_archive_work(void *raw) {
    OpenArchiveArgs *a = (OpenArchiveArgs *)raw;
    a->rc = -1;
    for (int i = 0; i < a->region_count; i++) {
        a->rc = pld_open_archive(&a->archive, a->region_ids[i]);
        if (R_SUCCEEDED(a->rc)) break;
    }
}

/* Step 2: Read summary + sessions */
typedef struct {
    FS_Archive     archive;
    PldFile       *pld;
    PldSessionLog *sessions;
    Result         rc_summary;
    Result         rc_sessions;
} ReadPldArgs;

static void read_pld_work(void *raw) {
    ReadPldArgs *a = (ReadPldArgs *)raw;
    a->rc_summary = pld_read_summary(a->archive, a->pld);
    if (R_FAILED(a->rc_summary)) {
        a->rc_sessions = -1;
        return;
    }
    a->rc_sessions = pld_read_sessions(a->archive, a->sessions);
}

/* Step 3: Merge */
typedef struct {
    PldFile       *pld;
    PldSessionLog *sessions;
} MergeArgs;

static void merge_work(void *raw) {
    MergeArgs *a = (MergeArgs *)raw;
    PldFile       sd_pld;
    PldSessionLog sd_sessions = {NULL, 0};
    mkdir(PLD_BACKUP_DIR, 0777);
    if (R_SUCCEEDED(pld_read_sd(PLD_MERGED_PATH, &sd_pld, &sd_sessions))) {
        pld_merge_sessions(a->sessions, &sd_sessions, true);
        pld_merge_summaries(a->pld, sd_pld.summaries, sd_pld.summary_count, true);
        pld_sessions_free(&sd_sessions);
        for (int i = 0; i < PLD_SUMMARY_COUNT; i++) {
            PldSummary *s = &a->pld->summaries[i];
            if (pld_summary_is_empty(s)) continue;
            s->total_secs = 0;
            for (int j = 0; j < a->sessions->count; j++)
                if (a->sessions->entries[j].title_id == s->title_id)
                    s->total_secs += a->sessions->entries[j].play_secs;
        }
    }
    pld_write_sd(PLD_MERGED_PATH, a->pld, a->sessions);
}

/* Step 4: title_names_load (trivial wrapper) */
static void title_names_load_work(void *arg) {
    (void)arg;
    title_names_load();
}

/* Step 5: Scan installed titles */
typedef struct {
    int new_names;
} ScanNamesArgs;

static void scan_names_work(void *raw) {
    ScanNamesArgs *a = (ScanNamesArgs *)raw;
    a->new_names = title_names_scan_installed();
    if (a->new_names > 0) title_names_save();
}

/* Step 6: title_icons_load_sd_cache (trivial wrapper) */
static void title_icons_load_work(void *arg) {
    (void)arg;
    title_icons_load_sd_cache();
}

/* Step 7 + post-sync: icon_fetch_missing */
typedef struct {
    const PldSummary *const *valid;
    int n;
} IconFetchArgs;

static void icon_fetch_work(void *raw) {
    IconFetchArgs *a = (IconFetchArgs *)raw;
    icon_fetch_missing(a->valid, a->n);
}

/* Sync: net_init */
typedef struct {
    NetCtx *ctx;
    NetRole role;
    Result  rc;
} NetInitArgs;

static void net_init_work(void *raw) {
    NetInitArgs *a = (NetInitArgs *)raw;
    a->rc = net_init(a->ctx, a->role);
}

/* Sync: session exchange */
typedef struct {
    NetCtx        *ctx;
    PldSessionLog *sessions;
    int            new_sess;
    int            rc;
} NetExchSessionsArgs;

static void net_exch_sessions_work(void *raw) {
    NetExchSessionsArgs *a = (NetExchSessionsArgs *)raw;
    a->rc = net_exchange_sessions(a->ctx, a->sessions, &a->new_sess);
}

/* Sync: summary exchange */
typedef struct {
    NetCtx  *ctx;
    PldFile *pld;
    int      new_apps;
    int      rc;
} NetExchSummariesArgs;

static void net_exch_summaries_work(void *raw) {
    NetExchSummariesArgs *a = (NetExchSummariesArgs *)raw;
    a->rc = net_exchange_summaries(a->ctx, a->pld, &a->new_apps);
}

/* Sync: title name exchange */
typedef struct {
    NetCtx *ctx;
    int     rc;
} NetExchNamesArgs;

static void net_exch_names_work(void *raw) {
    NetExchNamesArgs *a = (NetExchNamesArgs *)raw;
    a->rc = net_exchange_title_names(a->ctx);
    if (a->rc == 0) title_names_save();
}

/* Reset: re-read NAND data */
typedef struct {
    const u32     *region_ids;
    int            region_count;
    PldFile        pld;
    PldSessionLog  sessions;
    Result         rc;
} ResetReadArgs;

static void reset_read_work(void *raw) {
    ResetReadArgs *a = (ResetReadArgs *)raw;
    a->sessions.entries = NULL;
    a->sessions.count   = 0;
    a->rc = -1;
    FS_Archive rst_archive = 0;
    for (int i = 0; i < a->region_count; i++) {
        a->rc = pld_open_archive(&rst_archive, a->region_ids[i]);
        if (R_SUCCEEDED(a->rc)) break;
    }
    if (R_FAILED(a->rc)) return;
    a->rc = pld_read_summary(rst_archive, &a->pld);
    if (R_FAILED(a->rc)) { FSUSER_CloseArchive(rst_archive); return; }
    a->rc = pld_read_sessions(rst_archive, &a->sessions);
    FSUSER_CloseArchive(rst_archive);
    if (R_FAILED(a->rc)) { pld_sessions_free(&a->sessions); return; }
    pld_backup_from_path(PLD_MERGED_PATH);
    a->rc = pld_write_sd(PLD_MERGED_PATH, &a->pld, &a->sessions);
    if (R_FAILED(a->rc)) pld_sessions_free(&a->sessions);
}

/* ── Rendering ──────────────────────────────────────────────────── */

static void render_game_list(const PldSummary *const valid[], int n,
                             int sel, float scroll_y,
                             const PldSessionLog *sessions,
                             const char *status_msg,
                             bool show_system, bool show_unknown,
                             SortMode sort_mode, float anim_t)
{
    /* Visible rows (drawn first so header paints over any overlap) */
    char t_buf[20];
    char d0_buf[12];
    char d1_buf[12];
    char fallback[32];

    int first_vis = (int)(scroll_y / UI_ROW_H);
    if (first_vis < 0) first_vis = 0;
    int last_vis = first_vis + UI_VISIBLE_ROWS + 1; /* +1 for partial row */
    if (last_vis > n) last_vis = n;

    for (int i = first_vis; i < last_vis; i++) {
        float row_y = UI_LIST_Y + (float)i * UI_ROW_H - scroll_y;
        if (row_y + UI_ROW_H < UI_LIST_Y || row_y >= UI_LIST_BOT) continue;

        /* Per-row cascade fade + vertical slide */
        int vis_row = i - first_vis;
        float stagger = 0.24f;
        float fade_len = 0.30f;
        float row_start = (float)vis_row * stagger;
        float reveal = (anim_t <= row_start) ? 0.0f
                     : (anim_t >= row_start + fade_len) ? 1.0f
                     : (anim_t - row_start) / fade_len;
        u8 alpha = (u8)(reveal * 255.0f);
        if (alpha == 0) continue;
        float y_off = (1.0f - reveal) * -8.0f;
        row_y += y_off;

        u32 bg = (i == sel) ? UI_COL_ROW_SEL :
                 (i % 2 == 0) ? UI_COL_BG : UI_COL_ROW_ALT;
        ui_draw_rect(0, row_y, UI_TOP_W, UI_ROW_H,
                     (bg & 0x00FFFFFF) | ((u32)alpha << 24));

        const PldSummary *s = valid[i];

        const char *name = title_name_lookup(s->title_id);
        if (!name) name = title_db_lookup(s->title_id);
        if (!name) {
            snprintf(fallback, sizeof(fallback), "0x%016llX",
                     (unsigned long long)s->title_id);
            name = fallback;
        }

        pld_fmt_time(s->total_secs, t_buf, sizeof(t_buf));
        pld_fmt_date(s->first_played_days, d0_buf, sizeof(d0_buf));
        pld_fmt_date(s->last_played_days,  d1_buf, sizeof(d1_buf));

        int sess_count = pld_count_sessions_for(sessions, s->title_id);

        C2D_Image icon;
        if (title_icon_get(s->title_id, &icon)) {
            if (alpha == 255)
                ui_draw_image(icon, 6.0f, row_y, ICON_DRAW_SIZE);
            else
                ui_draw_image_alpha(icon, 6.0f, row_y, ICON_DRAW_SIZE, alpha);
        } else {
            /* Letter-initial fallback: colored square with the first letter */
            const u32 kIconColors[] = {
                C2D_Color32(0x4A, 0x86, 0xC8, 0xFF),  /* Activity blue */
                C2D_Color32(0xC8, 0x6A, 0x4A, 0xFF),  /* orange        */
                C2D_Color32(0x4A, 0xC8, 0x78, 0xFF),  /* green         */
                C2D_Color32(0xC8, 0x4A, 0x8A, 0xFF),  /* pink          */
                C2D_Color32(0x8A, 0x4A, 0xC8, 0xFF),  /* purple        */
                C2D_Color32(0xC8, 0xBE, 0x4A, 0xFF),  /* yellow        */
                C2D_Color32(0x4A, 0xB4, 0xC8, 0xFF),  /* teal          */
                C2D_Color32(0xC8, 0x4A, 0x4A, 0xFF),  /* red           */
            };
            u32 icon_col = kIconColors[(s->title_id >> 8) % 8];
            ui_draw_rect(6.0f, row_y, (float)ICON_DRAW_SIZE, (float)ICON_DRAW_SIZE,
                         (icon_col & 0x00FFFFFF) | ((u32)alpha << 24));
            char letter[2] = { name[0] ? name[0] : '?', '\0' };
            ui_draw_text(18.0f, row_y + 14.0f, UI_SCALE_LG,
                         (0xFFFFFFFF & 0x00FFFFFF) | ((u32)alpha << 24), letter);
        }

        u32 text_col     = (UI_COL_TEXT     & 0x00FFFFFF) | ((u32)alpha << 24);
        u32 text_dim_col = (UI_COL_TEXT_DIM & 0x00FFFFFF) | ((u32)alpha << 24);
        ui_draw_text(60.0f, row_y + 8, UI_SCALE_LG, text_col, name);
        ui_draw_text_right(394, row_y + 8, UI_SCALE_LG, text_dim_col, t_buf);
        ui_draw_textf(60.0f, row_y + 28, UI_SCALE_SM, text_dim_col,
                      "L:%u  S:%d  %s-%s",
                      (unsigned)s->launch_count, sess_count, d0_buf, d1_buf);
    }

    /* Header bar (drawn after rows so it covers any partial overlap) */
    ui_draw_rect(0, 0, UI_TOP_W, UI_HEADER_H, UI_COL_HEADER);
    ui_draw_text(6, 4, UI_SCALE_HDR, UI_COL_HEADER_TXT, "Activity Log++");
    {
        char hbuf[32];
        snprintf(hbuf, sizeof(hbuf), "Sort: %s", sort_labels[sort_mode]);
        ui_draw_text_right(UI_TOP_W - 6, 4, UI_SCALE_HDR, UI_COL_HEADER_TXT, hbuf);
    }

    /* Status bar */
    ui_draw_rect(0, 220, UI_TOP_W, UI_STATUS_H, UI_COL_STATUS_BG);
    if (status_msg && status_msg[0])
        ui_draw_text(4, 222, UI_SCALE_SM, UI_COL_STATUS_TXT, status_msg);
    else {
        const char *filter_label = show_unknown ? "All" :
                                   show_system  ? "Games+Sys" : "Games";
        ui_draw_textf(4, 222, UI_SCALE_SM, UI_COL_STATUS_TXT,
                      "%d %s  [%s]", n,
                      show_system ? "titles" : "games", filter_label);
    }
    ui_draw_text_right(396, 222, UI_SCALE_SM, UI_COL_STATUS_TXT,
                       "L/R:sort  Y:filter  X:rank");
}

static void render_bottom_stats(const PldSummary *valid[], int n,
                                const PldSessionLog *sessions,
                                u32 sync_count)
{
    /* Header */
    ui_draw_rect(0, 0, UI_BOT_W, UI_HEADER_H, UI_COL_HEADER);
    ui_draw_text(6, 4, UI_SCALE_HDR, UI_COL_HEADER_TXT, "Statistics");

    /* Total playtime across all titles & find most played */
    u32 total_secs = 0;
    u32 total_launches = 0;
    int most_idx = -1;
    u32 most_secs = 0;
    for (int i = 0; i < n; i++) {
        total_secs    += valid[i]->total_secs;
        total_launches += valid[i]->launch_count;
        if (valid[i]->total_secs > most_secs) {
            most_secs = valid[i]->total_secs;
            most_idx  = i;
        }
    }
    char t_buf[20];
    pld_fmt_time(total_secs, t_buf, sizeof(t_buf));

    /* Body rows */
    char vbuf[32];
    float y = 32.0f;

    ui_draw_text(8, y, UI_SCALE_LG, UI_COL_TEXT, "Games tracked");
    snprintf(vbuf, sizeof(vbuf), "%d", n);
    ui_draw_text_right(UI_BOT_W - 8, y, UI_SCALE_LG, UI_COL_TEXT_DIM, vbuf);
    y += 24.0f;

    ui_draw_text(8, y, UI_SCALE_LG, UI_COL_TEXT, "Total playtime");
    ui_draw_text_right(UI_BOT_W - 8, y, UI_SCALE_LG, UI_COL_TEXT_DIM, t_buf);
    y += 24.0f;

    ui_draw_text(8, y, UI_SCALE_LG, UI_COL_TEXT, "Syncs");
    snprintf(vbuf, sizeof(vbuf), "%lu", (unsigned long)sync_count);
    ui_draw_text_right(UI_BOT_W - 8, y, UI_SCALE_LG, UI_COL_TEXT_DIM, vbuf);
    y += 24.0f;

    ui_draw_text(8, y, UI_SCALE_LG, UI_COL_TEXT, "Total launches");
    snprintf(vbuf, sizeof(vbuf), "%lu", (unsigned long)total_launches);
    ui_draw_text_right(UI_BOT_W - 8, y, UI_SCALE_LG, UI_COL_TEXT_DIM, vbuf);
    y += 24.0f;

    ui_draw_text(8, y, UI_SCALE_LG, UI_COL_TEXT, "Total sessions");
    snprintf(vbuf, sizeof(vbuf), "%d", sessions->count);
    ui_draw_text_right(UI_BOT_W - 8, y, UI_SCALE_LG, UI_COL_TEXT_DIM, vbuf);
    y += 24.0f;

    ui_draw_text(8, y, UI_SCALE_LG, UI_COL_TEXT, "Most played");
    if (most_idx >= 0) {
        const char *name = title_name_lookup(valid[most_idx]->title_id);
        if (!name) name = title_db_lookup(valid[most_idx]->title_id);
        if (!name) name = "Unknown";
        ui_draw_text_trunc(UI_BOT_W - 8 - 160, y, UI_SCALE_LG,
                           UI_COL_TEXT_DIM, name, 160);
    }

    /* Divider */
    ui_draw_rect(0, 180, UI_BOT_W, 1, UI_COL_DIVIDER);

    /* Controls hint */
    ui_draw_text_right(UI_BOT_W - 4, 184, UI_SCALE_SM, UI_COL_TEXT_DIM,
                       "A:select  START:menu  Up/Dn:scroll");
}

/* ── Rankings rendering ─────────────────────────────────────────── */

static void render_rankings_top(const PldSummary *ranked[], int rank_count,
                                int rank_sel, int rank_scroll,
                                const int rank_sess[], RankingsTab tab,
                                float anim_t)
{
    /* Header bar */
    ui_draw_rect(0, 0, UI_TOP_W, UI_HEADER_H, UI_COL_HEADER);
    ui_draw_text(6, 4, UI_SCALE_HDR, UI_COL_HEADER_TXT, "Rankings");
    ui_draw_text_right(UI_TOP_W - 6, 4, UI_SCALE_HDR, UI_COL_HEADER_TXT,
                       rank_tab_labels[tab]);

    /* Find max metric value for proportional bars */
    u32 max_val = 1; /* avoid div-by-zero */
    for (int i = 0; i < rank_count; i++) {
        u32 val = 0;
        switch (tab) {
            case RANK_PLAYTIME: val = ranked[i]->total_secs;       break;
            case RANK_LAUNCHES: val = ranked[i]->launch_count;     break;
            case RANK_SESSIONS: val = (u32)rank_sess[i];           break;
            case RANK_RECENT:   val = ranked[i]->last_played_days; break;
            default: break;
        }
        if (val > max_val) max_val = val;
    }

    char fallback[32];
    char t_buf[20], d0_buf[12], d1_buf[12];

    for (int i = rank_scroll; i < rank_scroll + UI_VISIBLE_ROWS && i < rank_count; i++) {
        float row_y = UI_LIST_Y + (float)(i - rank_scroll) * UI_ROW_H;
        const PldSummary *s = ranked[i];

        /* Per-row cascade fade + vertical slide */
        int vis_row = i - rank_scroll;
        float stagger = 0.24f;
        float fade_len = 0.30f;
        float row_start = (float)vis_row * stagger;
        float reveal = (anim_t <= row_start) ? 0.0f
                     : (anim_t >= row_start + fade_len) ? 1.0f
                     : (anim_t - row_start) / fade_len;
        u8 alpha = (u8)(reveal * 255.0f);
        if (alpha == 0) continue;
        float y_off = (1.0f - reveal) * -8.0f;
        row_y += y_off;

        /* Row background */
        u32 bg = (i == rank_sel) ? UI_COL_ROW_SEL :
                 (i % 2 == 0) ? UI_COL_BG : UI_COL_ROW_ALT;
        ui_draw_rect(0, row_y, UI_TOP_W, UI_ROW_H,
                     (bg & 0x00FFFFFF) | ((u32)alpha << 24));

        /* Proportional bar (semi-transparent blue behind row) */
        u32 val = 0;
        switch (tab) {
            case RANK_PLAYTIME: val = s->total_secs;       break;
            case RANK_LAUNCHES: val = s->launch_count;     break;
            case RANK_SESSIONS: val = (u32)rank_sess[i];   break;
            case RANK_RECENT:   val = s->last_played_days; break;
            default: break;
        }
        float bar_max_w = UI_TOP_W - 4.0f;
        float bar_w = bar_max_w * ((float)val / (float)max_val);
        if (bar_w < 2.0f) bar_w = 2.0f;
        u8 bar_base_alpha = 0x30;
        u8 bar_alpha = (u8)((u32)bar_base_alpha * alpha / 255);
        u32 bar_col = C2D_Color32(0x4A, 0x86, 0xC8, bar_alpha);
        ui_draw_rect(2.0f, row_y + 1.0f, bar_w, UI_ROW_H - 2.0f, bar_col);

        u32 text_col     = (UI_COL_TEXT     & 0x00FFFFFF) | ((u32)alpha << 24);
        u32 text_dim_col = (UI_COL_TEXT_DIM & 0x00FFFFFF) | ((u32)alpha << 24);

        /* Rank number */
        char rank_num[16];
        snprintf(rank_num, sizeof(rank_num), "#%d", i + 1);
        ui_draw_text(4.0f, row_y + 16.0f, UI_SCALE_SM, text_dim_col, rank_num);

        /* Icon */
        C2D_Image icon;
        if (title_icon_get(s->title_id, &icon)) {
            if (alpha == 255)
                ui_draw_image(icon, 28.0f, row_y, ICON_DRAW_SIZE);
            else
                ui_draw_image_alpha(icon, 28.0f, row_y, ICON_DRAW_SIZE, alpha);
        } else {
            const char *name_tmp = title_name_lookup(s->title_id);
            if (!name_tmp) name_tmp = title_db_lookup(s->title_id);
            char letter[2] = { (name_tmp && name_tmp[0]) ? name_tmp[0] : '?', '\0' };
            const u32 kIconColors[] = {
                C2D_Color32(0x4A, 0x86, 0xC8, 0xFF),
                C2D_Color32(0xC8, 0x6A, 0x4A, 0xFF),
                C2D_Color32(0x4A, 0xC8, 0x78, 0xFF),
                C2D_Color32(0xC8, 0x4A, 0x8A, 0xFF),
                C2D_Color32(0x8A, 0x4A, 0xC8, 0xFF),
                C2D_Color32(0xC8, 0xBE, 0x4A, 0xFF),
                C2D_Color32(0x4A, 0xB4, 0xC8, 0xFF),
                C2D_Color32(0xC8, 0x4A, 0x4A, 0xFF),
            };
            u32 icon_col = kIconColors[(s->title_id >> 8) % 8];
            ui_draw_rect(28.0f, row_y, (float)ICON_DRAW_SIZE, (float)ICON_DRAW_SIZE,
                         (icon_col & 0x00FFFFFF) | ((u32)alpha << 24));
            ui_draw_text(40.0f, row_y + 14.0f, UI_SCALE_LG,
                         (0xFFFFFFFF & 0x00FFFFFF) | ((u32)alpha << 24), letter);
        }

        /* Name */
        const char *name = title_name_lookup(s->title_id);
        if (!name) name = title_db_lookup(s->title_id);
        if (!name) {
            snprintf(fallback, sizeof(fallback), "0x%016llX",
                     (unsigned long long)s->title_id);
            name = fallback;
        }
        ui_draw_text(82.0f, row_y + 8, UI_SCALE_LG, text_col, name);

        /* Metric value right-aligned */
        char metric[24];
        switch (tab) {
            case RANK_PLAYTIME:
                pld_fmt_time(s->total_secs, metric, sizeof(metric));
                break;
            case RANK_LAUNCHES:
                snprintf(metric, sizeof(metric), "%u", (unsigned)s->launch_count);
                break;
            case RANK_SESSIONS:
                snprintf(metric, sizeof(metric), "%d", rank_sess[i]);
                break;
            case RANK_RECENT:
                pld_fmt_date(s->last_played_days, metric, sizeof(metric));
                break;
            default:
                metric[0] = '\0';
                break;
        }
        ui_draw_text_right(394, row_y + 8, UI_SCALE_LG, text_dim_col, metric);

        /* Secondary line */
        pld_fmt_time(s->total_secs, t_buf, sizeof(t_buf));
        pld_fmt_date(s->first_played_days, d0_buf, sizeof(d0_buf));
        pld_fmt_date(s->last_played_days,  d1_buf, sizeof(d1_buf));
        ui_draw_textf(82.0f, row_y + 28, UI_SCALE_SM, text_dim_col,
                      "L:%u  S:%d  %s-%s",
                      (unsigned)s->launch_count, rank_sess[i], d0_buf, d1_buf);
    }

    /* Empty state */
    if (rank_count == 0) {
        ui_draw_text(8, 36, UI_SCALE_LG, UI_COL_TEXT_DIM, "No titles to rank");
    }

    /* Status bar */
    ui_draw_rect(0, 220, UI_TOP_W, UI_STATUS_H, UI_COL_STATUS_BG);
    ui_draw_text_right(396, 222, UI_SCALE_SM, UI_COL_STATUS_TXT,
                       "L/R:tab  A:detail  B:back");
}

static void render_rankings_bot(const PldSummary *ranked[], int rank_count,
                                const int rank_sess[], RankingsTab tab)
{
    /* Header */
    ui_draw_rect(0, 0, UI_BOT_W, UI_HEADER_H, UI_COL_HEADER);
    ui_draw_text(6, 4, UI_SCALE_HDR, UI_COL_HEADER_TXT, "Rankings");

    /* Aggregate stats for top N */
    float y = 32.0f;
    char vbuf[32];

    ui_draw_textf(8, y, UI_SCALE_LG, UI_COL_TEXT, "Top %d %s",
                  rank_count, rank_tab_labels[tab]);
    y += 24.0f;

    u32 total_time = 0;
    u32 total_launches = 0;
    int total_sessions = 0;
    for (int i = 0; i < rank_count; i++) {
        total_time += ranked[i]->total_secs;
        total_launches += ranked[i]->launch_count;
        total_sessions += rank_sess[i];
    }

    char tbuf[20];
    pld_fmt_time(total_time, tbuf, sizeof(tbuf));
    ui_draw_text(8, y, UI_SCALE_LG, UI_COL_TEXT, "Total playtime");
    ui_draw_text_right(UI_BOT_W - 8, y, UI_SCALE_LG, UI_COL_TEXT_DIM, tbuf);
    y += 24.0f;

    ui_draw_text(8, y, UI_SCALE_LG, UI_COL_TEXT, "Total launches");
    snprintf(vbuf, sizeof(vbuf), "%lu", (unsigned long)total_launches);
    ui_draw_text_right(UI_BOT_W - 8, y, UI_SCALE_LG, UI_COL_TEXT_DIM, vbuf);
    y += 24.0f;

    ui_draw_text(8, y, UI_SCALE_LG, UI_COL_TEXT, "Total sessions");
    snprintf(vbuf, sizeof(vbuf), "%d", total_sessions);
    ui_draw_text_right(UI_BOT_W - 8, y, UI_SCALE_LG, UI_COL_TEXT_DIM, vbuf);

    /* Divider */
    ui_draw_rect(0, 180, UI_BOT_W, 1, UI_COL_DIVIDER);

    /* Controls hint */
    ui_draw_text_right(UI_BOT_W - 4, 184, UI_SCALE_SM, UI_COL_TEXT_DIM,
                       "L/R:tab  Up/Dn:scroll  A:detail  B:back");
}

/* ── Detail screen ──────────────────────────────────────────────── */

#define DETAIL_ROW_H   16
#define DETAIL_LIST_Y  170
#define DETAIL_VISIBLE ((240 - DETAIL_LIST_Y) / DETAIL_ROW_H)  /* 4 */

static void render_detail_top(const PldSummary *s, const char *name,
                               const PldSessionLog *sessions,
                               const int *sess_indices, int sess_count,
                               int detail_scroll)
{
    /* Background */
    ui_draw_rect(0, 0, UI_TOP_W, UI_TOP_H, UI_COL_BG);

    /* Header bar */
    ui_draw_rect(0, 0, UI_TOP_W, UI_HEADER_H, UI_COL_HEADER);
    ui_draw_text(6, 4, UI_SCALE_HDR, UI_COL_HEADER_TXT, name);

    /* Icon (120×120) at left */
    C2D_Image icon;
    if (title_icon_get(s->title_id, &icon)) {
        ui_draw_image(icon, 8.0f, 28.0f, 120.0f);
    } else {
        /* Letter fallback at 120×120 */
        const u32 kIconColors[] = {
            C2D_Color32(0x4A, 0x86, 0xC8, 0xFF),
            C2D_Color32(0xC8, 0x6A, 0x4A, 0xFF),
            C2D_Color32(0x4A, 0xC8, 0x78, 0xFF),
            C2D_Color32(0xC8, 0x4A, 0x8A, 0xFF),
            C2D_Color32(0x8A, 0x4A, 0xC8, 0xFF),
            C2D_Color32(0xC8, 0xBE, 0x4A, 0xFF),
            C2D_Color32(0x4A, 0xB4, 0xC8, 0xFF),
            C2D_Color32(0xC8, 0x4A, 0x4A, 0xFF),
        };
        u32 icon_col = kIconColors[(s->title_id >> 8) % 8];
        ui_draw_rect(8.0f, 28.0f, 120.0f, 120.0f, icon_col);
        char letter[2] = { name[0] ? name[0] : '?', '\0' };
        ui_draw_text(48.0f, 68.0f, 1.5f, 0xFFFFFFFF, letter);
    }

    /* Stats (right side, x=136) */
    float sy = 30.0f;
    char tbuf[24];

    pld_fmt_time(s->total_secs, tbuf, sizeof(tbuf));
    ui_draw_textf(136, sy, UI_SCALE_LG, UI_COL_TEXT, "Playtime: %s", tbuf);
    sy += 18.0f;

    ui_draw_textf(136, sy, UI_SCALE_LG, UI_COL_TEXT, "Launches: %u",
                  (unsigned)s->launch_count);
    sy += 18.0f;

    ui_draw_textf(136, sy, UI_SCALE_LG, UI_COL_TEXT, "Sessions: %d",
                  sess_count);
    sy += 18.0f;

    {
        int streak = pld_longest_streak(sessions, sess_indices, sess_count);
        ui_draw_textf(136, sy, UI_SCALE_LG, UI_COL_TEXT, "Streak: %d days",
                      streak);
        sy += 18.0f;
    }

    {
        char dbuf[12];
        pld_fmt_date(s->first_played_days, dbuf, sizeof(dbuf));
        ui_draw_textf(136, sy, UI_SCALE_LG, UI_COL_TEXT, "First: %s", dbuf);
        sy += 18.0f;
    }
    {
        char dbuf[12];
        pld_fmt_date(s->last_played_days, dbuf, sizeof(dbuf));
        ui_draw_textf(136, sy, UI_SCALE_LG, UI_COL_TEXT, "Last:  %s", dbuf);
        sy += 18.0f;
    }

    ui_draw_textf(136, sy, UI_SCALE_SM, UI_COL_TEXT_DIM,
                  "ID: %016llX", (unsigned long long)s->title_id);

    /* Divider */
    ui_draw_rect(0, 152, UI_TOP_W, 1, UI_COL_DIVIDER);

    /* Column headers */
    ui_draw_text(6, 155, UI_SCALE_SM, UI_COL_TEXT_DIM, "Date/Time");
    ui_draw_text_right(394, 155, UI_SCALE_SM, UI_COL_TEXT_DIM, "Duration");

    /* Session rows */
    for (int i = 0; i < DETAIL_VISIBLE && (detail_scroll + i) < sess_count; i++) {
        int idx = sess_indices[detail_scroll + i];
        const PldSession *se = &sessions->entries[idx];
        float ry = DETAIL_LIST_Y + (float)i * DETAIL_ROW_H;

        u32 bg = ((detail_scroll + i) % 2 == 0) ? UI_COL_BG : UI_COL_ROW_ALT;
        ui_draw_rect(0, ry, UI_TOP_W, DETAIL_ROW_H, bg);

        char ts[20];
        pld_fmt_timestamp(se->timestamp, ts, sizeof(ts));
        ui_draw_text(6, ry + 1, UI_SCALE_SM, UI_COL_TEXT, ts);

        char dur[20];
        pld_fmt_time(se->play_secs, dur, sizeof(dur));
        ui_draw_text_right(394, ry + 1, UI_SCALE_SM, UI_COL_TEXT, dur);
    }

    /* Show empty state */
    if (sess_count == 0) {
        ui_draw_text(6, DETAIL_LIST_Y + 4, UI_SCALE_SM, UI_COL_TEXT_DIM,
                     "No sessions recorded");
    }
}

static void render_detail_bot(void)
{
    ui_draw_rect(0, 0, UI_BOT_W, UI_HEADER_H, UI_COL_HEADER);
    ui_draw_text(6, 4, UI_SCALE_HDR, UI_COL_HEADER_TXT, "Game Details");
    ui_draw_text_right(UI_BOT_W - 8, 36, UI_SCALE_LG, UI_COL_TEXT_DIM, "Up/Dn:scroll  B:back");
}

/* ── Menu overlay ───────────────────────────────────────────────── */

static void render_menu(int sel)
{
    static const char *items[] = { "Charts", "Sync", "Backup", "Export", "Restore", "Reset", "Quit" };
    static const int   NITEMS  = 7;

    float mx     = 8.0f;
    float my     = 28.0f;
    float mw     = 140.0f;
    float item_h = 22.0f;
    float mh     = NITEMS * item_h + 8.0f;

    /* Border rect */
    ui_draw_rect(mx, my, mw, mh, UI_COL_HEADER);
    /* Inner background */
    ui_draw_rect(mx + 2, my + 2, mw - 4, mh - 4, UI_COL_BG);

    for (int i = 0; i < NITEMS; i++) {
        float ry = my + 2.0f + (float)i * item_h;
        if (i == sel)
            ui_draw_rect(mx + 2, ry, mw - 4, item_h, UI_COL_ROW_SEL);
        u32 col = (i == sel) ? UI_COL_HEADER : UI_COL_TEXT;
        char label[32];
        snprintf(label, sizeof(label), "%s %s",
                 (i == sel) ? ">" : " ", items[i]);
        ui_draw_text(mx + 6, ry + 4, UI_SCALE_LG, col, label);
    }
}

/* ── CSV/JSON Export ────────────────────────────────────────────── */

/* Escape a string for CSV: if it contains commas, quotes, or newlines,
 * wrap in double-quotes and double any internal quotes. */
static void csv_write_escaped(FILE *f, const char *s)
{
    bool needs_quote = false;
    for (const char *p = s; *p; p++) {
        if (*p == ',' || *p == '"' || *p == '\n') { needs_quote = true; break; }
    }
    if (!needs_quote) { fputs(s, f); return; }
    fputc('"', f);
    for (const char *p = s; *p; p++) {
        if (*p == '"') fputc('"', f);
        fputc(*p, f);
    }
    fputc('"', f);
}

/* Escape a string for JSON: escape backslash and double-quote. */
static void json_write_escaped(FILE *f, const char *s)
{
    for (const char *p = s; *p; p++) {
        if (*p == '"' || *p == '\\') fputc('\\', f);
        fputc(*p, f);
    }
}

static Result export_data(const PldFile *pld, const PldSessionLog *sessions)
{
    mkdir("sdmc:/3ds/activity-log-pp", 0755);

    FILE *csv = fopen("sdmc:/3ds/activity-log-pp/export.csv", "w");
    FILE *json = fopen("sdmc:/3ds/activity-log-pp/export.json", "w");
    if (!csv || !json) {
        if (csv) fclose(csv);
        if (json) fclose(json);
        return -1;
    }

    /* CSV header */
    fputs("title_id,name,playtime_secs,playtime,launches,sessions,first_played,last_played\n", csv);

    /* JSON opening */
    fputs("{\n  \"titles\": [\n", json);

    bool first_json = true;
    for (int i = 0; i < PLD_SUMMARY_COUNT; i++) {
        const PldSummary *s = &pld->summaries[i];
        if (pld_summary_is_empty(s)) continue;

        /* Title name lookup */
        const char *name = title_name_lookup(s->title_id);
        if (!name) name = title_db_lookup(s->title_id);
        if (!name) name = "Unknown";

        /* Format fields */
        char time_buf[32], first_buf[16], last_buf[16];
        pld_fmt_time(s->total_secs, time_buf, sizeof(time_buf));
        pld_fmt_date(s->first_played_days, first_buf, sizeof(first_buf));
        pld_fmt_date(s->last_played_days, last_buf, sizeof(last_buf));

        int sess_count = pld_count_sessions_for(sessions, s->title_id);

        /* CSV row */
        fprintf(csv, "%016llX,", (unsigned long long)s->title_id);
        csv_write_escaped(csv, name);
        fprintf(csv, ",%lu,%s,%u,%d,%s,%s\n",
                (unsigned long)s->total_secs, time_buf,
                s->launch_count, sess_count, first_buf, last_buf);

        /* JSON entry */
        if (!first_json) fputs(",\n", json);
        first_json = false;
        fprintf(json, "    {\n");
        fprintf(json, "      \"title_id\": \"%016llX\",\n", (unsigned long long)s->title_id);
        fprintf(json, "      \"name\": \"");
        json_write_escaped(json, name);
        fprintf(json, "\",\n");
        fprintf(json, "      \"playtime_secs\": %lu,\n", (unsigned long)s->total_secs);
        fprintf(json, "      \"playtime\": \"%s\",\n", time_buf);
        fprintf(json, "      \"launches\": %u,\n", s->launch_count);
        fprintf(json, "      \"sessions\": %d,\n", sess_count);
        fprintf(json, "      \"first_played\": \"%s\",\n", first_buf);
        fprintf(json, "      \"last_played\": \"%s\"\n", last_buf);
        fprintf(json, "    }");
    }

    fputs("\n  ]\n}\n", json);

    fclose(csv);
    fclose(json);
    return 0;
}

typedef struct {
    const PldFile       *pld;
    const PldSessionLog *sessions;
    Result               rc;
} ExportArgs;

static void export_work(void *raw) {
    ExportArgs *a = (ExportArgs *)raw;
    a->rc = export_data(a->pld, a->sessions);
}

/* ── Sync flow ──────────────────────────────────────────────────── */

static void run_sync_flow(PldFile *pld, PldSessionLog *sessions,
                          u32 *sync_count, char *status_msg, int status_msg_len)
{
    NetCtx net_ctx;
    memset(&net_ctx, 0, sizeof(net_ctx));
    net_ctx.tcp_sock = net_ctx.listen_sock = net_ctx.udp_sock = -1;
    bool net_active = false;

    /* ── Role selection ── */
    while (aptMainLoop()) {
        hidScanInput();
        u32 role_keys = hidKeysDown();

        if (role_keys & (KEY_X | KEY_Y | KEY_B)) {
            if (role_keys & KEY_B) return;  /* cancel/back to viewer */
            /* KEY_X = Host, KEY_Y = Client */
            NetRole role = (role_keys & KEY_X) ? NET_ROLE_HOST : NET_ROLE_CLIENT;
            NetInitArgs ni_args = { &net_ctx, role, -1 };
            run_loading_with_spinner("Activity Log++", "Initializing network...",
                                     net_init_work, &ni_args);
            if (R_FAILED(ni_args.rc)) {
                char err_body[80];
                snprintf(err_body, sizeof(err_body),
                         "Network init failed: 0x%08lX\n\nPress START to continue.",
                         ni_args.rc);
                while (aptMainLoop()) {
                    hidScanInput();
                    if (hidKeysDown() & KEY_START) break;
                    draw_message_screen("Network Error", err_body);
                }
                return;
            }
            net_active = true;
            break;
        }

        draw_message_screen("Activity Log++",
                            "Connect to another 3DS?\n\nX: Host\nY: Client\nB: Back");
    }

    if (!net_active) return;

    /* ── Network status loop ── */
    {
        int connected_frames = 0;
        NetState prev_state = (NetState)-1;
        char prev_peer_ip[16] = {0};
        char net_title[64] = "Connecting...";
        char net_body[192] = "";

        while (aptMainLoop()) {
            hidScanInput();
            u32 net_keys = hidKeysDown();

            if (net_ctx.state != NET_STATE_CONNECTED &&
                net_ctx.state != NET_STATE_ERROR) {
                if (net_keys & KEY_START) {
                    net_shutdown(&net_ctx);
                    return;
                }
                net_tick(&net_ctx);
            } else if (net_ctx.state == NET_STATE_ERROR) {
                if (net_keys & KEY_START) {
                    net_shutdown(&net_ctx);
                    return;
                }
            } else {
                connected_frames++;
                if (connected_frames >= 120) break;
            }

            bool net_changed = (net_ctx.state != prev_state) ||
                               (strcmp(net_ctx.peer_ip, prev_peer_ip) != 0);
            if (net_changed) {
                if (net_ctx.state == NET_STATE_CONNECTED) {
                    snprintf(net_title, sizeof(net_title), "Connected!");
                    snprintf(net_body, sizeof(net_body),
                             "Peer: %s", net_ctx.peer_ip);
                } else if (net_ctx.state == NET_STATE_ERROR) {
                    snprintf(net_title, sizeof(net_title), "Network Error");
                    snprintf(net_body, sizeof(net_body),
                             "Press START to continue.");
                } else if (net_ctx.role == NET_ROLE_HOST) {
                    snprintf(net_title, sizeof(net_title), "HOST");
                    snprintf(net_body, sizeof(net_body),
                             "Own IP: %s\nBroadcasting...\nWaiting for client\n\nSTART: cancel",
                             net_ctx.own_ip);
                } else {
                    snprintf(net_title, sizeof(net_title), "CLIENT");
                    if (net_ctx.peer_ip[0] != '\0')
                        snprintf(net_body, sizeof(net_body),
                                 "Connecting to %s...\n\nSTART: cancel",
                                 net_ctx.peer_ip);
                    else
                        snprintf(net_body, sizeof(net_body),
                                 "Scanning for host...\n\nSTART: cancel");
                }
                prev_state = net_ctx.state;
                snprintf(prev_peer_ip, sizeof(prev_peer_ip), "%s",
                         net_ctx.peer_ip);
            }

            draw_message_screen_ex(net_title, net_body,
                                   net_ctx.state != NET_STATE_ERROR);
        }
    }

    if (net_ctx.state != NET_STATE_CONNECTED) {
        net_shutdown(&net_ctx);
        return;
    }

    /* ── M6/M7: Session + Summary sync ── */
    int sess_rc = -1, app_rc = -1;
    int new_sess = 0, new_apps = 0;

    /* Step 1: Session exchange */
    {
        NetExchSessionsArgs es_args = { &net_ctx, sessions, 0, -1 };
        run_loading_with_spinner("Syncing...", "Exchanging sessions...",
                                 net_exch_sessions_work, &es_args);
        sess_rc = es_args.rc;
        new_sess = es_args.new_sess;
    }

    /* Step 2: Summary exchange */
    if (sess_rc == 0) {
        NetExchSummariesArgs ea_args = { &net_ctx, pld, 0, -1 };
        run_loading_with_spinner("Syncing...", "Syncing app list...",
                                 net_exch_summaries_work, &ea_args);
        app_rc = ea_args.rc;
        new_apps = ea_args.new_apps;
    }

    /* Step 3: Title name exchange (best-effort) */
    if (sess_rc == 0 && app_rc == 0) {
        NetExchNamesArgs en_args = { &net_ctx, -1 };
        run_loading_with_spinner("Syncing...", "Exchanging title names...",
                                 net_exch_names_work, &en_args);
    }

    /* Step 4: Recompute total_secs per title from merged session log */
    if (sess_rc == 0 && app_rc == 0) {
        for (int i = 0; i < PLD_SUMMARY_COUNT; i++) {
            PldSummary *s = &pld->summaries[i];
            if (pld_summary_is_empty(s)) continue;
            s->total_secs = 0;
            for (int j = 0; j < sessions->count; j++) {
                if (sessions->entries[j].title_id == s->title_id)
                    s->total_secs += sessions->entries[j].play_secs;
            }
        }
    }

    /* Result display (~2 seconds) and SD save */
    if (sess_rc == 0 && app_rc == 0) {
        char sync_body[64];
        snprintf(sync_body, sizeof(sync_body),
                 "+%d sessions, +%d apps", new_sess, new_apps);
        snprintf(status_msg, (size_t)status_msg_len,
                 "Synced: +%d sess +%d apps", new_sess, new_apps);
        for (int f = 0; f < 120 && aptMainLoop(); f++) {
            hidScanInput();
            draw_message_screen("Sync Complete", sync_body);
        }

        pld_backup_from_path(PLD_MERGED_PATH);   /* safety snapshot */
        Result sd_rc = pld_write_sd(PLD_MERGED_PATH, pld, sessions);
        if (R_FAILED(sd_rc)) {
            snprintf(status_msg, (size_t)status_msg_len, "SD save failed");
        } else {
            (*sync_count)++;
            save_sync_count(*sync_count);
        }
    } else {
        snprintf(status_msg, (size_t)status_msg_len, "Sync failed");
        for (int f = 0; f < 120 && aptMainLoop(); f++) {
            hidScanInput();
            draw_message_screen("Sync Failed", "Continuing with local data.");
        }
    }

    net_shutdown(&net_ctx);
}

/* ── Entry point ────────────────────────────────────────────────── */

int main(void)
{
    gfxInitDefault();
    ui_init();
    fsInit();
    APT_SetAppCpuTimeLimit(30);  /* Allow worker threads on core 1 */

    /* Step 1: Open save archive */
    OpenArchiveArgs oa_args = { region_ids, 4, 0, -1 };
    run_with_spinner("Activity Log++", "Opening save archive...", 1, 7,
                     open_archive_work, &oa_args);
    if (R_FAILED(oa_args.rc)) {
        char err_body[96];
        snprintf(err_body, sizeof(err_body),
                 "Error: 0x%08lX\n\nIs CFW active and Activity Log used?\n\nPress START to exit.",
                 oa_args.rc);
        while (aptMainLoop()) {
            hidScanInput();
            if (hidKeysDown() & KEY_START) break;
            draw_message_screen("Error", err_body);
        }
        ui_fini();
        fsExit();
        gfxExit();
        return 1;
    }

    /* Step 2: Read summary + sessions */
    PldFile pld;
    PldSessionLog sessions;
    ReadPldArgs rp_args = { oa_args.archive, &pld, &sessions, -1, -1 };
    run_with_spinner("Activity Log++", "Reading pld.dat...", 2, 7,
                     read_pld_work, &rp_args);
    FSUSER_CloseArchive(oa_args.archive);
    if (R_FAILED(rp_args.rc_summary)) {
        char err_body[80];
        snprintf(err_body, sizeof(err_body),
                 "Error reading summary: 0x%08lX\n\nPress START to exit.",
                 rp_args.rc_summary);
        while (aptMainLoop()) {
            hidScanInput();
            if (hidKeysDown() & KEY_START) break;
            draw_message_screen("Error", err_body);
        }
        ui_fini();
        fsExit();
        gfxExit();
        return 1;
    }
    if (R_FAILED(rp_args.rc_sessions)) {
        char err_body[80];
        snprintf(err_body, sizeof(err_body),
                 "Error reading sessions: 0x%08lX\n\nPress START to exit.",
                 rp_args.rc_sessions);
        while (aptMainLoop()) {
            hidScanInput();
            if (hidKeysDown() & KEY_START) break;
            draw_message_screen("Error", err_body);
        }
        ui_fini();
        fsExit();
        gfxExit();
        return 1;
    }

    /* Step 3: Load SD merged.dat and add-only merge into NAND data */
    MergeArgs merge_args = { &pld, &sessions };
    run_with_spinner("Activity Log++", "Loading merged data...", 3, 7,
                     merge_work, &merge_args);

    u32 sync_count = load_sync_count();

    /* Step 4: Load persisted title names */
    run_with_spinner("Activity Log++", "Loading title names...", 4, 7,
                     title_names_load_work, NULL);

    /* Step 5: Scan installed titles */
    ScanNamesArgs sn_args = { 0 };
    run_with_spinner("Activity Log++", "Scanning installed titles...", 5, 7,
                     scan_names_work, &sn_args);

    /* Build valid[] before icon fetch so fetch knows which titles need icons */
    const PldSummary *valid[PLD_SUMMARY_COUNT];
    bool show_system  = false;  /* L key toggles; default = games only */
    bool show_unknown = false;  /* R key toggles; default = named titles only */
    SortMode sort_mode = SORT_LAST_PLAYED;
    int n = collect_valid(&pld, valid, show_system, show_unknown);
    sort_valid(valid, n, sort_mode, &sessions, &pld);

    /* Step 6: Load icon cache */
    run_with_spinner("Activity Log++", "Loading icon cache...", 6, 7,
                     title_icons_load_work, NULL);

    /* Step 7: Fetch missing icons */
    IconFetchArgs if_args = { valid, n };
    run_with_spinner("Activity Log++", "Fetching missing icons (this may take a moment)...", 7, 7,
                     icon_fetch_work, &if_args);

    int sel        = 0;   /* currently highlighted entry index */
    int scroll_top = 0;   /* index of first visible row        */
    float scroll_y = 0.0f; /* smooth scroll pixel offset        */

    char status_msg[48] = {0};

    bool menu_open = false;
    int  menu_sel  = 0;   /* 0=Sync  1=Backup  2=Restore  3=Charts  4=Quit */

    /* Charts state */
    typedef enum { CHART_PIE, CHART_BAR, CHART_TAB_COUNT } ChartTab;
    bool charts_view = false;
    ChartTab chart_tab = CHART_PIE;
    PieSlice pie_slices[PIE_SLICES + 1];
    int  pie_count = 0;
    u32  pie_total = 0;
    int  chart_anim_frame = 0;
    int  list_anim_frame  = 0;
    int  rank_anim_frame  = 0;

    /* Rankings state */
    bool rankings_view = false;
    RankingsTab rank_tab = RANK_PLAYTIME;
    int  rank_sel    = 0;
    int  rank_scroll = 0;
    const PldSummary *ranked[RANK_MAX];
    int  rank_sess[RANK_MAX];
    int  rank_count  = 0;

    /* ── Input loop ── */
    bool quit_requested = false;
    while (!quit_requested && aptMainLoop()) {
        hidScanInput();
        u32 keys = hidKeysDown();
        u32 held = hidKeysHeld();
        u32 nav  = nav_tick(keys, held);

        if (charts_view) {
            /* ── Charts view (tabbed: pie / bar) ── */
            if (keys & KEY_B) {
                charts_view = false;
                list_anim_frame = 0;
            } else if (keys & KEY_L) {
                chart_tab = (chart_tab + CHART_TAB_COUNT - 1) % CHART_TAB_COUNT;
                chart_anim_frame = 0;
            } else if (keys & KEY_R) {
                chart_tab = (chart_tab + 1) % CHART_TAB_COUNT;
                chart_anim_frame = 0;
            }

            float anim_t = (float)chart_anim_frame / 40.0f;
            if (anim_t > 3.0f) anim_t = 3.0f;
            chart_anim_frame++;

            ui_begin_frame();
            ui_target_top();
            if (chart_tab == CHART_BAR)
                render_bar_top(pie_slices, pie_count, pie_total, anim_t);
            else
                render_pie_top(pie_slices, pie_count, pie_total, anim_t);
            ui_target_bot();
            render_pie_bot(pie_slices, pie_count, pie_total, anim_t);
            ui_end_frame();
        } else if (rankings_view) {
            /* ── Rankings view ── */
            if (keys & KEY_B) {
                rankings_view = false;
                list_anim_frame = 0;
            } else if (keys & KEY_L) {
                rank_tab = (rank_tab + RANK_TAB_COUNT - 1) % RANK_TAB_COUNT;
                rank_count = build_rankings(valid, n, rank_tab, &sessions, &pld,
                                            ranked, rank_sess);
                rank_sel = 0; rank_scroll = 0;
                rank_anim_frame = 0;
            } else if (keys & KEY_R) {
                rank_tab = (rank_tab + 1) % RANK_TAB_COUNT;
                rank_count = build_rankings(valid, n, rank_tab, &sessions, &pld,
                                            ranked, rank_sess);
                rank_sel = 0; rank_scroll = 0;
                rank_anim_frame = 0;
            }

            /* Navigation with hold-to-repeat */
            if (nav & KEY_DOWN) {
                if (rank_sel < rank_count - 1) {
                    rank_sel++;
                    if (rank_sel >= rank_scroll + UI_VISIBLE_ROWS)
                        rank_scroll = rank_sel - UI_VISIBLE_ROWS + 1;
                }
            } else if (nav & KEY_UP) {
                if (rank_sel > 0) {
                    rank_sel--;
                    if (rank_sel < rank_scroll)
                        rank_scroll = rank_sel;
                }
            }

            if ((keys & KEY_A) && rank_count > 0) {
                /* Open detail for ranked[rank_sel] */
                const PldSummary *det_s = ranked[rank_sel];
                const char *det_name = title_name_lookup(det_s->title_id);
                if (!det_name) det_name = title_db_lookup(det_s->title_id);
                char det_fallback[32];
                if (!det_name) {
                    snprintf(det_fallback, sizeof(det_fallback), "0x%016llX",
                             (unsigned long long)det_s->title_id);
                    det_name = det_fallback;
                }

                int *det_indices = malloc(sessions.count * sizeof(int));
                int  det_count = 0;
                if (det_indices) {
                    for (int i = 0; i < sessions.count; i++) {
                        if (sessions.entries[i].title_id == det_s->title_id)
                            det_indices[det_count++] = i;
                    }
                    for (int i = 0; i < det_count - 1; i++) {
                        for (int j = i + 1; j < det_count; j++) {
                            if (sessions.entries[det_indices[i]].timestamp <
                                sessions.entries[det_indices[j]].timestamp) {
                                int tmp = det_indices[i];
                                det_indices[i] = det_indices[j];
                                det_indices[j] = tmp;
                            }
                        }
                    }
                }

                int detail_scroll = 0;
                bool detail_done = false;
                nav_held_key = 0; nav_held_frames = 0;
                while (!detail_done && aptMainLoop()) {
                    hidScanInput();
                    u32 dkeys = hidKeysDown();
                    u32 dheld = hidKeysHeld();
                    u32 dnav  = nav_tick(dkeys, dheld);
                    if (dkeys & KEY_B) {
                        detail_done = true;
                    } else if (dnav & KEY_DOWN) {
                        if (detail_scroll < det_count - DETAIL_VISIBLE)
                            detail_scroll++;
                    } else if (dnav & KEY_UP) {
                        if (detail_scroll > 0)
                            detail_scroll--;
                    }

                    if (!detail_done) {
                        ui_begin_frame();
                        ui_target_top();
                        render_detail_top(det_s, det_name, &sessions,
                                          det_indices, det_count,
                                          detail_scroll);
                        ui_target_bot();
                        render_detail_bot();
                        ui_end_frame();
                    }
                }
                free(det_indices);
            }

            /* Render rankings */
            float rank_anim_t = (float)rank_anim_frame / 40.0f;
            if (rank_anim_t > 2.0f) rank_anim_t = 2.0f;
            rank_anim_frame++;

            ui_begin_frame();
            ui_target_top();
            render_rankings_top(ranked, rank_count, rank_sel, rank_scroll,
                                rank_sess, rank_tab, rank_anim_t);
            ui_target_bot();
            render_rankings_bot(ranked, rank_count, rank_sess, rank_tab);
            ui_end_frame();
        } else if (menu_open) {
            /* ── Menu open: navigate and confirm ── */
            if (keys & KEY_UP) {
                if (menu_sel > 0) menu_sel--;
            } else if (keys & KEY_DOWN) {
                if (menu_sel < 6) menu_sel++;
            } else if (keys & KEY_B) {
                menu_open = false;
            } else if (keys & KEY_START) {
                /* Double-tap START to quit */
                quit_requested = true;
            } else if (keys & KEY_A) {
                switch (menu_sel) {
                    case 0: /* Charts */
                        pie_count = build_pie_data(valid, n, pie_slices, &pie_total);
                        charts_view = true;
                        chart_tab = CHART_PIE;
                        chart_anim_frame = 0;
                        menu_open = false;
                        break;

                    case 1: /* Sync */
                        run_sync_flow(&pld, &sessions, &sync_count,
                                      status_msg, sizeof(status_msg));
                        n = collect_valid(&pld, valid, show_system, show_unknown);
                        sort_valid(valid, n, sort_mode, &sessions, &pld);
                        /* Fetch icons for any titles that arrived from the
                         * sync partner and aren't already in the store. */
                        {
                            IconFetchArgs psif_args = { valid, n };
                            run_loading_with_spinner("Activity Log++",
                                "Fetching missing icons (this may take a moment)...",
                                icon_fetch_work, &psif_args);
                        }
                        sel = 0; scroll_top = 0; scroll_y = 0.0f;
                        list_anim_frame = 0;
                        menu_open = false;
                        break;

                    case 2: /* Backup */
                        {
                            Result bk_rc = pld_backup_from_path(PLD_MERGED_PATH);
                            if (R_SUCCEEDED(bk_rc))
                                snprintf(status_msg, sizeof(status_msg), "Backup OK");
                            else
                                snprintf(status_msg, sizeof(status_msg),
                                         "Backup failed: 0x%08lX", bk_rc);
                        }
                        menu_open = false;
                        break;

                    case 3: /* Export */
                        {
                            ExportArgs exp_args = { &pld, &sessions, -1 };
                            run_loading_with_spinner("Activity Log++", "Exporting data...",
                                                     export_work, &exp_args);
                            if (R_SUCCEEDED(exp_args.rc))
                                snprintf(status_msg, sizeof(status_msg), "Exported to SD");
                            else
                                snprintf(status_msg, sizeof(status_msg), "Export failed");
                        }
                        menu_open = false;
                        break;

                    case 4: /* Restore */
                        {
                            PldBackupList bklist;
                            Result list_rc = pld_list_backups(&bklist);
                            if (R_FAILED(list_rc) || bklist.count == 0) {
                                snprintf(status_msg, sizeof(status_msg),
                                         bklist.count == 0 ? "No backups found"
                                                           : "Error listing backups");
                            } else {
                                /* Pre-load app counts */
                                int app_counts[PLD_MAX_BACKUPS] = {0};
                                for (int i = 0; i < bklist.count; i++) {
                                    char fp[128];
                                    snprintf(fp, sizeof(fp), "%s/%s",
                                             PLD_BACKUP_DIR, bklist.names[i]);
                                    pld_backup_app_count(fp, &app_counts[i]);
                                }

                                int  chooser_sel  = 0;
                                bool chooser_done = false;
                                nav_held_key = 0; nav_held_frames = 0;
                                while (!chooser_done && aptMainLoop()) {
                                    hidScanInput();
                                    u32 ckeys = hidKeysDown();
                                    u32 cheld = hidKeysHeld();
                                    u32 cnav  = nav_tick(ckeys, cheld);
                                    if (ckeys & KEY_B) {
                                        status_msg[0] = '\0';
                                        chooser_done = true;
                                    } else if (cnav & KEY_UP) {
                                        if (chooser_sel > 0) chooser_sel--;
                                    } else if (cnav & KEY_DOWN) {
                                        if (chooser_sel < bklist.count - 1) chooser_sel++;
                                    } else if (ckeys & KEY_A) {
                                        char full_path[128];
                                        snprintf(full_path, sizeof(full_path), "%s/%s",
                                                 PLD_BACKUP_DIR, bklist.names[chooser_sel]);

                                        PldFile       rst_pld;
                                        PldSessionLog rst_sessions = {NULL, 0};
                                        Result rst_rc = pld_read_sd(full_path, &rst_pld, &rst_sessions);
                                        if (R_SUCCEEDED(rst_rc)) {
                                            rst_rc = pld_write_sd(PLD_MERGED_PATH, &rst_pld, &rst_sessions);
                                        }
                                        if (R_SUCCEEDED(rst_rc)) {
                                            pld_sessions_free(&sessions);
                                            pld      = rst_pld;
                                            sessions = rst_sessions;
                                            n = collect_valid(&pld, valid, show_system, show_unknown);
                                            sort_valid(valid, n, sort_mode, &sessions, &pld);
                                            sel = 0; scroll_top = 0; scroll_y = 0.0f;
                                            list_anim_frame = 0;
                                        } else {
                                            pld_sessions_free(&rst_sessions);
                                        }

                                        if (R_SUCCEEDED(rst_rc))
                                            snprintf(status_msg, sizeof(status_msg), "Restore OK");
                                        else
                                            snprintf(status_msg, sizeof(status_msg),
                                                     "Restore failed: 0x%08lX", rst_rc);
                                        chooser_done = true;
                                    }

                                    if (!chooser_done) {
                                        ui_begin_frame();
                                        ui_target_top();
                                        ui_draw_rect(0, 0, UI_TOP_W, UI_HEADER_H, UI_COL_HEADER);
                                        ui_draw_text(6, 4, UI_SCALE_HDR, UI_COL_HEADER_TXT,
                                                     "Restore from Backup");
                                        ui_draw_text(6, 28, UI_SCALE_SM, UI_COL_TEXT_DIM,
                                                     "Up/Down:select  A:restore  B:cancel");
                                        for (int i = 0; i < bklist.count; i++) {
                                            float ry = 46.0f + (float)i * 18.0f;
                                            u32 rbg = (i == chooser_sel) ? UI_COL_ROW_SEL :
                                                      (i % 2 == 0) ? UI_COL_BG : UI_COL_ROW_ALT;
                                            ui_draw_rect(0, ry, UI_TOP_W, 18, rbg);
                                            char label[32];
                                            fmt_backup_label(bklist.names[i], label, sizeof(label));
                                            ui_draw_textf(6, ry + 2, UI_SCALE_LG, UI_COL_TEXT,
                                                          "%s  %d apps", label, app_counts[i]);
                                        }
                                        ui_target_bot();
                                        ui_draw_rect(0, 0, UI_BOT_W, UI_HEADER_H, UI_COL_HEADER);
                                        ui_draw_text(6, 4, UI_SCALE_HDR, UI_COL_HEADER_TXT,
                                                     "Activity Log++");
                                        ui_end_frame();
                                    }
                                }
                            }
                        }
                        menu_open = false;
                        break;

                    case 5: /* Reset */
                        {
                            /* Confirmation sub-loop */
                            bool rst_confirmed = false;
                            bool rst_done = false;
                            while (!rst_done && aptMainLoop()) {
                                hidScanInput();
                                u32 rkeys = hidKeysDown();
                                if (rkeys & KEY_A) {
                                    rst_confirmed = true;
                                    rst_done = true;
                                } else if (rkeys & (KEY_B | KEY_START)) {
                                    rst_done = true;
                                }
                                if (!rst_done) {
                                    draw_message_screen("Reset to Local",
                                        "Reset to local activity data?\n\n"
                                        "NOTE: This will remove data on\n"
                                        "this console from any synced systems\n\n"
                                        "A: confirm   B: cancel");
                                }
                            }
                            if (rst_confirmed) {
                                ResetReadArgs rr_args;
                                memset(&rr_args, 0, sizeof(rr_args));
                                rr_args.region_ids = region_ids;
                                rr_args.region_count = 4;
                                rr_args.rc = -1;
                                run_loading_with_spinner("Activity Log++", "Re-reading NAND data...",
                                                         reset_read_work, &rr_args);
                                if (R_SUCCEEDED(rr_args.rc)) {
                                    pld_sessions_free(&sessions);
                                    pld      = rr_args.pld;
                                    sessions = rr_args.sessions;
                                    n = collect_valid(&pld, valid, show_system, show_unknown);
                                    sort_valid(valid, n, sort_mode, &sessions, &pld);
                                    sel = 0; scroll_top = 0; scroll_y = 0.0f;
                                    list_anim_frame = 0;
                                    sync_count = 0;
                                    save_sync_count(0);
                                    snprintf(status_msg, sizeof(status_msg), "Reset to local data");
                                } else {
                                    snprintf(status_msg, sizeof(status_msg),
                                             "Reset failed: 0x%08lX", rr_args.rc);
                                }
                            }
                        }
                        menu_open = false;
                        break;

                    case 6: /* Quit */
                        quit_requested = true;
                        break;
                }
            }
        } else {
            /* ── Menu closed: viewer navigation ── */
            if (keys & KEY_START) {
                menu_open = true;
                menu_sel  = 0;
            }

            else if (keys & KEY_X) {
                rankings_view = true;
                rank_tab = RANK_PLAYTIME;
                rank_sel = 0; rank_scroll = 0;
                rank_anim_frame = 0;
                rank_count = build_rankings(valid, n, rank_tab, &sessions, &pld,
                                            ranked, rank_sess);
            } else if (keys & KEY_Y) {
                /* Cycle filter: games → games+sys → all → games */
                if (!show_system && !show_unknown) {
                    show_system = true;  show_unknown = false;
                } else if (show_system && !show_unknown) {
                    show_system = true;  show_unknown = true;
                } else {
                    show_system = false; show_unknown = false;
                }
                n = collect_valid(&pld, valid, show_system, show_unknown);
                sort_valid(valid, n, sort_mode, &sessions, &pld);
                sel = 0; scroll_top = 0; scroll_y = 0.0f;
                status_msg[0] = '\0';
                list_anim_frame = 0;
            } else if (keys & KEY_L) {
                sort_mode = (sort_mode + SORT_COUNT - 1) % SORT_COUNT;
                sort_valid(valid, n, sort_mode, &sessions, &pld);
                sel = 0; scroll_top = 0; scroll_y = 0.0f;
                status_msg[0] = '\0';
                list_anim_frame = 0;
            } else if (keys & KEY_R) {
                sort_mode = (sort_mode + 1) % SORT_COUNT;
                sort_valid(valid, n, sort_mode, &sessions, &pld);
                sel = 0; scroll_top = 0; scroll_y = 0.0f;
                status_msg[0] = '\0';
                list_anim_frame = 0;
            }

            /* Navigation with hold-to-repeat */
            if (nav & KEY_DOWN) {
                if (sel < n - 1) {
                    sel++;
                    if (sel >= scroll_top + UI_VISIBLE_ROWS)
                        scroll_top = sel - UI_VISIBLE_ROWS + 1;
                }
            } else if (nav & KEY_UP) {
                if (sel > 0) {
                    sel--;
                    if (sel < scroll_top)
                        scroll_top = sel;
                }
            }

            if ((keys & KEY_A) && n > 0) {
                /* ── Detail screen sub-loop ── */
                const PldSummary *det_s = valid[sel];
                const char *det_name = title_name_lookup(det_s->title_id);
                if (!det_name) det_name = title_db_lookup(det_s->title_id);
                char det_fallback[32];
                if (!det_name) {
                    snprintf(det_fallback, sizeof(det_fallback), "0x%016llX",
                             (unsigned long long)det_s->title_id);
                    det_name = det_fallback;
                }

                /* Build filtered session index list, sorted by timestamp desc */
                int *det_indices = malloc(sessions.count * sizeof(int));
                int  det_count = 0;
                if (det_indices) {
                    for (int i = 0; i < sessions.count; i++) {
                        if (sessions.entries[i].title_id == det_s->title_id)
                            det_indices[det_count++] = i;
                    }
                    /* Sort descending by timestamp (most recent first) */
                    for (int i = 0; i < det_count - 1; i++) {
                        for (int j = i + 1; j < det_count; j++) {
                            if (sessions.entries[det_indices[i]].timestamp <
                                sessions.entries[det_indices[j]].timestamp) {
                                int tmp = det_indices[i];
                                det_indices[i] = det_indices[j];
                                det_indices[j] = tmp;
                            }
                        }
                    }
                }

                int detail_scroll = 0;
                bool detail_done = false;
                nav_held_key = 0; nav_held_frames = 0;
                while (!detail_done && aptMainLoop()) {
                    hidScanInput();
                    u32 dkeys = hidKeysDown();
                    u32 dheld = hidKeysHeld();
                    u32 dnav  = nav_tick(dkeys, dheld);
                    if (dkeys & KEY_B) {
                        detail_done = true;
                    } else if (dnav & KEY_DOWN) {
                        if (detail_scroll < det_count - DETAIL_VISIBLE)
                            detail_scroll++;
                    } else if (dnav & KEY_UP) {
                        if (detail_scroll > 0)
                            detail_scroll--;
                    }

                    if (!detail_done) {
                        ui_begin_frame();
                        ui_target_top();
                        render_detail_top(det_s, det_name, &sessions,
                                          det_indices, det_count,
                                          detail_scroll);
                        ui_target_bot();
                        render_detail_bot();
                        ui_end_frame();
                    }
                }
                free(det_indices);
            }
        }

        if (!charts_view && !rankings_view) {
            /* Smooth scroll interpolation */
            float scroll_target = (float)scroll_top * UI_ROW_H;
            scroll_y = lerpf(scroll_y, scroll_target, 0.3f);
            if (scroll_y - scroll_target < 0.5f && scroll_y - scroll_target > -0.5f)
                scroll_y = scroll_target;

            float list_anim_t = (float)list_anim_frame / 40.0f;
            if (list_anim_t > 2.0f) list_anim_t = 2.0f;
            list_anim_frame++;

            ui_begin_frame();
            ui_target_top();
            render_game_list(valid, n, sel, scroll_y, &sessions, status_msg,
                             show_system, show_unknown, sort_mode, list_anim_t);
            if (menu_open) render_menu(menu_sel);
            ui_target_bot();
            render_bottom_stats(valid, n, &sessions, sync_count);
            ui_end_frame();
        }
    }

    pld_sessions_free(&sessions);
    title_icons_free();
    title_names_free();
    ui_fini();
    fsExit();
    gfxExit();
    return 0;
}
