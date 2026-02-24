#pragma once
#include <3ds.h>
#include <citro2d.h>

/* Colour palette (ABGR u32 for C2D_Color32) */
#define UI_COL_BG         C2D_Color32(0xF7, 0xF7, 0xF2, 0xFF)  /* cream white   */
#define UI_COL_HEADER     C2D_Color32(0x4A, 0x86, 0xC8, 0xFF)  /* Activity blue */
#define UI_COL_HEADER_TXT C2D_Color32(0xFF, 0xFF, 0xFF, 0xFF)
#define UI_COL_ROW_ALT    C2D_Color32(0xF0, 0xF0, 0xEB, 0xFF)
#define UI_COL_ROW_SEL    C2D_Color32(0xD0, 0xE8, 0xFD, 0xFF)
#define UI_COL_TEXT       C2D_Color32(0x1A, 0x1A, 0x1A, 0xFF)
#define UI_COL_TEXT_DIM   C2D_Color32(0x88, 0x88, 0x88, 0xFF)
#define UI_COL_DIVIDER    C2D_Color32(0xDD, 0xDD, 0xDD, 0xFF)
#define UI_COL_STATUS_BG  C2D_Color32(0xEE, 0xEE, 0xEE, 0xFF)
#define UI_COL_STATUS_TXT C2D_Color32(0x44, 0x44, 0x44, 0xFF)

/* Screen dimensions */
#define UI_TOP_W  400
#define UI_TOP_H  240
#define UI_BOT_W  320
#define UI_BOT_H  240

/* Game list layout */
#define UI_HEADER_H   24   /* top header bar height                  */
#define UI_STATUS_H   20   /* bottom status bar height                */
#define UI_ROW_H      48   /* pixels per game list row                */
#define UI_LIST_Y     UI_HEADER_H
#define UI_LIST_BOT   (UI_TOP_H - UI_STATUS_H)
#define UI_VISIBLE_ROWS ((UI_LIST_BOT - UI_LIST_Y) / UI_ROW_H)   /* = 6 */

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
void ui_draw_triangle(float x0, float y0, float x1, float y1,
                      float x2, float y2, u32 color);
