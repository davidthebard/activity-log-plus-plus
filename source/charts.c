#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <3ds.h>

#include "charts.h"
#include "ui.h"
#include "pld.h"
#include "title_names.h"
#include "title_db.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

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

int build_pie_data(const PldSummary *valid[], int n,
                   PieSlice slices[], u32 *total_out)
{
    if (n <= 0) { *total_out = 0; return 0; }

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
    float step = 5.0f * (float)M_PI / 180.0f;
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

void render_pie_top(const PieSlice slices[], int slice_count, u32 total,
                    float anim_t)
{
    ui_draw_header(UI_TOP_W);
    ui_draw_text(6, 4, UI_SCALE_HDR, UI_COL_HEADER_TXT, "Charts: Pie");

    if (slice_count == 0 || total == 0) {
        ui_draw_text(8, 36, UI_SCALE_LG, UI_COL_TEXT_DIM, "No playtime data");
        return;
    }

    float cx = 110.0f, cy = 132.0f, r = 80.0f;
    float angle = -((float)M_PI / 2.0f);
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

    float lx = 210.0f, ly_base = 30.0f;
    char fallback[32];
    for (int i = 0; i < slice_count; i++) {
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

    ui_draw_status_bar(UI_TOP_W);
    ui_draw_text_right(396, 222, UI_SCALE_SM, UI_COL_STATUS_TXT, "L/R:tab  B:back");
}

void render_pie_bot(const PieSlice slices[], int slice_count, u32 total,
                    float anim_t)
{
    ui_draw_header(UI_BOT_W);
    ui_draw_text(6, 4, UI_SCALE_HDR, UI_COL_HEADER_TXT, "Charts");

    if (slice_count == 0 || total == 0) {
        ui_draw_text(8, 36, UI_SCALE_LG, UI_COL_TEXT_DIM, "No data");
        return;
    }

    float row_h = 18.0f;
    float y_base = 28.0f;
    float y_last = y_base;
    char fallback[32];
    char t_buf[20];
    for (int i = 0; i < slice_count; i++) {
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

        ui_draw_rect(8, y + 2, 8, 8, col_a);

        char detail[32];
        snprintf(detail, sizeof(detail), "%s  %d%%", t_buf, pct_int);
        float detail_w = ui_text_width(detail, UI_SCALE_SM);
        ui_draw_text_trunc(20, y, UI_SCALE_SM, txt_col, name,
                           (UI_BOT_W - 8) - detail_w - 4 - 20);
        ui_draw_text_right(UI_BOT_W - 8, y, UI_SCALE_SM, dim_col, detail);
    }

    float y = y_last;
    y += 2.0f;
    ui_draw_rect(0, y, UI_BOT_W, 1, UI_COL_DIVIDER);
    ui_draw_grad_v(0, y + 1, UI_BOT_W, 2,
                   C2D_Color32(0x00, 0x00, 0x00, 0x10), UI_COL_SHADOW_NONE);
    y += 4.0f;
    pld_fmt_time(total, t_buf, sizeof(t_buf));
    ui_draw_text(8, y, UI_SCALE_SM, UI_COL_TEXT, "Total");
    ui_draw_text_right(UI_BOT_W - 8, y, UI_SCALE_SM, UI_COL_TEXT, t_buf);

    ui_draw_status_bar(UI_BOT_W);
    ui_draw_text_right(UI_BOT_W - 4, 222, UI_SCALE_SM, UI_COL_STATUS_TXT,
                       "L/R:tab  B:back");
}

void render_bar_top(const PieSlice slices[], int slice_count, u32 total,
                    float anim_t)
{
    ui_draw_header(UI_TOP_W);
    ui_draw_text(6, 4, UI_SCALE_HDR, UI_COL_HEADER_TXT, "Charts: Bar");

    if (slice_count == 0 || total == 0) {
        ui_draw_text(8, 36, UI_SCALE_LG, UI_COL_TEXT_DIM, "No playtime data");
        ui_draw_status_bar(UI_TOP_W);
        ui_draw_text_right(396, 222, UI_SCALE_SM, UI_COL_STATUS_TXT, "L/R:tab  B:back");
        return;
    }

    u32 max_secs = 0;
    for (int i = 0; i < slice_count; i++)
        if (slices[i].secs > max_secs) max_secs = slices[i].secs;

    float bar_max_w = 200.0f;
    float row_h = 20.0f;
    float y_base = (float)UI_HEADER_H + 4.0f;
    char fallback[32];
    char t_buf[20];

    for (int i = 0; i < slice_count; i++) {
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

        float bar_w = (max_secs > 0)
                      ? (float)slices[i].secs / (float)max_secs * bar_max_w
                      : 0.0f;
        bar_w *= reveal;
        if (bar_w < 2.0f && slices[i].secs > 0 && reveal > 0.0f) bar_w = 2.0f;
        ui_draw_rect(8, y + 2, bar_w, 14, bar_col);

        pld_fmt_time(slices[i].secs, t_buf, sizeof(t_buf));
        float time_w = ui_text_width(t_buf, UI_SCALE_SM);
        ui_draw_text_right(396, y + 1, UI_SCALE_SM, dim_col, t_buf);

        ui_draw_text_trunc(bar_max_w + 16, y + 1, UI_SCALE_SM, txt_col, name,
                           396 - time_w - 4 - (bar_max_w + 16));
    }

    ui_draw_status_bar(UI_TOP_W);
    ui_draw_text_right(396, 222, UI_SCALE_SM, UI_COL_STATUS_TXT, "L/R:tab  B:back");
}
