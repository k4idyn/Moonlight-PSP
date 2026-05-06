/*
 * game_grid_ui.cpp - Game Library Grid Screen (UIManager rendering)
 *
 * Layout: Single horizontal carousel of tiles (100x150px)
 *         Trailing games overlap the screen edge to signal scrolling.
 *         D-Pad Left/Right scrolls the library.
 *
 * Controls:
 *   D-pad Left/Right : move and scroll between tiles
 *   Cross (X)/Start  : launch selected game
 *   Square ([])      : refresh library
 *   Circle (O)       : back to host list
 */

#include <pspkernel.h>
#include <pspdisplay.h>
#include <pspctrl.h>
#include <pspgu.h>
#include <stdio.h>
#include <string.h>
#include <cstring>
#include <math.h>

extern "C" {
#include "game_list_parser.h"
#include "ui_manager.h"
#include "settings_menu.h"
}

#include <pspiofilemgr.h>
#include <stdarg.h>

#include "diag_log.h"
static void grid_log(const char *fmt, ...)
{
    char buf[256]; va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
    diag_log_write("GRID", "%s", buf);
}

/* =========================================================================
 * Layout constants — 480x272 PSP native
 * ========================================================================= */
#define SCREEN_W      480
#define SCREEN_H      272

/* Portrait poster: 2:3 ratio */
#define ICON_W        100
#define ICON_H        150

#define GRID_PAD      40          /* horizontal gap between tiles */
#define TILE_LABEL_H  14          /* text area below each tile */

#define HEADER_H      32
#define FOOTER_H      26

/* Horizontal scrolling: Start at 20px from left */
#define GRID_START_X  20

/* Vertical centering of the row */
#define AVAILABLE_H   (SCREEN_H - HEADER_H - FOOTER_H)
#define ROW_BLOCK_H   (ICON_H + TILE_LABEL_H + 4)
#define GRID_START_Y  (HEADER_H + (AVAILABLE_H - ROW_BLOCK_H) / 2)

/* MAX_GAMES defined in game_list_parser.h */

/* =========================================================================
 * Palette: one distinct hue per slot index (wraps for > 8 games)
 * Colours are 0xAABBGGRR (PSP ABGR8888 / what ui_manager expects)
 * These provide variety between tiles; the active theme’s accent
 * colour tints the selected tile instead.
 * ========================================================================= */
static const unsigned int k_tile_colours[] = {
    0xFF4A6FA5,   /* steel blue   */
    0xFF6BAE75,   /* sage green   */
    0xFFBF6B6B,   /* muted red    */
    0xFF9B7EC8,   /* soft purple  */
    0xFFC4974F,   /* amber        */
    0xFF4AABA5,   /* teal         */
    0xFFC46B9A,   /* rose         */
    0xFF8BA54A,   /* olive        */
};
#define PALETTE_SIZE  (int)(sizeof(k_tile_colours)/sizeof(k_tile_colours[0]))
#define UI_COL_SHADOW 0x50000000u  /* Uniform tile drop-shadow */

/* =========================================================================
 * Internal tile descriptor
 * ========================================================================= */
typedef struct {
    char  title[64];
    int   app_id;
    void *icon_rgb565;    /* NULL = show placeholder */
    unsigned int bg_colour;
} GameTile;

/* =========================================================================
 * Private State Struct - Encapsulating to prevent corruption
 * ========================================================================= */
typedef struct {
    int      num_tiles;
    int      selected;
    float    scroll_target;  /* integer index we want to see */
    float    scroll_curr;    /* floating point position for smooth slide */
    float    focus_anim;     /* 0.0 -> 1.0 scaling factor */
    char     cached_host[64];
    int      empty_repaired; /* 1 = already forced re-pair for 0-app list */
    uint32_t sentinel; 
} GridState;

/* Target 2026 high-fluidity motion */
static GridState s_state = {0, 0, 0.0f, 0.0f, 0.0f, "", 0, 0xDEADBEEF};
static GameTile  s_tiles[MAX_GAMES];
static int       s_fetch_error = 0; /* 1 = last game_list_fetch failed (network/TLS) */

