#pragma once
#include <3ds.h>
#include "pld.h"

#define PIE_SLICES 8

typedef struct {
    const PldSummary *s;  /* NULL for "Other" */
    u32 secs;
    float pct;            /* 0.0-1.0 */
} PieSlice;

int  build_pie_data(const PldSummary *valid[], int n,
                    PieSlice slices[], u32 *total_out);

void render_pie_top(const PieSlice slices[], int slice_count, u32 total,
                    float anim_t);
void render_pie_bot(const PieSlice slices[], int slice_count, u32 total,
                    float anim_t);
void render_bar_top(const PieSlice slices[], int slice_count, u32 total,
                    float anim_t);
