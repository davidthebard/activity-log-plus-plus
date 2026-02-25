#include "ui.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static C3D_RenderTarget *s_top;
static C3D_RenderTarget *s_bot;
static C2D_TextBuf       s_textbuf;

void ui_init(void) {
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
    C2D_Init(C2D_DEFAULT_MAX_OBJECTS);
    C2D_Prepare();
    s_top     = C2D_CreateScreenTarget(GFX_TOP,    GFX_LEFT);
    s_bot     = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);
    s_textbuf = C2D_TextBufNew(16384);
}

void ui_fini(void) {
    C2D_TextBufDelete(s_textbuf);
    C2D_Fini();
    C3D_Fini();
}

void ui_begin_frame(void) {
    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
    C2D_TextBufClear(s_textbuf);
}

void ui_end_frame(void) {
    C3D_FrameEnd(0);
}

void ui_target_top(void) {
    C2D_TargetClear(s_top, UI_COL_BG);
    C2D_SceneBegin(s_top);
}

void ui_target_bot(void) {
    C2D_TargetClear(s_bot, UI_COL_BG);
    C2D_SceneBegin(s_bot);
}

void ui_draw_rect(float x, float y, float w, float h, u32 color) {
    C2D_DrawRectSolid(x, y, 0.5f, w, h, color);
}

void ui_draw_text(float x, float y, float scale, u32 color, const char *str) {
    C2D_Text t;
    C2D_TextParse(&t, s_textbuf, str);
    C2D_TextOptimize(&t);
    C2D_DrawText(&t, C2D_WithColor, x, y, 0.5f, scale, scale, color);
}

void ui_draw_text_right(float x, float y, float scale, u32 color, const char *str) {
    C2D_Text t;
    C2D_TextParse(&t, s_textbuf, str);
    C2D_TextOptimize(&t);
    C2D_DrawText(&t, C2D_WithColor | C2D_AlignRight, x, y, 0.5f, scale, scale, color);
}

void ui_draw_textf(float x, float y, float scale, u32 color, const char *fmt, ...) {
    char buf[128];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    ui_draw_text(x, y, scale, color, buf);
}

void ui_draw_image(C2D_Image img, float x, float y, float size) {
    float sx = size / (float)img.subtex->width;
    float sy = size / (float)img.subtex->height;
    C2D_DrawImageAt(img, x, y, 0.5f, NULL, sx, sy);
}

void ui_draw_image_alpha(C2D_Image img, float x, float y, float size, u8 alpha) {
    float sx = size / (float)img.subtex->width;
    float sy = size / (float)img.subtex->height;
    C2D_ImageTint tint;
    C2D_PlainImageTint(&tint, C2D_Color32(0xFF, 0xFF, 0xFF, alpha), 0.0f);
    C2D_DrawImageAt(img, x, y, 0.5f, &tint, sx, sy);
}

void ui_draw_triangle(float x0, float y0, float x1, float y1,
                      float x2, float y2, u32 color) {
    C2D_DrawTriangle(x0, y0, color, x1, y1, color, x2, y2, color, 0.5f);
}

void ui_draw_circle(float cx, float cy, float r, u32 color) {
    float step = 10.0f * 3.14159265f / 180.0f; /* ~10 degrees */
    float angle = 0.0f;
    float end = 2.0f * 3.14159265f;
    while (angle < end - 0.001f) {
        float next = angle + step;
        if (next > end) next = end;
        float x0 = cx + r * cosf(angle);
        float y0 = cy + r * sinf(angle);
        float x1 = cx + r * cosf(next);
        float y1 = cy + r * sinf(next);
        C2D_DrawTriangle(cx, cy, color, x0, y0, color, x1, y1, color, 0.5f);
        angle = next;
    }
}

void ui_draw_rounded_rect(float x, float y, float w, float h, float r, u32 color) {
    /* Clamp radius so it doesn't exceed half the dimension */
    if (r > w * 0.5f) r = w * 0.5f;
    if (r > h * 0.5f) r = h * 0.5f;

    /* 3 rectangles forming the cross shape */
    C2D_DrawRectSolid(x + r, y,     0.5f, w - 2*r, h,       color); /* horizontal strip */
    C2D_DrawRectSolid(x,     y + r, 0.5f, r,       h - 2*r, color); /* left strip       */
    C2D_DrawRectSolid(x + w - r, y + r, 0.5f, r, h - 2*r, color);  /* right strip      */

    /* 4 quarter-circle fans (~15° steps = 6 triangles per corner) */
    const float PI_2 = 1.5707963f; /* pi/2 */
    const int SEGS = 6;
    float step = PI_2 / (float)SEGS;

    /* corner centres */
    float cx[4] = { x + r,     x + w - r, x + w - r, x + r     };
    float cy[4] = { y + r,     y + r,     y + h - r, y + h - r  };
    /* start angles: TL=PI, TR=3PI/2, BR=0, BL=PI/2 */
    float sa[4] = { 3.14159265f, 3*PI_2, 0.0f, PI_2 };

    for (int c = 0; c < 4; c++) {
        float a = sa[c];
        for (int s = 0; s < SEGS; s++) {
            float a1 = a + step;
            C2D_DrawTriangle(
                cx[c], cy[c], color,
                cx[c] + r * cosf(a),  cy[c] + r * sinf(a),  color,
                cx[c] + r * cosf(a1), cy[c] + r * sinf(a1), color,
                0.5f);
            a = a1;
        }
    }
}