#define s_num_tiles s_state.num_tiles
#define s_selected  s_state.selected
#define s_cached_host s_state.cached_host

static void check_corruption(const char *loc) {
    if (s_state.sentinel != 0xDEADBEEF) {
        grid_log("[CRITICAL] GridState corruption at %s! sentinel=0x%08X\n", loc, s_state.sentinel);
        s_state.sentinel = 0xDEADBEEF;
    }
}

static void update_animations(void) {
    /* Standard Wii-style Center Selection:
     * The scroll point is exactly the current integer selection index.
     * Frame-delta compensated so animation speed is consistent
     * regardless of actual frame rate (dropped VBlanks, CPU stalls). */
    static u32 s_last_anim_us = 0;
    u32 now_us = sceKernelGetSystemTimeLow();
    float dt = 1.0f; /* default to 1.0 (60Hz assumption) on first frame */
    if (s_last_anim_us != 0) {
        u32 elapsed = now_us - s_last_anim_us;
        dt = (float)elapsed / 16667.0f; /* ratio to 60Hz frame interval */
        if (dt > 4.0f) dt = 4.0f;      /* clamp to prevent huge jumps */
        if (dt < 0.1f) dt = 0.1f;      /* clamp lower bound */
    }
    s_last_anim_us = now_us;

    float target_scroll = (float)s_selected;

    s_state.scroll_curr += (target_scroll - s_state.scroll_curr) * 0.15f * dt;
    s_state.focus_anim  += ((float)s_selected - s_state.focus_anim) * 0.20f * dt;
}

static int tile_screen_pos(int idx, int *out_x, int *out_y)
{
    float scroll = s_state.scroll_curr;
    /* Center the current scroll 'focus' point at (SCREEN_W / 2) */
    float center_x = (float)SCREEN_W / 2.0f;
    float tile_center_offset = (float)ICON_W / 2.0f;
    
    *out_x = (int)(center_x - tile_center_offset + ((float)idx - scroll) * (float)(ICON_W + GRID_PAD));
    *out_y = GRID_START_Y;
    
    if (*out_x + ICON_W < -40) return 0;
    if (*out_x > SCREEN_W + 40) return 0;
    return 1;
}

/* =========================================================================
 * Render one full frame
 * ========================================================================= */
