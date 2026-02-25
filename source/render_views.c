#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <3ds.h>

#include "render_views.h"
#include "ui.h"
#include "pld.h"
#include "title_names.h"
#include "title_db.h"
#include "title_icons.h"

/* ── View mode labels ───────────────────────────────────────────── */

const char *view_labels[VIEW_COUNT] = {
    "Last Played", "Playtime", "Launches", "Avg Session", "First Played", "Name"
};

/* ── Hold-to-repeat navigation ─────────────────────────────────── */

#define NAV_INITIAL_DELAY  18
#define NAV_REPEAT_RATE     4

static u32 nav_held_key    = 0;
static int nav_held_frames = 0;

u32 nav_tick(u32 keys_down, u32 keys_held)
{
    u32 dir_down = keys_down & (KEY_UP | KEY_DOWN);
    u32 dir_held = keys_held & (KEY_UP | KEY_DOWN);
    u32 fire = 0;

    if (dir_down) {
        nav_held_key    = dir_down;
        nav_held_frames = 0;
        fire = dir_down;
    } else if (dir_held && dir_held == nav_held_key) {
        nav_held_frames++;
        if (nav_held_frames >= NAV_INITIAL_DELAY &&
            ((nav_held_frames - NAV_INITIAL_DELAY) % NAV_REPEAT_RATE == 0))
            fire = nav_held_key;
    } else {
        nav_held_key    = 0;
        nav_held_frames = 0;
    }
    return fire;
}

void nav_reset(void) {
    nav_held_key    = 0;
    nav_held_frames = 0;
}

float lerpf(float a, float b, float t) {
    return a + (b - a) * t;
}

/* ── Helpers ────────────────────────────────────────────────────── */

void fmt_backup_label(const char *name, char *out, size_t len)
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
bool should_show(u64 title_id, bool show_system)
{
    u32 hi = (u32)(title_id >> 32);
    if (hi == 0x00040000 || hi == 0x00048004) return true;
    if (show_system && (hi == 0x00040010 || hi == 0x00040030)) return true;
    return false;
}

