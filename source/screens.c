#include <stdio.h>
#include <string.h>
#include <math.h>
#include <3ds.h>

#include "screens.h"
#include "ui.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ── Transient screen helpers ──────────────────────────────────── */

void draw_spinner(float cx, float cy)
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

void draw_message_screen_ex(const char *title, const char *body,
                            bool show_spinner)
{
    ui_begin_frame();

    ui_target_top();
    ui_draw_header(UI_TOP_W);
    ui_draw_text(6, 4, UI_SCALE_HDR, UI_COL_HEADER_TXT, title);

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
    ui_draw_header(UI_BOT_W);
    ui_draw_text(6, 4, UI_SCALE_HDR, UI_COL_HEADER_TXT, "Activity Log++");

    ui_end_frame();
}

void draw_message_screen(const char *title, const char *body)
{
    draw_message_screen_ex(title, body, false);
}

void draw_loading_screen(const char *title, const char *body)
{
    draw_message_screen_ex(title, body, true);
}

void draw_progress_screen(const char *title, const char *body,
                          int step, int total_steps)
{
    ui_begin_frame();

    ui_target_top();
    ui_draw_header(UI_TOP_W);
    ui_draw_text(6, 4, UI_SCALE_HDR, UI_COL_HEADER_TXT, title);

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
    ui_draw_header(UI_BOT_W);
    ui_draw_text(6, 4, UI_SCALE_HDR, UI_COL_HEADER_TXT, "Activity Log++");

    ui_end_frame();
}

/* ── Background-thread spinner helper ───────────────────────────── */

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

void run_with_spinner(const char *title, const char *body,
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
        if (step > 0)
            draw_progress_screen(title, body, step, total_steps);
        else
            draw_loading_screen(title, body);
        func(arg);
    }
}

void run_loading_with_spinner(const char *title, const char *body,
                              WorkerFunc func, void *arg)
{
    run_with_spinner(title, body, 0, 0, func, arg);
}