static void render_grid(void)
{
    update_animations();

    ui_begin_frame();
    ui_draw_gradient_bg(g_ui_bg_color, g_ui_bg_color);
    ui_draw_header("Game Library");

    if (s_num_tiles == 0) {
        ui_draw_text_centered(0.0f, (float)SCREEN_W, (float)(SCREEN_H / 2),
                              UI_COL_TEXT_DIM, "No games found on server.");
        ui_draw_text_centered(0.0f, (float)SCREEN_W, (float)(SCREEN_H / 2 + 18),
                              UI_COL_TEXT_DIM, "Press [] to refresh or O to go back.");
        ui_draw_footer_hint("{SQ}: Refresh  {O}: Back");
        ui_end_frame();
        return;
    }

    /* Modern 2026 Carousel Rendering: Wii-Rounded + Decoupled Scaling */
    for (int i = 0; i < s_num_tiles; i++) {
        int tx, ty;
        if (!tile_screen_pos(i, &tx, &ty)) continue;

        float dist = fabsf((float)i - s_state.focus_anim);
        float scale = (dist < 1.0f) ? (1.0f + (1.0f - dist) * 0.15f) : 1.0f;

        int tw = (int)((float)ICON_W * scale);
        int th = (int)((float)ICON_H * scale);
        int ox = tx - (tw - ICON_W) / 2;
        int oy = ty - (th - ICON_H) / 2;
        int focused = (i == s_selected);

        /* Carousel Layer Order: Shadow -> Solid Card Base -> Flat Image -> Thick Overlay Bezel */
        
        int pad = 6; /* 6px padding: Card is physically larger than the image */
        int card_x = ox - pad;
        int card_y = oy - pad;
        int card_w = tw + 2 * pad;
        int card_h = th + 2 * pad;

        /* 1. 3-layer drop shadow — grows +1px when focused for hover effect */
        {
            int so = focused ? 1 : 0; /* shadow offset boost */
            ui_set_blend(1);
            ui_draw_rect_rounded(card_x + 3 + so, card_y + 3 + so, card_w, card_h, 14, 0x18000000u);
            ui_draw_rect_rounded(card_x + 2 + so, card_y + 2 + so, card_w, card_h, 14, 0x28000000u);
            ui_draw_rect_rounded(card_x + 1 + so, card_y + 1 + so, card_w, card_h, 14, 0x38000000u);
            ui_set_blend(0);
        }

        /* 2. Solid Card Base (Thick background, visible through any alpha channels) */
        u32 card_col = focused ? g_ui_sel_color : g_ui_card_color;
        ui_draw_rect_rounded(card_x, card_y, card_w, card_h, 12, card_col);

        /* 3. The Icon PNG - Drawn simply in the center */
        if (s_tiles[i].icon_rgb565) {
            ui_draw_texture_rgb565(ox, oy, tw, th,
                                   s_tiles[i].icon_rgb565, 100, 150, 128, 256, focused);
        } else {
            ui_draw_texture_placeholder(ox, oy, tw, th, s_tiles[i].title, focused);
        }

        /* 4. The Bezel Overlay - Exactly overlays the image's square edges to perfectly mask them */
        /* It matches the card dimensions (card_x, card_y, card_w, card_h), with radius 12 and thickness exactly equal to 'pad' so it stops right at the image edge! */
        ui_draw_hollow_rect_rounded(card_x, card_y, card_w, card_h, 12, pad, card_col);

        if (focused) {
           ui_draw_text_centered((float)card_x, (float)card_w, (float)(card_y + card_h + 16), g_ui_text_foc_color, s_tiles[i].title);
        }
    }

    if (s_num_tiles > 1) {
        char label[32];
        snprintf(label, sizeof(label), "%d / %d", s_selected + 1, s_num_tiles);
        ui_draw_text_right((float)(SCREEN_W - 18), 22.0f, UI_COL_TEXT_DIM, label); /* Right margin, centered in pill */
    }

    ui_draw_footer_hint("{LF}/{RF}: Navigate  {X}: Start  {SQ}: Refresh  {O}: Back");
    ui_end_frame();
}

/* =========================================================================
 * Load game list and statically download/cache icons
 * ========================================================================= */
static void load_games_from_server(const char *host_ip)
{
    static GameList s_game_list;

    /* Show loading spinner */
    ui_begin_frame();
    ui_draw_gradient_bg(g_ui_bg_color, g_ui_bg_color);
    ui_draw_header("Game Library");
    ui_draw_spinner(-1, SCREEN_H / 2 - 20, "Parsing games list...");
    ui_end_frame();

    game_list_init(&s_game_list, host_ip);
    int ret = game_list_fetch(&s_game_list);
    if (ret < 0) {
        char msg[80];
        snprintf(msg, sizeof(msg), "game_list_fetch error: %d", ret);
        ui_begin_frame();
        ui_draw_gradient_bg(g_ui_bg_color, g_ui_bg_color);
        ui_draw_error_modal("Failed to Load Games", msg, "Check host IP and Sunshine.   {O}: Back");
        ui_end_frame();
        sceKernelDelayThread(3 * 1000 * 1000);
        s_num_tiles = 0;
        s_fetch_error = 1;
        return;
    }
    s_fetch_error = 0;

    /* Start background icon downloads (or load from cache) */
    game_list_download_icons(&s_game_list);

    int count = s_game_list.count;
    if (count > MAX_GAMES) count = MAX_GAMES;

    for (int i = 0; i < count; i++) {
        GameInfo *info = game_list_get_game_by_index(&s_game_list, i);
        if (!info) continue;
        strncpy(s_tiles[i].title, info->title, 63);
        s_tiles[i].title[63]   = '\0';
        s_tiles[i].app_id      = info->id;
        s_tiles[i].bg_colour   = k_tile_colours[i % PALETTE_SIZE];
        s_tiles[i].icon_rgb565 = (void *)info->iconData;
    }
    s_num_tiles = count;
    grid_log("load_games_from_server: populated %d tiles (count=%d)\n", s_num_tiles, count);
    check_corruption("load_games_from_server_finish");
}

