/**
 * @file ui_renderer.h
 * @brief Premium GU-based UI renderer for PSP Moonlight
 */

#ifndef UI_RENDERER_H
#define UI_RENDERER_H

#include <pspgu.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize the UI system */
int ui_renderer_init(void);

/* Shutdown the UI system */
void ui_renderer_shutdown(void);

/* Begin a UI frame (clears screen, starts GU) */
void ui_renderer_begin_frame(void);

/* End a UI frame (finishes GU, swaps buffers) */
void ui_renderer_end_frame(void);

/* High-level Drawing Functions */
void ui_draw_background(void);
void ui_draw_header(const char* title);
void ui_draw_panel(int x, int y, int w, int h, unsigned int color, int border);
void ui_draw_menu_item(const char* label, int x, int y, int width, int selected);
void ui_draw_text(const char* text, int x, int y, unsigned int color);
void ui_draw_status_bar(const char* text);
void ui_draw_wifi_selector(const char* profiles[], int count, int selected);
void ui_draw_wifi_status(const char* ssid, const char* state_str);

/* Helper for colors: ABGR format */
#define UI_COLOR_WHITE      0xFFFFFFFF
#define UI_COLOR_BLACK      0xFF000000
#define UI_COLOR_BLUE       0xFFFFBB00
#define UI_COLOR_PALE_BLUE  0xFFFFEECC
#define UI_COLOR_CYAN       0xFFFFFF00
#define UI_COLOR_DARK_GREY  0xFF333333
#define UI_COLOR_TRANS_BLACK 0x80000000
#define UI_COLOR_GLOW_BLUE  0xFF00AAFF

#ifdef __cplusplus
}
#endif

#endif /* UI_RENDERER_H */
