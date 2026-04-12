/*
 * hud.c - Heads-Up Display overlay for PSP Moonlight streaming
 *
 * Renders an alpha-blended semi-transparent overlay showing streaming stats.
 * Triggered by Home or Note button press.
 * Includes a quit option selectable with the Cross button.
 */

#include <pspkernel.h>
#include <pspctrl.h>
#include <pspgu.h>
#include <pspdisplay.h>
#include <stdio.h>
#include <string.h>

#include "shared.h"
#include "hud.h"
#include "stream_session.h"
#include "safety_buffer.h"
#include "ui_manager.h"

/*============================================================================
 * Constants
 *============================================================================*/

/* GU command list buffer for HUD — 16 KB (sufficient for overlay) */
static u32 __attribute__((aligned(16))) hud_display_list[16 * 1024 / 4];

/* HUD overlay colors — derived from the active theme where possible.
 * The semi-transparent BG and border stay fixed so the overlay is
 * always legible on top of video, but text and highlight use the theme. */
#define HUD_BG_COLOR        0x80000000  /* Semi-transparent black */
#define HUD_TEXT_COLOR      UI_COL_TEXT
#define HUD_HIGHLIGHT_COLOR UI_COL_ACCENT
#define HUD_BORDER_COLOR    UI_COL_BORDER

/* HUD dimensions and positioning */
#define HUD_WIDTH           200
#define HUD_HEIGHT          100
#define HUD_X               (FRAME_WIDTH - HUD_WIDTH - 10)  /* Top-right */
#define HUD_Y               10
#define HUD_PADDING         8
#define HUD_LINE_HEIGHT     16

/* Menu item indices */
#define MENU_ITEM_STATS     0
#define MENU_ITEM_PAUSE     1
#define MENU_ITEM_QUIT      2
#define MENU_ITEM_COUNT     3

/*============================================================================
 * Vertex structures
 *============================================================================*/

/* Vertex for colored rectangles (no texture) */
typedef struct {
    u32 color;
    float x, y, z;
} ColorVertex;

/* Vertex for 2D text rendering (using GU font) */
typedef struct {
    u16 u, v;
    u32 color;
    float x, y, z;
} TextVertex;

/*============================================================================
 * Internal State
 *============================================================================*/

static HudStats g_stats = { 0, 0.0f };     /* Current statistics (latency, fps) */
static int g_hud_visible = 0;               /* HUD visibility flag */
static int g_selected_item = 0;             /* Currently selected menu item */
static int g_initialized = 0;               /* Initialization flag */

/* Forward declarations for icon drawing functions (defined below) */
static void draw_wifi_icon(int x, int y);
static void draw_rewind_icon(int x, int y);
int hud_should_show_rewind_icon(void);
void hud_show_rewind_icon(void);
void hud_hide_rewind_icon(void);

/* Button debounce state */
static u32 g_prev_buttons = 0;

/* Wi-Fi icon state */
static int g_wifi_icon_visible = 0;
static u32 g_wifi_icon_show_time = 0;
#define WIFI_ICON_DURATION_US  (2 * 1000 * 1000)  /* 2 seconds */

/* Rewind icon state */
static int g_rewind_icon_visible = 0;
static u32 g_rewind_icon_show_time = 0;
#define REWIND_ICON_DURATION_US  (2 * 1000 * 1000)  /* 2 seconds */

/* draw_text - thin wrapper around UIManager's intraFont text rendering.
 * Kept as a static helper so local callers don't need changing. */
static void draw_text(int x, int y, u32 color, const char *text)
{
    /* Delegate to UIManager — must be called inside a sceGuStart frame */
    ui_draw_text((float)x, (float)(y + 12), color, text);
}

/*============================================================================
 * Helper: Check if button was just pressed (debounced)
 *============================================================================*/
static int button_pressed(u32 buttons, u32 mask)
{
    return ((buttons & mask) && !(g_prev_buttons & mask)) ? 1 : 0;
}