/* =========================================================================
 * Public API
 * ========================================================================= */

extern "C" int game_grid_ui_run(const char *host_ip)
{

    grid_log("game_grid_ui_run: entered (host=%s cached=%s num_tiles=%d)\n", 
             host_ip ? host_ip : "NULL", s_cached_host, s_num_tiles);
    check_corruption("game_grid_ui_run_entry");

    s_selected = 0;
    s_state.scroll_curr = 0.0f;
    s_state.focus_anim = 0.0f;

    /* Skip re-fetch when returning from RTSP failure for the same host */
    if (s_num_tiles > 0 && host_ip &&
        strncmp(s_cached_host, host_ip, sizeof(s_cached_host)) == 0) {
        /* Reuse cached tile list — just re-render */
        grid_log("game_grid_ui_run: reusing %d cached tiles\n", s_num_tiles);
    } else {
        grid_log("game_grid_ui_run: host changed or no tiles, re-fetching...\n");
        s_num_tiles = 0;
        /* Reset stale-pair guard on host change */
        if (host_ip && strncmp(s_cached_host, host_ip, sizeof(s_cached_host)) != 0) {
            s_state.empty_repaired = 0;
        }
        if (host_ip) {
            strncpy(s_cached_host, host_ip, sizeof(s_cached_host) - 1);
            s_cached_host[sizeof(s_cached_host) - 1] = '\0';
        }
        load_games_from_server(host_ip);

        /* If server returned a VALID response with 0 apps (not a network
         * error) and we haven't already forced re-pair for this host,
         * signal the caller to clear stale pairing.  Network errors set
         * s_fetch_error = 1 and should NOT trigger re-pair — the user
         * already saw the error modal and can press O to go back. */
        if (s_num_tiles == 0 && !s_fetch_error && !s_state.empty_repaired) {
            s_state.empty_repaired = 1;
            grid_log("game_grid_ui_run: 0 apps from valid response — probable stale pairing, returning -3\n");
            return -3;
        }
    }

    check_corruption("game_grid_ui_run_loop_start");

    /* Auto-launch DISABLED — manual navigation only via RemoteJoy */

    while (1) {
        UIEvent evt = ui_process_input();

        if (evt != UI_EVT_NONE) {
            grid_log("game_grid_ui_run: event=%d num_tiles=%d selected=%d\n", 
                     (int)evt, s_num_tiles, s_selected);
        }

        switch (evt) {
        case UI_EVT_LEFT:
            if (s_selected > 0) s_selected--;
            break;

        case UI_EVT_RIGHT:
            if (s_selected < s_num_tiles - 1) s_selected++;
            break;

        /* ---- Launch ---- */
        case UI_EVT_SELECT:
        case UI_EVT_START:
            if (s_num_tiles > 0 && s_selected < s_num_tiles)
                return s_tiles[s_selected].app_id;
            break;

        /* ---- Back ---- */
        case UI_EVT_BACK:
            return -1;

        /* ---- Refresh Library (Square button) ---- */
        case UI_EVT_SCAN:
            grid_log("game_grid_ui_run: Refresh library (Square) triggered\n");
            s_num_tiles  = 0;
            s_selected   = 0;
            s_state.scroll_curr = 0.0f;
            memset(s_tiles, 0, sizeof(s_tiles));
            load_games_from_server(host_ip);
            break;

        default:
            break;
        }



        render_grid();
    }

    return -1;
}

extern "C" const char *game_grid_ui_get_selected_title(void)
{
    if (s_selected >= 0 && s_selected < s_num_tiles)
        return s_tiles[s_selected].title;
    return "Game";
}
