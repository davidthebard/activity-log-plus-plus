#include "ui.h"
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
    s_textbuf = C2D_TextBufNew(4096);
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
    size_t len = strlen(str);
    if (len >= sizeof(buf) - 3) len = sizeof(buf) - 4;

    while (len > 0) {
        len--;
        memcpy(buf, str, len);
        buf[len] = '.'; buf[len+1] = '.'; buf[len+2] = '.'; buf[len+3] = '\0';
        if (ui_text_width(buf, scale) <= max_w)
            break;
    }

    if (len == 0) {
        buf[0] = '.'; buf[1] = '.'; buf[2] = '.'; buf[3] = '\0';
    }

    ui_draw_text(x, y, scale, color, buf);
}