int collect_valid(const PldFile *pld,
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

/* ── Sort comparators ──────────────────────────────────────────── */

static int cmp_last_played(const void *a, const void *b) {
    const PldSummary *sa = *(const PldSummary *const *)a;
    const PldSummary *sb = *(const PldSummary *const *)b;
    if (sb->last_played_days != sa->last_played_days)
        return (int)sb->last_played_days - (int)sa->last_played_days;
    return 0;
}

static int cmp_first_played(const void *a, const void *b) {
    const PldSummary *sa = *(const PldSummary *const *)a;
    const PldSummary *sb = *(const PldSummary *const *)b;
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

void sort_valid(const PldSummary *valid[], int n,
                ViewMode mode, const PldSessionLog *sessions,
                const PldFile *pld)
{
    if (n <= 1) return;

    typedef int (*cmp_fn)(const void *, const void *);
    cmp_fn cmp = NULL;
    switch (mode) {
        case VIEW_LAST_PLAYED:  cmp = cmp_last_played;  break;
        case VIEW_FIRST_PLAYED: cmp = cmp_first_played; break;
        case VIEW_NAME:         cmp = cmp_name;         break;
        default: return;
    }
    qsort(valid, (size_t)n, sizeof(valid[0]), cmp);
}

/* ── Rankings builder ──────────────────────────────────────────── */

static const PldSummary *sort_summaries_base;
static u32 rank_avg_cache[PLD_SUMMARY_COUNT];

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

static int cmp_rank_avg_session(const void *a, const void *b) {
    const PldSummary *sa = *(const PldSummary *const *)a;
    const PldSummary *sb = *(const PldSummary *const *)b;
    int ia = (int)(sa - sort_summaries_base);
    int ib = (int)(sb - sort_summaries_base);
    if (rank_avg_cache[ib] != rank_avg_cache[ia])
        return (rank_avg_cache[ib] > rank_avg_cache[ia]) ? 1 : -1;
    return 0;
}

int build_rankings(const PldSummary *valid[], int n,
                   ViewMode mode, const PldSessionLog *sessions,
                   const PldFile *pld,
                   const PldSummary *ranked[RANK_MAX],
                   u32 rank_metric_out[RANK_MAX])
{
    if (n <= 0) return 0;

    const PldSummary **tmp = malloc((size_t)n * sizeof(tmp[0]));
    if (!tmp) return 0;
    memcpy(tmp, valid, (size_t)n * sizeof(tmp[0]));

    if (mode == VIEW_AVG_SESSION) {
        sort_summaries_base = pld->summaries;
        for (int i = 0; i < n; i++) {
            int slot = (int)(tmp[i] - pld->summaries);
            rank_avg_cache[slot] = (tmp[i]->launch_count > 0)
                ? (tmp[i]->total_secs / tmp[i]->launch_count) : 0;
        }
    }

    typedef int (*cmp_fn)(const void *, const void *);
    cmp_fn cmp = NULL;
    switch (mode) {
        case VIEW_PLAYTIME:    cmp = cmp_rank_playtime;    break;
        case VIEW_LAUNCHES:    cmp = cmp_rank_launches;    break;
        case VIEW_AVG_SESSION: cmp = cmp_rank_avg_session; break;
        default: free(tmp); return 0;
    }
    qsort(tmp, (size_t)n, sizeof(tmp[0]), cmp);

    int count = (n < RANK_MAX) ? n : RANK_MAX;
    for (int i = 0; i < count; i++) {
        ranked[i] = tmp[i];
        rank_metric_out[i] = (tmp[i]->launch_count > 0)
            ? (tmp[i]->total_secs / tmp[i]->launch_count) : 0;
    }
    free(tmp);
    return count;
}

/* ── Game list rendering ───────────────────────────────────────── */

void render_game_list(const PldSummary *const valid[], int n,
                      int sel, float scroll_y,
                      const PldSessionLog *sessions,
                      const char *status_msg,
                      bool show_system, bool show_unknown,
                      ViewMode mode, float anim_t,
                      float sel_pop)
{
    ui_draw_rect(0, UI_LIST_Y, UI_TOP_W, UI_LIST_BOT - UI_LIST_Y, UI_COL_LIST_BG);

    char t_buf[20];
    char d0_buf[12];
    char d1_buf[12];
    char fallback[32];

    int first_vis = (int)(scroll_y / UI_ROW_PITCH);
    if (first_vis < 0) first_vis = 0;
    int last_vis = first_vis + UI_VISIBLE_ROWS + 2;
    if (last_vis > n) last_vis = n;

    for (int i = first_vis; i < last_vis; i++) {
        float row_y = UI_LIST_Y + (float)i * UI_ROW_PITCH - scroll_y;
        if (row_y + UI_ROW_H + UI_ROW_GAP < UI_LIST_Y || row_y >= UI_LIST_BOT) continue;

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

        bool selected = (i == sel);
        float pop = selected ? sel_pop : 0.0f;
        float grow = pop * 4.0f;
        row_y -= grow * 0.5f;
        float row_h = (float)UI_ROW_H + grow;

        float icon_sz = (float)ICON_DRAW_SIZE + grow;
        float icon_x = (float)UI_ROW_MARGIN - grow * 0.5f;
        float icon_y = row_y;
        float icon_r = (float)UI_ROW_RADIUS;
        C2D_Image icon;

        u8 sh_base = (u8)(0x38 + (u32)(pop * (0x70 - 0x38)));
        u8 sh_alpha = (u8)((u32)sh_base * alpha / 255);

        ui_draw_drop_shadow(icon_x, icon_y, icon_sz, icon_sz, icon_r, sh_alpha);

        if (title_icon_get(s->title_id, &icon)) {
            if (alpha == 255)
                ui_draw_image(icon, icon_x, icon_y, icon_sz);
            else
                ui_draw_image_alpha(icon, icon_x, icon_y, icon_sz, alpha);
            ui_draw_rounded_mask(icon_x, icon_y, icon_sz, icon_sz,
                                 icon_r, UI_COL_LIST_BG);
        } else {
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
            ui_draw_rounded_rect(icon_x, icon_y, icon_sz, icon_sz, icon_r,
                                 (icon_col & 0x00FFFFFF) | ((u32)alpha << 24));
            char letter[2] = { name[0] ? name[0] : '?', '\0' };
            ui_draw_text(icon_x + 12.0f, icon_y + 14.0f, UI_SCALE_LG,
                         (0xFFFFFFFF & 0x00FFFFFF) | ((u32)alpha << 24), letter);
        }

        float card_x = (float)(UI_ROW_MARGIN + ICON_DRAW_SIZE + UI_ICON_GAP);
        float card_w = (float)(UI_TOP_W - UI_ROW_MARGIN) - card_x;
        float card_r = (float)UI_ROW_RADIUS;

        ui_draw_drop_shadow(card_x, row_y, card_w, row_h, card_r, sh_alpha);

        if (selected) {
            ui_draw_rounded_rect(card_x - 1, row_y - 1, card_w + 2, row_h + 2,
                                 card_r + 1,
                                 (UI_COL_SEL_BORDER & 0x00FFFFFF) | ((u32)alpha << 24));
            ui_draw_rounded_rect(card_x, row_y, card_w, row_h, card_r,
                                 (UI_COL_ROW_SEL & 0x00FFFFFF) | ((u32)alpha << 24));
        } else {
            ui_draw_rounded_rect(card_x, row_y, card_w, row_h, card_r,
                                 (UI_COL_CARD & 0x00FFFFFF) | ((u32)alpha << 24));
        }

        u32 text_col     = (UI_COL_TEXT     & 0x00FFFFFF) | ((u32)alpha << 24);
        u32 text_dim_col = (UI_COL_TEXT_DIM & 0x00FFFFFF) | ((u32)alpha << 24);
        float text_x = card_x + 6.0f;
        float text_r = (float)(UI_TOP_W - UI_ROW_MARGIN) - 6.0f;
        ui_draw_text(text_x, row_y + 8, UI_SCALE_LG, text_col, name);
        ui_draw_text_right(text_r, row_y + 8, UI_SCALE_LG, text_dim_col, t_buf);
        {
            u32 avg_secs = (s->launch_count > 0) ? (s->total_secs / s->launch_count) : 0;
            char avg_buf[20];
            pld_fmt_time(avg_secs, avg_buf, sizeof(avg_buf));
            ui_draw_textf(text_x, row_y + 28, UI_SCALE_SM, text_dim_col,
                          "L:%u  Avg:%s  %s-%s",
                          (unsigned)s->launch_count, avg_buf, d0_buf, d1_buf);
        }
    }

    ui_draw_header(UI_TOP_W);
    ui_draw_text(6, 4, UI_SCALE_HDR, UI_COL_HEADER_TXT, "Activity Log++");
    {
        char hbuf[32];
        snprintf(hbuf, sizeof(hbuf), "Sort: %s", view_labels[mode]);
        ui_draw_text_right(UI_TOP_W - 6, 4, UI_SCALE_HDR, UI_COL_HEADER_TXT, hbuf);
    }
}

/* ── Bottom stats ──────────────────────────────────────────────── */

void render_bottom_stats(const PldSummary *valid[], int n,
                         const PldSessionLog *sessions,
                         u32 sync_count,
                         const char *status_msg,
                         bool show_system, bool show_unknown)
{
    ui_draw_header(UI_BOT_W);
    ui_draw_text(6, 4, UI_SCALE_HDR, UI_COL_HEADER_TXT, "Statistics");

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

    {
        u32 avg_secs = (total_launches > 0) ? (total_secs / total_launches) : 0;
        char avg_buf[20];
        pld_fmt_time(avg_secs, avg_buf, sizeof(avg_buf));
        ui_draw_text(8, y, UI_SCALE_LG, UI_COL_TEXT, "Avg session");
        ui_draw_text_right(UI_BOT_W - 8, y, UI_SCALE_LG, UI_COL_TEXT_DIM, avg_buf);
    }
    y += 24.0f;

    ui_draw_text(8, y, UI_SCALE_LG, UI_COL_TEXT, "Most played");
    if (most_idx >= 0) {
        const char *name = title_name_lookup(valid[most_idx]->title_id);
        if (!name) name = title_db_lookup(valid[most_idx]->title_id);
        if (!name) name = "Unknown";
        ui_draw_text_trunc(UI_BOT_W - 8 - 160, y, UI_SCALE_LG,
                           UI_COL_TEXT_DIM, name, 160);
    }

    ui_draw_rect(0, 180, UI_BOT_W, 1, UI_COL_DIVIDER);
    ui_draw_grad_v(0, 181, UI_BOT_W, 2,
                   C2D_Color32(0x00, 0x00, 0x00, 0x10), UI_COL_SHADOW_NONE);

    if (status_msg && status_msg[0])
        ui_draw_text(4, 184, UI_SCALE_SM, UI_COL_STATUS_TXT, status_msg);
    else {
        const char *filter_label = show_unknown ? "All" :
                                   show_system  ? "Games+Sys" : "Games";
        ui_draw_textf(4, 184, UI_SCALE_SM, UI_COL_STATUS_TXT,
                      "%d %s  [%s]", n,
                      show_system ? "titles" : "games", filter_label);
    }

    ui_draw_text_right(UI_BOT_W - 4, 198, UI_SCALE_SM, UI_COL_TEXT_DIM,
                       "A:select  START:menu  Up/Dn:scroll");
    ui_draw_text_right(UI_BOT_W - 4, 212, UI_SCALE_SM, UI_COL_TEXT_DIM,
                       "L/R:mode  Y:filter");
}

/* ── Rankings rendering ─────────────────────────────────────────── */

void render_rankings_top(const PldSummary *ranked[], int rank_count,
                         int rank_sel, int rank_scroll,
                         const u32 rank_metric[], ViewMode mode,
                         float anim_t, float sel_pop)
{
    ui_draw_rect(0, UI_LIST_Y, UI_TOP_W, UI_LIST_BOT - UI_LIST_Y, UI_COL_LIST_BG);

    u32 max_val = 1;
    for (int i = 0; i < rank_count; i++) {
        u32 val = 0;
        switch (mode) {
            case VIEW_PLAYTIME:    val = ranked[i]->total_secs;   break;
            case VIEW_LAUNCHES:    val = ranked[i]->launch_count; break;
            case VIEW_AVG_SESSION: val = rank_metric[i];          break;
            default: break;
        }
        if (val > max_val) max_val = val;
    }

    char fallback[32];
    char d0_buf[12], d1_buf[12];

    for (int i = rank_scroll; i < rank_scroll + UI_VISIBLE_ROWS + 1 && i < rank_count; i++) {
        float row_y = UI_LIST_Y + (float)(i - rank_scroll) * UI_ROW_PITCH;
        const PldSummary *s = ranked[i];

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

        bool selected = (i == rank_sel);
        float pop = selected ? sel_pop : 0.0f;
        float grow = pop * 4.0f;
        row_y -= grow * 0.5f;
        float row_h = (float)UI_ROW_H + grow;

        float row_x = (float)UI_ROW_MARGIN;
        float row_w = (float)(UI_TOP_W - 2 * UI_ROW_MARGIN);
        float card_r = (float)UI_ROW_RADIUS;

        u32 val = 0;
        switch (mode) {
            case VIEW_PLAYTIME:    val = s->total_secs;   break;
            case VIEW_LAUNCHES:    val = s->launch_count; break;
            case VIEW_AVG_SESSION: val = rank_metric[i];  break;
            default: break;
        }
        float bar_max_w = row_w;
        float bar_w = bar_max_w * ((float)val / (float)max_val);
        if (bar_w < 2.0f) bar_w = 2.0f;
        u8 bar_base_alpha = 0x30;
        u8 bar_alpha = (u8)((u32)bar_base_alpha * alpha / 255);
        u32 bar_col = C2D_Color32(0x4A, 0x86, 0xC8, bar_alpha);
        ui_draw_rounded_rect(row_x, row_y, bar_w, row_h, card_r, bar_col);

        u8 sh_base = (u8)(0x38 + (u32)(pop * (0x70 - 0x38)));
        u8 sh_alpha = (u8)((u32)sh_base * alpha / 255);

        float icon_sz = (float)ICON_DRAW_SIZE + grow;
        float rank_icon_x = (float)UI_ROW_MARGIN - grow * 0.5f;
        float icon_y = row_y;
        float icon_r = (float)UI_ROW_RADIUS;
        C2D_Image icon;

        ui_draw_drop_shadow(rank_icon_x, icon_y, icon_sz, icon_sz, icon_r, sh_alpha);

        if (title_icon_get(s->title_id, &icon)) {
            if (alpha == 255)
                ui_draw_image(icon, rank_icon_x, icon_y, icon_sz);
            else
                ui_draw_image_alpha(icon, rank_icon_x, icon_y, icon_sz, alpha);
            ui_draw_rounded_mask(rank_icon_x, icon_y, icon_sz, icon_sz,
                                 icon_r, UI_COL_LIST_BG);
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
            ui_draw_rounded_rect(rank_icon_x, icon_y, icon_sz, icon_sz, icon_r,
                                 (icon_col & 0x00FFFFFF) | ((u32)alpha << 24));
            ui_draw_text(rank_icon_x + 12.0f, icon_y + 14.0f, UI_SCALE_LG,
                         (0xFFFFFFFF & 0x00FFFFFF) | ((u32)alpha << 24), letter);
        }

        float card_x = (float)(UI_ROW_MARGIN + ICON_DRAW_SIZE + UI_ICON_GAP);
        float card_w = (float)(UI_TOP_W - UI_ROW_MARGIN) - card_x;

        ui_draw_drop_shadow(card_x, row_y, card_w, row_h, card_r, sh_alpha);

        if (selected) {
            ui_draw_rounded_rect(card_x - 1, row_y - 1, card_w + 2, row_h + 2,
                                 card_r + 1,
                                 (UI_COL_SEL_BORDER & 0x00FFFFFF) | ((u32)alpha << 24));
            ui_draw_rounded_rect(card_x, row_y, card_w, row_h, card_r,
                                 (UI_COL_ROW_SEL & 0x00FFFFFF) | ((u32)alpha << 24));
        } else {
            ui_draw_rounded_rect(card_x, row_y, card_w, row_h, card_r,
                                 (UI_COL_CARD & 0x00FFFFFF) | ((u32)alpha << 24));
        }

        u32 text_col     = (UI_COL_TEXT     & 0x00FFFFFF) | ((u32)alpha << 24);
        u32 text_dim_col = (UI_COL_TEXT_DIM & 0x00FFFFFF) | ((u32)alpha << 24);
        float text_x = card_x + 6.0f;
        float text_r = (float)(UI_TOP_W - UI_ROW_MARGIN) - 6.0f;

        char rank_num[16];
        snprintf(rank_num, sizeof(rank_num), "#%d", i + 1);
        ui_draw_text(text_x, row_y + 16.0f, UI_SCALE_SM, text_dim_col, rank_num);

        const char *name = title_name_lookup(s->title_id);
        if (!name) name = title_db_lookup(s->title_id);
        if (!name) {
            snprintf(fallback, sizeof(fallback), "0x%016llX",
                     (unsigned long long)s->title_id);
            name = fallback;
        }
        ui_draw_text(text_x + 30.0f, row_y + 8, UI_SCALE_LG, text_col, name);

        char metric[24];
        switch (mode) {
            case VIEW_PLAYTIME:
                pld_fmt_time(s->total_secs, metric, sizeof(metric));
                break;
            case VIEW_LAUNCHES:
                snprintf(metric, sizeof(metric), "%u", (unsigned)s->launch_count);
                break;
            case VIEW_AVG_SESSION:
                pld_fmt_time(rank_metric[i], metric, sizeof(metric));
                break;
            default:
                metric[0] = '\0';
                break;
        }
        ui_draw_text_right(text_r, row_y + 8, UI_SCALE_LG, text_dim_col, metric);

        {
            char avg_buf[20];
            pld_fmt_time(rank_metric[i], avg_buf, sizeof(avg_buf));
            pld_fmt_date(s->first_played_days, d0_buf, sizeof(d0_buf));
            pld_fmt_date(s->last_played_days,  d1_buf, sizeof(d1_buf));
            ui_draw_textf(text_x + 30.0f, row_y + 28, UI_SCALE_SM, text_dim_col,
                          "L:%u  Avg:%s  %s-%s",
                          (unsigned)s->launch_count, avg_buf, d0_buf, d1_buf);
        }
    }

    if (rank_count == 0) {
        ui_draw_text(8, 36, UI_SCALE_LG, UI_COL_TEXT_DIM, "No titles to rank");
    }

    ui_draw_header(UI_TOP_W);
    ui_draw_text(6, 4, UI_SCALE_HDR, UI_COL_HEADER_TXT, "Rankings");
    ui_draw_text_right(UI_TOP_W - 6, 4, UI_SCALE_HDR, UI_COL_HEADER_TXT,
                       view_labels[mode]);
}

/* ── Detail screen ──────────────────────────────────────────────── */

void render_detail_top(const PldSummary *s, const char *name,
                       const PldSessionLog *sessions,
                       const int *sess_indices, int sess_count,
                       int detail_scroll)
{
    ui_draw_rect(0, 0, UI_TOP_W, UI_TOP_H, UI_COL_BG);

    ui_draw_header(UI_TOP_W);
    ui_draw_text(6, 4, UI_SCALE_HDR, UI_COL_HEADER_TXT, name);

    C2D_Image icon;
    if (title_icon_get(s->title_id, &icon)) {
        ui_draw_image(icon, 8.0f, 28.0f, 120.0f);
    } else {
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

    float sy = 30.0f;
    char tbuf[24];

    pld_fmt_time(s->total_secs, tbuf, sizeof(tbuf));
    ui_draw_textf(136, sy, UI_SCALE_LG, UI_COL_TEXT, "Playtime: %s", tbuf);
    sy += 18.0f;

    ui_draw_textf(136, sy, UI_SCALE_LG, UI_COL_TEXT, "Launches: %u",
                  (unsigned)s->launch_count);
    sy += 18.0f;

    {
        u32 avg_secs = (s->launch_count > 0) ? (s->total_secs / s->launch_count) : 0;
        char avg_buf[20];
        pld_fmt_time(avg_secs, avg_buf, sizeof(avg_buf));
        ui_draw_textf(136, sy, UI_SCALE_LG, UI_COL_TEXT, "Avg session: %s", avg_buf);
    }
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

    ui_draw_rect(0, 152, UI_TOP_W, 1, UI_COL_DIVIDER);
    ui_draw_grad_v(0, 153, UI_TOP_W, 2,
                   C2D_Color32(0x00, 0x00, 0x00, 0x10), UI_COL_SHADOW_NONE);

    ui_draw_text(6, 155, UI_SCALE_SM, UI_COL_TEXT_DIM, "Date/Time");
    ui_draw_text_right(394, 155, UI_SCALE_SM, UI_COL_TEXT_DIM, "Duration");

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

    if (sess_count == 0) {
        ui_draw_text(6, DETAIL_LIST_Y + 4, UI_SCALE_SM, UI_COL_TEXT_DIM,
                     "No sessions recorded");
    }
}

void render_detail_bot(void)
{
    ui_draw_header(UI_BOT_W);
    ui_draw_text(6, 4, UI_SCALE_HDR, UI_COL_HEADER_TXT, "Game Details");
    ui_draw_text_right(UI_BOT_W - 8, 36, UI_SCALE_LG, UI_COL_TEXT_DIM, "Up/Dn:scroll  B:back");
}

/* ── Menu overlay ───────────────────────────────────────────────── */

void render_menu(int sel)
{
    static const char *items[] = { "Charts", "Sync", "Backup", "Export", "Restore", "Reset", "Quit" };
    static const int   NITEMS  = 7;

    float mx     = 8.0f;
    float my     = 28.0f;
    float mw     = 140.0f;
    float item_h = 22.0f;
    float mh     = NITEMS * item_h + 8.0f;

    ui_draw_rect(mx, my, mw, mh, UI_COL_HEADER);
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