/*============================================================================
 * Public API Implementation
 *============================================================================*/

void hud_init(void)
{
    g_hud_visible = 0;
    g_selected_item = 0;
    g_initialized = 1;
    g_prev_buttons = 0;

    memset(&g_stats, 0, sizeof(g_stats));
}

void hud_update_stats(const HudStats *stats)
{
    if (stats) {
        g_stats = *stats;
    }
}

void hud_render(void)
{
    char buf[64];
    int y_offset;
    int i;

    if (!g_initialized || !g_hud_visible)
        return;

    /* Begin HUD rendering */
    sceGuStart(GU_DIRECT, hud_display_list);

    /* --- WHAT THIS DOES ---
     * Draw the HUD panel using UIManager helpers inside the existing
     * GU frame started by the caller (display_frame context).
     * Single rounded rect for polished look without excessive GPU cost.
     * Alpha blending is enabled for the semi-transparent panel.
     */
    ui_set_blend(1);
    /* Single shadow offset (cheap simple rect) */
    ui_draw_rect(HUD_X + 2, HUD_Y + 2, HUD_WIDTH, HUD_HEIGHT, 0x30000000);
    /* Panel background — semi-transparent dark with rounded corners */
    ui_draw_rect_rounded(HUD_X, HUD_Y, HUD_WIDTH, HUD_HEIGHT, 6, HUD_BG_COLOR);
    ui_set_blend(0);

    /* Draw title */
    y_offset = HUD_Y + HUD_PADDING;

    /* Stats using UIManager intraFont text */
    snprintf(buf, sizeof(buf), "Latency: %dms", g_stats.latency_ms);
    draw_text(HUD_X + HUD_PADDING, y_offset, HUD_TEXT_COLOR, buf);
    y_offset += HUD_LINE_HEIGHT + 4;

    snprintf(buf, sizeof(buf), "FPS: %.1f", g_stats.fps);
    draw_text(HUD_X + HUD_PADDING, y_offset, HUD_TEXT_COLOR, buf);
    y_offset += HUD_LINE_HEIGHT + 8;

    /* Draw menu items using UIManager text */
    for (i = 0; i < MENU_ITEM_COUNT; i++) {
        u32 color = (i == g_selected_item) ? HUD_HIGHLIGHT_COLOR : HUD_TEXT_COLOR;
        const char *label;
        if (i == MENU_ITEM_QUIT) label = "Quit";
        else if (i == MENU_ITEM_PAUSE) label = "Pause";
        else label = "Stats";
        /* Highlight the selected row */
        if (i == g_selected_item) {
            /* Accent-colored rounded pill highlight (matches settings card style) */
            u32 sel_glow = (g_ui_accent_color & 0x00FFFFFFu) | 0x60000000u;
            ui_set_blend(1);
            ui_draw_rect_rounded(HUD_X + HUD_PADDING, y_offset - 2,
                         HUD_WIDTH - HUD_PADDING*2, HUD_LINE_HEIGHT + 4,
                         4, sel_glow);
            ui_set_blend(0);
        }
        draw_text(HUD_X + HUD_PADDING + 8, y_offset, color, label);
        y_offset += HUD_LINE_HEIGHT;
    }

    /* Draw Wi-Fi warning icon if active (top-left corner) */
    if (hud_should_show_wifi_icon())
    {
        draw_wifi_icon(10, 10);
    }

    /* Draw Rewind/Pause icon if active (center of screen) */
    if (hud_should_show_rewind_icon())
    {
        draw_rewind_icon(FRAME_WIDTH / 2 - 24, FRAME_HEIGHT / 2 - 24);
    }

    /* Finish HUD rendering */
    sceGuFinish();
    sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE);
}

