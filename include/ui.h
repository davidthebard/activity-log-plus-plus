#pragma once
#include <3ds.h>
#include <citro2d.h>

/* Colour palette (ABGR u32 for C2D_Color32) */
#define UI_COL_BG         C2D_Color32(0xED, 0xED, 0xE8, 0xFF)  /* light gray bg */
#define UI_COL_LIST_BG    C2D_Color32(0xD8, 0xD8, 0xD2, 0xFF)  /* darker list bg*/
#define UI_COL_HEADER     C2D_Color32(0x4A, 0x86, 0xC8, 0xFF)  /* Activity blue */
#define UI_COL_HEADER_TXT C2D_Color32(0xFF, 0xFF, 0xFF, 0xFF)
#define UI_COL_ROW_ALT    C2D_Color32(0xF0, 0xF0, 0xEB, 0xFF)
#define UI_COL_ROW_SEL    C2D_Color32(0xD0, 0xE8, 0xFD, 0xFF)
#define UI_COL_TEXT       C2D_Color32(0x1A, 0x1A, 0x1A, 0xFF)
#define UI_COL_TEXT_DIM   C2D_Color32(0x88, 0x88, 0x88, 0xFF)
#define UI_COL_DIVIDER    C2D_Color32(0xDD, 0xDD, 0xDD, 0xFF)
#define UI_COL_STATUS_BG  C2D_Color32(0xEE, 0xEE, 0xEE, 0xFF)
#define UI_COL_STATUS_TXT C2D_Color32(0x44, 0x44, 0x44, 0xFF)

/* Header gradient — 3DS-style blue */
#define UI_COL_HEADER_TOP   C2D_Color32(0x6A, 0xA6, 0xE0, 0xFF)  /* lighter blue  */
#define UI_COL_HEADER_BOT   C2D_Color32(0x3A, 0x76, 0xB8, 0xFF)  /* darker blue   */

/* Gloss highlight — semi-transparent white fading to transparent */
#define UI_COL_GLOSS        C2D_Color32(0xFF, 0xFF, 0xFF, 0x20)  /* ~12% white    */
#define UI_COL_GLOSS_NONE   C2D_Color32(0xFF, 0xFF, 0xFF, 0x00)  /* transparent   */

/* Status bar gradient */
#define UI_COL_STATUS_TOP   C2D_Color32(0xF4, 0xF4, 0xF0, 0xFF)
#define UI_COL_STATUS_BOT   C2D_Color32(0xE0, 0xE0, 0xDC, 0xFF)

/* Drop shadow — semi-transparent black fading to transparent */
#define UI_COL_SHADOW       C2D_Color32(0x00, 0x00, 0x00, 0x28)  /* ~16% opacity  */
#define UI_COL_SHADOW_NONE  C2D_Color32(0x00, 0x00, 0x00, 0x00)  /* transparent   */

/* Selection accent border */
#define UI_COL_SEL_BORDER   C2D_Color32(0x80, 0xB8, 0xE8, 0xFF)  /* soft blue     */

/* Card colours */
#define UI_COL_CARD         C2D_Color32(0xFA, 0xFA, 0xF6, 0xFF)  /* cream-white   */
#define UI_COL_CARD_SHADOW  C2D_Color32(0x00, 0x00, 0x00, 0x18)  /* subtle shadow */

/* Screen dimensions */
#define UI_TOP_W  400
#define UI_TOP_H  240
#define UI_BOT_W  320
#define UI_BOT_H  240

/* Game list layout */
#define UI_HEADER_H   24   /* top header bar height                  */
#define UI_STATUS_H   20   /* bottom status bar height                */
#define UI_ROW_H      48   /* pixels per game list row                */
#define UI_ROW_GAP    12   /* vertical gap between card rows          */
#define UI_ROW_MARGIN  4   /* horizontal inset from screen edge       */
#define UI_ICON_GAP    4   /* gap between icon and card               */
#define UI_ROW_RADIUS  4   /* corner radius for card rounding         */
#define UI_ROW_PITCH  (UI_ROW_H + UI_ROW_GAP)
#define UI_LIST_Y     UI_HEADER_H
#define UI_LIST_BOT   UI_TOP_H
#define UI_VISIBLE_ROWS ((UI_LIST_BOT - UI_LIST_Y) / UI_ROW_PITCH)

/* Text scales */
#define UI_SCALE_LG  0.60f   /* primary row text   */
#define UI_SCALE_SM  0.45f   /* secondary row text */
#define UI_SCALE_HDR 0.55f   /* header / status    */

/* Lifecycle */
void ui_init(void);
void ui_fini(void);

/* Call at the start and end of every rendered frame */
void ui_begin_frame(void);
void ui_end_frame(void);

/* Target selection — call before drawing to a screen */
void ui_target_top(void);
void ui_target_bot(void);

/* Drawing primitives */
void ui_draw_rect(float x, float y, float w, float h, u32 color);
void ui_draw_text(float x, float y, float scale, u32 color, const char *str);
void ui_draw_text_right(float x, float y, float scale, u32 color, const char *str);
void ui_draw_textf(float x, float y, float scale, u32 color,
                   const char *fmt, ...) __attribute__((format(printf, 5, 6)));
void ui_draw_image(C2D_Image img, float x, float y, float size);
void ui_draw_image_alpha(C2D_Image img, float x, float y, float size, u8 alpha);
void ui_draw_triangle(float x0, float y0, float x1, float y1,
                      float x2, float y2, u32 color);
void ui_draw_circle(float cx, float cy, float r, u32 color);
void ui_draw_rounded_rect(float x, float y, float w, float h, float r, u32 color);
void ui_draw_drop_shadow(float x, float y, float w, float h, float r, u8 base_alpha);
void ui_draw_rounded_mask(float x, float y, float w, float h, float r, u32 bg);
void ui_draw_grad_v(float x, float y, float w, float h, u32 top_col, u32 bot_col);
void ui_draw_header(float width);
void ui_draw_status_bar(float width);

/* Text measurement & truncation */
float ui_text_width(const char *str, float scale);
void  ui_draw_text_trunc(float x, float y, float scale, u32 color,
                         const char *str, float max_w);