void ui_draw_drop_shadow(float x, float y, float w, float h, float r, u8 base_alpha) {
    /* Multi-layer diffuse shadow: 6 concentric rounded rects fading outward.
     * Each layer adds ~0.7px spread and a small downward offset.
     * Alpha follows an inverse-square falloff for a smooth gradient. */
    const int LAYERS = 6;
    for (int i = LAYERS; i >= 1; i--) {
        float t = (float)i / (float)LAYERS;          /* 1.0 = outermost */
        float spread = (float)i * 0.7f;
        float off_y  = (float)i * 0.4f;
        /* Inverse-square falloff: inner layers get most of the alpha */
        u8 a = (u8)((u32)base_alpha * (1.0f - t * t) * 0.18f);
        if (a == 0) continue;
        ui_draw_rounded_rect(x - spread + 1, y + off_y,
                             w + 2 * spread, h + 2 * spread,
                             r + spread,
                             C2D_Color32(0x00, 0x00, 0x00, a));
    }
}

void ui_draw_rounded_mask(float x, float y, float w, float h, float r, u32 bg) {
    /* Draw background-colored corner masks to fake rounded corners on images.
     * For each corner, a triangle fan from the square corner through the arc
     * fills the area outside the rounded edge. */
    if (r > w * 0.5f) r = w * 0.5f;
    if (r > h * 0.5f) r = h * 0.5f;

    const float PI   = 3.14159265f;
    const float PI_2 = 1.5707963f;
    const int SEGS = 6;
    float step = PI_2 / (float)SEGS;

    /* corner square corners (the actual square vertex to fan from) */
    float sx[4] = { x,         x + w,     x + w,     x         };
    float sy[4] = { y,         y,         y + h,     y + h     };
    /* arc centres */
    float cx[4] = { x + r,     x + w - r, x + w - r, x + r     };
    float cy[4] = { y + r,     y + r,     y + h - r, y + h - r  };
    /* start angles: TL=PI, TR=3PI/2, BR=0, BL=PI/2 */
    float sa[4] = { PI, 3*PI_2, 0.0f, PI_2 };

    for (int c = 0; c < 4; c++) {
        float a = sa[c];
        /* First point on arc */
        float px = cx[c] + r * cosf(a);
        float py = cy[c] + r * sinf(a);
        for (int s = 0; s < SEGS; s++) {
            float a1 = a + step;
            float nx = cx[c] + r * cosf(a1);
            float ny = cy[c] + r * sinf(a1);
            C2D_DrawTriangle(sx[c], sy[c], bg, px, py, bg, nx, ny, bg, 0.5f);
            px = nx;
            py = ny;
            a = a1;
        }
    }
}

void ui_draw_grad_v(float x, float y, float w, float h, u32 top_col, u32 bot_col) {
    C2D_DrawRectangle(x, y, 0.5f, w, h, top_col, top_col, bot_col, bot_col);
}

void ui_draw_header(float width) {
    /* Gradient blue bar */
    ui_draw_grad_v(0, 0, width, UI_HEADER_H, UI_COL_HEADER_TOP, UI_COL_HEADER_BOT);
    /* Gloss highlight on upper half */
    ui_draw_grad_v(0, 0, width, UI_HEADER_H / 2.0f, UI_COL_GLOSS, UI_COL_GLOSS_NONE);
    /* 3px drop shadow below */
    ui_draw_grad_v(0, UI_HEADER_H, width, 3, UI_COL_SHADOW, UI_COL_SHADOW_NONE);
}

void ui_draw_status_bar(float width) {
    /* 3px upward shadow above the bar */
    ui_draw_grad_v(0, 220 - 3, width, 3, UI_COL_SHADOW_NONE, UI_COL_SHADOW);
    /* Gradient gray bar */
    ui_draw_grad_v(0, 220, width, UI_STATUS_H, UI_COL_STATUS_TOP, UI_COL_STATUS_BOT);
}

float ui_text_width(const char *str, float scale) {
    C2D_Text t;
    C2D_TextParse(&t, s_textbuf, str);
    C2D_TextOptimize(&t);
    float w = 0.0f, h = 0.0f;
    C2D_TextGetDimensions(&t, scale, scale, &w, &h);
    return w;
}

void ui_draw_text_trunc(float x, float y, float scale, u32 color,
                        const char *str, float max_w) {
    if (ui_text_width(str, scale) <= max_w) {
        ui_draw_text(x, y, scale, color, str);
        return;
    }

    char buf[128];
    size_t slen = strlen(str);
    if (slen >= sizeof(buf) - 3) slen = sizeof(buf) - 4;

    /* Binary search for the longest prefix that fits with "..." */
    size_t lo = 0, hi = slen, best = 0;
    while (lo <= hi) {
        size_t mid = (lo + hi) / 2;
        memcpy(buf, str, mid);
        buf[mid] = '.'; buf[mid+1] = '.'; buf[mid+2] = '.'; buf[mid+3] = '\0';
        if (ui_text_width(buf, scale) <= max_w) {
            best = mid;
            if (mid == hi) break;
            lo = mid + 1;
        } else {
            if (mid == 0) break;
            hi = mid - 1;
        }
    }

    memcpy(buf, str, best);
    buf[best] = '.'; buf[best+1] = '.'; buf[best+2] = '.'; buf[best+3] = '\0';
    ui_draw_text(x, y, scale, color, buf);
}