int hud_handle_input(u32 buttons)
{
    int quit_selected = 0;

    if (!g_initialized)
        return 0;

    /* Toggle HUD on Note only. Reserve Home for the PSP system exit path. */
    if (button_pressed(buttons, PSP_CTRL_NOTE)) {
        g_hud_visible = !g_hud_visible;
        if (g_hud_visible) {
            g_selected_item = 0;  /* Reset selection when opening */
        }
    }

    /* Handle navigation when HUD is visible */
    if (g_hud_visible) {
        /* Navigate menu with Up/Down */
        if (button_pressed(buttons, PSP_CTRL_UP)) {
            g_selected_item--;
            if (g_selected_item < 0)
                g_selected_item = MENU_ITEM_COUNT - 1;
        }

        if (button_pressed(buttons, PSP_CTRL_DOWN)) {
            g_selected_item++;
            if (g_selected_item >= MENU_ITEM_COUNT)
                g_selected_item = 0;
        }

        /* Select with Cross button */
        if (button_pressed(buttons, PSP_CTRL_CROSS)) {
            if (g_selected_item == MENU_ITEM_QUIT) {
                quit_selected = 1;
                g_hud_visible = 0;  /* Hide HUD */
                abort_stream_to_menu();  /* Return to menu instead of exiting */
            } else if (g_selected_item == MENU_ITEM_PAUSE) {
                quit_selected = 2;  /* 2 = pause (return to menu, keep host session) */
                g_hud_visible = 0;
            }
        }

        /* Close HUD with Circle button */
        if (button_pressed(buttons, PSP_CTRL_CIRCLE)) {
            g_hud_visible = 0;
        }
    }

    /* Save button state for debouncing */
    g_prev_buttons = buttons;

    return quit_selected;
}

int hud_is_visible(void)
{
    return g_hud_visible;
}

void hud_shutdown(void)
{
    g_initialized = 0;
    g_hud_visible = 0;
}

/*============================================================================
 * Wi-Fi Icon Functions
 *============================================================================*/

/*
 * draw_wifi_icon - Draw a simple Wi-Fi signal icon
 *
 * Renders a stylized Wi-Fi icon with 3 curved arcs representing signal bars.
 * The icon is drawn in yellow (0xFFFFFF00) when signal is weak.
 */
static void draw_wifi_icon(int x, int y)
{
    int ok = 1;
    /* Accent color for Wi-Fi warning icon — follows the active theme */
    u32 wifi_color = UI_COL_ACCENT;

    /* Enable blending */
    sceGuEnable(GU_BLEND);
    sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
    sceGuDisable(GU_TEXTURE_2D);

    /* Draw Wi-Fi icon as simplified signal bars */
    /* Bar 1 (innermost - smallest arc) */
    {
        ColorVertex *verts = (ColorVertex *)sceGuGetMemory(2 * sizeof(ColorVertex));
        if (!verts) { ok = 0; goto done; }

        verts[0].color = wifi_color;
        verts[0].x = (float)(x + 8);
        verts[0].y = (float)(y + 12);
        verts[0].z = 0.0f;

        verts[1].color = wifi_color;
        verts[1].x = (float)(x + 16);
        verts[1].y = (float)(y + 16);
        verts[1].z = 0.0f;

        sceGuDrawArray(GU_SPRITES,
                       GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D,
                       2, NULL, verts);
    }

    /* Bar 2 (middle arc) */
    {
        ColorVertex *verts = (ColorVertex *)sceGuGetMemory(2 * sizeof(ColorVertex));
        if (!verts) { ok = 0; goto done; }

        verts[0].color = wifi_color;
        verts[0].x = (float)(x + 5);
        verts[0].y = (float)(y + 8);
        verts[0].z = 0.0f;

        verts[1].color = wifi_color;
        verts[1].x = (float)(x + 19);
        verts[1].y = (float)(y + 16);
        verts[1].z = 0.0f;

        sceGuDrawArray(GU_SPRITES,
                       GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D,
                       2, NULL, verts);
    }

    /* Bar 3 (outermost - largest arc) */
    {
        ColorVertex *verts = (ColorVertex *)sceGuGetMemory(2 * sizeof(ColorVertex));
        if (!verts) { ok = 0; goto done; }

        verts[0].color = wifi_color;
        verts[0].x = (float)(x + 2);
        verts[0].y = (float)(y + 4);
        verts[0].z = 0.0f;

        verts[1].color = wifi_color;
        verts[1].x = (float)(x + 22);
        verts[1].y = (float)(y + 16);
        verts[1].z = 0.0f;

        sceGuDrawArray(GU_SPRITES,
                       GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D,
                       2, NULL, verts);
    }

    /* Draw dot at bottom (device indicator) */
    {
        ColorVertex *verts = (ColorVertex *)sceGuGetMemory(2 * sizeof(ColorVertex));
        if (!verts) { ok = 0; goto done; }

        verts[0].color = wifi_color;
        verts[0].x = (float)(x + 11);
        verts[0].y = (float)(y + 17);
        verts[0].z = 0.0f;

        verts[1].color = wifi_color;
        verts[1].x = (float)(x + 13);
        verts[1].y = (float)(y + 19);
        verts[1].z = 0.0f;

        sceGuDrawArray(GU_SPRITES,
                       GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D,
                       2, NULL, verts);
    }

done:
    if (!ok) {
        /* Intentionally no logging in HUD hot path. */
    }
    sceGuDisable(GU_BLEND);
    sceGuEnable(GU_TEXTURE_2D);
}

void hud_show_wifi_icon(void)
{
    g_wifi_icon_visible = 1;
    g_wifi_icon_show_time = sceKernelGetSystemTimeLow();
}

int hud_should_show_wifi_icon(void)
{
    if (!g_wifi_icon_visible)
        return 0;

    /* Check if icon duration has expired */
    u32 current_time = sceKernelGetSystemTimeLow();
    if ((current_time - g_wifi_icon_show_time) >= WIFI_ICON_DURATION_US)
    {
        g_wifi_icon_visible = 0;
        return 0;
    }

    return 1;
}

/*============================================================================
 * Rewind/Pause Icon Functions
 *============================================================================*/

/*
 * draw_rewind_icon - Draw a rewind/pause icon for buffering indication
 *
 * Renders a stylized rewind symbol with two triangles pointing left,
 * surrounded by a pause symbol (two vertical bars).
 * The icon is drawn in orange (0xFFFF8000) when buffering.
 */
static void draw_rewind_icon(int x, int y)
{
    int ok = 1;
    /* Accent color for rewind/buffering icon — follows the active theme */
    u32 rewind_color = UI_COL_ACCENT;

    /* Enable blending */
    sceGuEnable(GU_BLEND);
    sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
    sceGuDisable(GU_TEXTURE_2D);

    /* Draw pause symbol (two vertical bars) on the sides */
    /* Left bar */
    {
        ColorVertex *verts = (ColorVertex *)sceGuGetMemory(2 * sizeof(ColorVertex));
        if (!verts) { ok = 0; goto done; }

        verts[0].color = rewind_color;
        verts[0].x = (float)x;
        verts[0].y = (float)(y + 8);
        verts[0].z = 0.0f;

        verts[1].color = rewind_color;
        verts[1].x = (float)(x + 6);
        verts[1].y = (float)(y + 40);
        verts[1].z = 0.0f;

        sceGuDrawArray(GU_SPRITES,
                       GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D,
                       2, NULL, verts);
    }

    /* Right bar */
    {
        ColorVertex *verts = (ColorVertex *)sceGuGetMemory(2 * sizeof(ColorVertex));
        if (!verts) { ok = 0; goto done; }

        verts[0].color = rewind_color;
        verts[0].x = (float)(x + 42);
        verts[0].y = (float)(y + 8);
        verts[0].z = 0.0f;

        verts[1].color = rewind_color;
        verts[1].x = (float)(x + 48);
        verts[1].y = (float)(y + 40);
        verts[1].z = 0.0f;

        sceGuDrawArray(GU_SPRITES,
                       GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D,
                       2, NULL, verts);
    }

    /* Draw rewind symbol (two left-pointing triangles) in the center */
    /* Left triangle (larger) */
    {
        ColorVertex *verts = (ColorVertex *)sceGuGetMemory(3 * sizeof(ColorVertex));
        if (!verts) { ok = 0; goto done; }

        /* Triangle pointing left */
        verts[0].color = rewind_color;
        verts[0].x = (float)(x + 8);
        verts[0].y = (float)(y + 24);
        verts[0].z = 0.0f;

        verts[1].color = rewind_color;
        verts[1].x = (float)(x + 24);
        verts[1].y = (float)(y + 12);
        verts[1].z = 0.0f;

        verts[2].color = rewind_color;
        verts[2].x = (float)(x + 24);
        verts[2].y = (float)(y + 36);
        verts[2].z = 0.0f;

        sceGuDrawArray(GU_TRIANGLES,
                       GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D,
                       3, NULL, verts);
    }

    /* Right triangle (smaller, overlapping) */
    {
        ColorVertex *verts = (ColorVertex *)sceGuGetMemory(3 * sizeof(ColorVertex));
        if (!verts) { ok = 0; goto done; }

        /* Triangle pointing left */
        verts[0].color = rewind_color;
        verts[0].x = (float)(x + 20);
        verts[0].y = (float)(y + 24);
        verts[0].z = 0.0f;

        verts[1].color = rewind_color;
        verts[1].x = (float)(x + 36);
        verts[1].y = (float)(y + 12);
        verts[1].z = 0.0f;

        verts[2].color = rewind_color;
        verts[2].x = (float)(x + 36);
        verts[2].y = (float)(y + 36);
        verts[2].z = 0.0f;

        sceGuDrawArray(GU_TRIANGLES,
                       GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D,
                       3, NULL, verts);
    }

    /* Draw "BUFFERING" text indicator below icon */
    /* Simple text rendering using small rectangles for each letter */
    const char *text = "BUFFERING";
    int text_x = x - 16;
    int text_y = y + 50;
    int len = strlen(text);

    for (int i = 0; i < len; i++) {
        if (text[i] != ' ') {
            ColorVertex *verts = (ColorVertex *)sceGuGetMemory(2 * sizeof(ColorVertex));
            if (!verts) { ok = 0; break; }

            verts[0].color = rewind_color;
            verts[0].x = (float)(text_x + i * 8);
            verts[0].y = (float)text_y;
            verts[0].z = 0.0f;

            verts[1].color = rewind_color;
            verts[1].x = (float)(text_x + i * 8 + 6);
            verts[1].y = (float)(text_y + 10);
            verts[1].z = 0.0f;

            sceGuDrawArray(GU_SPRITES,
                           GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D,
                           2, NULL, verts);
        }
    }

done:
    if (!ok) {
        /* Intentionally no logging in HUD hot path. */
    }
    sceGuDisable(GU_BLEND);
    sceGuEnable(GU_TEXTURE_2D);
}

/*
 * hud_show_rewind_icon - Show the rewind/pause icon (called by safety_buffer.c)
 */
void hud_show_rewind_icon(void)
{
    g_rewind_icon_visible = 1;
    g_rewind_icon_show_time = sceKernelGetSystemTimeLow();
}

/*
 * hud_hide_rewind_icon - Hide the rewind/pause icon
 */
void hud_hide_rewind_icon(void)
{
    g_rewind_icon_visible = 0;
}

int hud_should_show_rewind_icon(void)
{
    if (!g_rewind_icon_visible)
        return 0;

    /* Check if icon duration has expired */
    u32 current_time = sceKernelGetSystemTimeLow();
    if ((current_time - g_rewind_icon_show_time) >= REWIND_ICON_DURATION_US)
    {
        g_rewind_icon_visible = 0;
        return 0;
    }

    return 1;
}
