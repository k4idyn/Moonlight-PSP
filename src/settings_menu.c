/*
 * settings_menu.c - PSP Settings Menu System (UIManager rendering)
 *
 * All display output goes through ui_manager.h / UIManager GU calls.
 * No pspDebugScreen calls remain in this file.
 *
 * Layout (480x272):
 *   y=0..27   header bar     "Moonlight Settings"
 *   y=36      resolution row (panel + left/right arrows + value)
 *   y=72      fps row
 *   y=108     control mode row
 *   y=144     bitrate progress bar                                 (read-only)
 *   y=252..271 footer hint
 */

#include <pspkernel.h>
#include <pspctrl.h>
#include <pspdisplay.h>
#include <stdio.h>
#include <string.h>

#include "settings_menu.h"
#include "config.h"
#include "ui_manager.h"
#include "osk_input.h"

extern volatile unsigned int g_remote_buttons;

/*--------------------------------------------------------------------------\n * Option array definitions (declared extern in settings_menu.h)
 *
 * MATHEMATICAL DERIVATION (706 samples, 14 hardware runs):
 *   CPU decode:   385 µs per 1000 pixels (H.264 baseline, FFmpeg 4.4.5 -O2)
 *   ME yuv2rgba:   31 µs (uncached DMA via 0x40000000)
 *   GE upscale:  2000 µs (bilinear blit 480x272)
 *   Overhead:    1000 µs (sceKernel scheduling, IRQ latency, VSync)
 *   Total/frame: 385 * (W*H/1000) + 3031 µs
 *
 *   Quality:   385*76.544 + 3031 =  32,501 µs → 33,333 µs budget (97.5% @30fps)
 *   Balanced:  385*36.864 + 3031 =  17,224 µs → 33,333 µs budget (51.7% @30fps)
 *   Perf:      385*21.504 + 3031 =  11,310 µs → 16,667 µs budget (67.9% @60fps)
 *
 * Bitrate: 0.35 bits-per-pixel × pixels × fps / 1000 = kbps
 *   Quality:   0.35 × 76544 × 30 / 1000 =  804 kbps
 *   Balanced:  0.35 × 36864 × 30 / 1000 =  387 kbps
 *   Perf:      0.35 × 21504 × 60 / 1000 =  452 kbps
 *--------------------------------------------------------------------------*/

/* Resolution presets — PSP-1000 hardware-tuned (mod-2)
 * Native: 480x272 for full-screen clarity; Perf: 256x144 for max fps.
 * Custom: user-defined via OSK (defaults to 480x272). */
static char s_custom_label[16] = "Custom";

int RESOLUTION_WIDTHS[3]  = { 480, 256, 480 };
int RESOLUTION_HEIGHTS[3] = { 272, 144, 272 };
const char * RESOLUTION_LABELS[3] = {
    "480x272",   /* Native — PSP LCD native resolution */
    "256x144",   /* Performance — 30fps, low latency */
    s_custom_label, /* Custom — user enters WxH via OSK */
};

/* Useless-box auto-coupling: when resolution changes, snap to these */
const int RESOLUTION_OPTIMAL_FPS_IDX[3] = { 0, 2, 0 };  /* 15fps, 30fps, 15fps */
const int RESOLUTION_OPTIMAL_BITRATE[3] = { 500, 500, 500 };  /* 500kbps flat */

void resolution_update_custom(int width, int height)
{
    RESOLUTION_WIDTHS[RESOLUTION_CUSTOM_INDEX]  = width;
    RESOLUTION_HEIGHTS[RESOLUTION_CUSTOM_INDEX] = height;
    snprintf(s_custom_label, sizeof(s_custom_label), "%dx%d", width, height);
}

/* FPS values that evenly divide 60Hz (no VSync judder) */
const char * const FPS_OPTIONS[4] = {
    "15 FPS",
    "20 FPS",
    "30 FPS",
    "60 FPS"
};
const int FPS_VALUES[4] = { 15, 20, 30, 60 };

const char * const CONTROL_MODE_OPTIONS[2] = {
    "Xbox",
    "Browser"
};

const char * const THEME_OPTIONS[10] = {
    "Ocean Depths",
    "Sunset Blvd",
    "Forest Canopy",
    "Modern Minimal",
    "Golden Hour",
    "Arctic Frost",
    "Desert Rose",
    "Tech Innovate",
    "Botanical",
    "Midnight Gal."
};

/*--------------------------------------------------------------------------
 * Pixel layout constants — ALL in native 480x272 coordinates
 *--------------------------------------------------------------------------*/
#define ROW_Y_START   40    /* First setting row top pixel          */
#define ROW_HEIGHT    30    /* Height of each setting row card      */
#define ROW_GAP        6    /* Gap between rows                     */
#define ROW_X         12    /* Left edge of setting cards           */
#define ROW_W        456    /* Card width                           */
#define VAL_W        120    /* Width of the value box on right side */

/*--------------------------------------------------------------------------
 * Menu Item Indices
 *--------------------------------------------------------------------------*/
#define MENU_ITEM_RESOLUTION    0
#define MENU_ITEM_FPS           1
#define MENU_ITEM_CONTROL_MODE  2
#define MENU_ITEM_THEME         3
#define MENU_ITEM_BITRATE       4
#define MENU_ITEM_COUNT         5

#define BITRATE_STEP           100
#define BITRATE_MIN            300

/*--------------------------------------------------------------------------
 * Local State
 *--------------------------------------------------------------------------*/
static MenuState g_menu_state;

static int clamp_index(int value, int min_value, int max_value)
{
    if (value < min_value) {
        return min_value;
    }
    if (value > max_value) {
        return max_value;
    }
    return value;
}

/*--------------------------------------------------------------------------
 * Useless-box coupling: auto-snap FPS + bitrate when resolution changes
 *
 * Like a "useless box" — flip one switch and the mechanism immediately
 * flips the others to maintain mathematical optimality.  The user sees
 * FPS and bitrate update instantly in the UI when cycling resolution.
 *
 * Bitrate formula: 0.35 bpp × W × H × FPS / 1000 (kbps)
 *--------------------------------------------------------------------------*/
static void autocouple_from_resolution(void)
{
    int ri = g_menu_state.resolutionIndex;
    g_menu_state.fpsIndex = RESOLUTION_OPTIMAL_FPS_IDX[ri];
    g_menu_state.bitrate  = RESOLUTION_OPTIMAL_BITRATE[ri];
}

/* Recompute bitrate when FPS changes (keep 0.35 bpp target) */
static void autocouple_bitrate_from_fps(void)
{
    /* Flat 500kbps for all presets — proven safe on 802.11b.
     * Higher bitrate scales decode cost ~linearly (measured:
     * 804kbps → 637 µs/Kpx vs 385 µs/Kpx at 500kbps). */
    g_menu_state.bitrate = 500;
}

/*--------------------------------------------------------------------------
 * Helper: draw one setting row as a card + value selector
 *
 * Each row looks like:
 *   [  Label Name              < VALUE >  ]
 * The card background is darker when not focused, brighter when focused.
 *--------------------------------------------------------------------------*/
static void draw_setting_row(int row_idx, const char *label,
                             const char *value, int is_selected)
{
    /* --- WHAT THIS DOES ---
     * Calculate this row's pixel Y position from its index.
     * Then draw a card panel, the label, and the value selector box.
     */
    int ry = ROW_Y_START + row_idx * (ROW_HEIGHT + ROW_GAP);

    UiButton card;
    card.x = ROW_X;
    card.y = ry;
    card.w = ROW_W;
    card.h = ROW_HEIGHT;
    card.focused = is_selected;
    card.label[0] = '\0';   /* we draw label + value manually below */
    ui_draw_button(&card);

    /* Label on the left (padded 8 px from card left edge) */
    ui_draw_text((float)(ROW_X + 10),
                 (float)(ry + ROW_HEIGHT / 2 + 4),
                 is_selected ? UI_COL_TEXT_FOCUS : UI_COL_TEXT,
                 label);

    /* Value selector on the right side of the card */
    int vx = ROW_X + ROW_W - VAL_W - 8;
    if (is_selected) {
        /* Chain draw calls to get correct positioning */
        float vy = (float)(ry + ROW_HEIGHT / 2 + 4);
        float x2 = ui_draw_text((float)vx, vy, UI_COL_ACCENT, "< ");
        x2 = ui_draw_text(x2, vy, UI_COL_TEXT_FOCUS, value);
        ui_draw_text(x2, vy, UI_COL_ACCENT, " >");
    } else {
        ui_draw_text((float)(vx + 16),
                     (float)(ry + ROW_HEIGHT / 2 + 4),
                     UI_COL_TEXT_DIM, value);
    }
}

/*--------------------------------------------------------------------------
 * settings_menu_draw - Render the settings menu via UIManager (GU)
 *--------------------------------------------------------------------------*/
void settings_menu_draw(const PspConfig *config)
{
    /* --- WHAT THIS DOES ---
     * Open a GU frame, paint the gradient background, draw the header
     * bar, then render one card per setting row.  Close the frame and
     * wait for VBlank so the user sees a tear-free render.
     */

    ui_begin_frame();

    /* Gradient background: dark indigo top, slightly warmer bottom */
    ui_draw_gradient_bg(UI_COL_BG_TOP, UI_COL_BG_BOT);

    /* Top header bar */
    ui_draw_header("Moonlight Settings");

    /* --- Setting rows --- */
    draw_setting_row(MENU_ITEM_RESOLUTION,
                     "Resolution",
                     RESOLUTION_LABELS[g_menu_state.resolutionIndex],
                     g_menu_state.currentSelection == MENU_ITEM_RESOLUTION);

    draw_setting_row(MENU_ITEM_FPS,
                     "Frame Rate",
                     FPS_OPTIONS[g_menu_state.fpsIndex],
                     g_menu_state.currentSelection == MENU_ITEM_FPS);

    draw_setting_row(MENU_ITEM_CONTROL_MODE,
                     "Control Mode",
                     CONTROL_MODE_OPTIONS[g_menu_state.controlModeIndex],
                     g_menu_state.currentSelection == MENU_ITEM_CONTROL_MODE);

    draw_setting_row(MENU_ITEM_THEME,
                     "Theme",
                     THEME_OPTIONS[g_menu_state.uiThemeIndex],
                     g_menu_state.currentSelection == MENU_ITEM_THEME);

    /* Bitrate setting row (selectable, Left/Right to adjust) */
    {
        char brate_val[16];
        snprintf(brate_val, sizeof(brate_val), "%d kbps", g_menu_state.bitrate);
        draw_setting_row(MENU_ITEM_BITRATE,
                         "Bitrate",
                         brate_val,
                         g_menu_state.currentSelection == MENU_ITEM_BITRATE);
    }

    /* Footer hint — context-sensitive for Custom resolution */
    if (g_menu_state.currentSelection == MENU_ITEM_RESOLUTION &&
        g_menu_state.resolutionIndex == RESOLUTION_CUSTOM_INDEX) {
        ui_draw_footer_hint(
            "L/R: Change  X: Edit  Start: Save  /\\: Skip");
    } else {
        ui_draw_footer_hint(
            "Up/Down: Navigate  L/R: Change  X: Save  /\\: Skip");
    }

    ui_end_frame();
}

/*--------------------------------------------------------------------------
 * settings_menu_init - Initialize settings with defaults
 *--------------------------------------------------------------------------*/
void settings_menu_init(PspConfig *config)
{
    /* Try to load config from file, use defaults if not found */
    loadConfig(config);
    
    /* Initialize menu state from loaded config */
    g_menu_state.currentSelection = 0;

    /* If saved resolutionIndex is Custom, repopulate the custom slot
     * from the saved width/height so the label shows the right value. */
    if (config->resolutionIndex == RESOLUTION_CUSTOM_INDEX &&
        config->width >= 16 && config->height >= 16) {
        resolution_update_custom(config->width, config->height);
        g_menu_state.resolutionIndex = RESOLUTION_CUSTOM_INDEX;
    } else {
        /* Find closest matching preset (presets only, not custom) */
        int best = 0;
        int best_diff = 999999;
        int i;
        for (i = 0; i < RESOLUTION_PRESET_COUNT; i++) {
            int diff = (RESOLUTION_WIDTHS[i] > config->width
                            ? RESOLUTION_WIDTHS[i] - config->width
                            : config->width - RESOLUTION_WIDTHS[i])
                     + (RESOLUTION_HEIGHTS[i] > config->height
                            ? RESOLUTION_HEIGHTS[i] - config->height
                            : config->height - RESOLUTION_HEIGHTS[i]);
            if (diff < best_diff) {
                best_diff = diff;
                best = i;
            }
        }
        g_menu_state.resolutionIndex = best;
    }

    g_menu_state.fpsIndex = clamp_index(config->fpsIndex, 0, FPS_COUNT - 1);
    g_menu_state.controlModeIndex = clamp_index((int)config->controlMode, 0, CONTROL_MODE_COUNT - 1);
    g_menu_state.bitrate = clamp_index(config->bitrate, BITRATE_MIN, MAX_BITRATE);
    g_menu_state.uiThemeIndex = clamp_index(config->uiThemeIndex, 0, 9);
    g_menu_state.needsRedraw = 1;
}

/*--------------------------------------------------------------------------
 * Helper: Update config based on menu state
 *--------------------------------------------------------------------------*/
static void update_config_from_menu(PspConfig *config)
{
    /* Resolution: set width/height from selected preset */
    config->resolutionIndex = g_menu_state.resolutionIndex;
    config->width  = RESOLUTION_WIDTHS[g_menu_state.resolutionIndex];
    config->height = RESOLUTION_HEIGHTS[g_menu_state.resolutionIndex];
    
    /* Update FPS — uses FPS_VALUES array for clean mapping */
    config->fpsIndex = g_menu_state.fpsIndex;
    config->fps = FPS_VALUES[g_menu_state.fpsIndex];
    
    /* Update control mode */
    config->controlMode = (ControlMode)g_menu_state.controlModeIndex;

    /* Update bitrate */
    config->bitrate = g_menu_state.bitrate;
    config->uiThemeIndex = g_menu_state.uiThemeIndex;
}

/*--------------------------------------------------------------------------
 * settings_menu_run - Main menu loop
 *--------------------------------------------------------------------------*/
int settings_menu_run(PspConfig *config)
{
    SceCtrlData pad;
    SceCtrlData prev_pad;
    int result = -1;
    
    /* Auto-skip DISABLED — manual navigation only via RemoteJoy */

    /* Initialize controller sampling */
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);
    
    /* Get initial state for BOTH pad and prev_pad to avoid garbage-button detection */
    memset(&pad, 0, sizeof(pad));
    memset(&prev_pad, 0, sizeof(prev_pad));
    sceCtrlPeekBufferPositive(&pad, 1);
    pad.Buttons |= g_remote_buttons; g_remote_buttons = 0;
    memcpy(&prev_pad, &pad, sizeof(pad));
    
    /* Main menu loop */
    while (1) {
        /* Save previous state, then read current controller state */
        memcpy(&prev_pad, &pad, sizeof(pad));
        sceCtrlPeekBufferPositive(&pad, 1);
        pad.Buttons |= g_remote_buttons; g_remote_buttons = 0;
        
        /* Keep settings-menu input local until stream session is active. */
        
        /* Check for D-pad UP */
        if ((pad.Buttons & PSP_CTRL_UP) && !(prev_pad.Buttons & PSP_CTRL_UP)) {
            g_menu_state.currentSelection--;
            if (g_menu_state.currentSelection < 0) {
                g_menu_state.currentSelection = MENU_ITEM_COUNT - 1;
            }
            g_menu_state.needsRedraw = 1;
        }
        
        /* Check for D-pad DOWN */
        if ((pad.Buttons & PSP_CTRL_DOWN) && !(prev_pad.Buttons & PSP_CTRL_DOWN)) {
            g_menu_state.currentSelection++;
            if (g_menu_state.currentSelection >= MENU_ITEM_COUNT) {
                g_menu_state.currentSelection = 0;
            }
            g_menu_state.needsRedraw = 1;
        }
        
        /* Check for D-pad LEFT */
        if ((pad.Buttons & PSP_CTRL_LEFT) && !(prev_pad.Buttons & PSP_CTRL_LEFT)) {
            switch (g_menu_state.currentSelection) {
                case MENU_ITEM_RESOLUTION:
                    g_menu_state.resolutionIndex--;
                    if (g_menu_state.resolutionIndex < 0) {
                        g_menu_state.resolutionIndex = RESOLUTION_COUNT - 1;
                    }
                    autocouple_from_resolution(); /* useless-box: snap FPS + bitrate */
                    break;
                case MENU_ITEM_FPS:
                    g_menu_state.fpsIndex--;
                    if (g_menu_state.fpsIndex < 0) {
                        g_menu_state.fpsIndex = FPS_COUNT - 1;
                    }
                    autocouple_bitrate_from_fps(); /* useless-box: snap bitrate */
                    break;
                case MENU_ITEM_THEME:
                    g_menu_state.uiThemeIndex = clamp_index(g_menu_state.uiThemeIndex - 1, 0, 9);
                    ui_apply_theme(g_menu_state.uiThemeIndex);
                    break;
                case MENU_ITEM_CONTROL_MODE:
                    g_menu_state.controlModeIndex--;
                    if (g_menu_state.controlModeIndex < 0) {
                        g_menu_state.controlModeIndex = 1; /* CONTROL_MODE_COUNT-1 */
                    }
                    break;
                case MENU_ITEM_BITRATE:
                    g_menu_state.bitrate -= BITRATE_STEP;
                    if (g_menu_state.bitrate < BITRATE_MIN) {
                        g_menu_state.bitrate = BITRATE_MIN;
                    }
                    break;
            }
            g_menu_state.needsRedraw = 1;
        }
        
        /* Check for D-pad RIGHT */
        if ((pad.Buttons & PSP_CTRL_RIGHT) && !(prev_pad.Buttons & PSP_CTRL_RIGHT)) {
            switch (g_menu_state.currentSelection) {
                case MENU_ITEM_RESOLUTION:
                    g_menu_state.resolutionIndex++;
                    if (g_menu_state.resolutionIndex >= RESOLUTION_COUNT) {
                        g_menu_state.resolutionIndex = 0;
                    }
                    autocouple_from_resolution(); /* useless-box: snap FPS + bitrate */
                    break;
                case MENU_ITEM_FPS:
                    g_menu_state.fpsIndex++;
                    if (g_menu_state.fpsIndex >= FPS_COUNT) {
                        g_menu_state.fpsIndex = 0;
                    }
                    autocouple_bitrate_from_fps(); /* useless-box: snap bitrate */
                    break;
                case MENU_ITEM_CONTROL_MODE:
                    g_menu_state.controlModeIndex++;
                    if (g_menu_state.controlModeIndex >= CONTROL_MODE_COUNT) {
                        g_menu_state.controlModeIndex = 0;
                    }
                    break;
                case MENU_ITEM_THEME:
                    g_menu_state.uiThemeIndex = clamp_index(g_menu_state.uiThemeIndex + 1, 0, 9);
                    ui_apply_theme(g_menu_state.uiThemeIndex);
                    break;
                case MENU_ITEM_BITRATE:
                    g_menu_state.bitrate += BITRATE_STEP;
                    if (g_menu_state.bitrate > MAX_BITRATE) {
                        g_menu_state.bitrate = MAX_BITRATE;
                    }
                    break;
            }
            g_menu_state.needsRedraw = 1;
        }
        
        /* Check for CROSS — context-dependent:
         *   If Resolution row is focused AND set to Custom → open resolution OSK
         *   Otherwise → Save and Continue (same as Start) */
        if ((pad.Buttons & PSP_CTRL_CROSS) && !(prev_pad.Buttons & PSP_CTRL_CROSS)) {
            if (g_menu_state.currentSelection == MENU_ITEM_RESOLUTION &&
                g_menu_state.resolutionIndex == RESOLUTION_CUSTOM_INDEX) {
                int cw, ch;
                if (osk_get_resolution_input(&cw, &ch) == 0) {
                    resolution_update_custom(cw, ch);
                    autocouple_from_resolution();
                }
                g_menu_state.needsRedraw = 1;
            } else {
                update_config_from_menu(config);
                saveConfig(config);
                result = 0;
                break;
            }
        }

        /* Check for START (always Save and Continue) */
        if ((pad.Buttons & PSP_CTRL_START) && !(prev_pad.Buttons & PSP_CTRL_START)) {
            update_config_from_menu(config);
            saveConfig(config);
            result = 0;
            break;
        }
        
        /* Check for TRIANGLE (Continue without saving), edge-triggered */
        if ((pad.Buttons & PSP_CTRL_TRIANGLE) && !(prev_pad.Buttons & PSP_CTRL_TRIANGLE)) {
            result = 0;
            break;
        }
        
        /* Redraw if needed */
        if (g_menu_state.needsRedraw) {
            settings_menu_draw(config);
            g_menu_state.needsRedraw = 0;
        }
        
        /* Small delay */
        sceKernelDelayThread(16667);  /* ~60 Hz */
    }
    
    return result;
}

/*--------------------------------------------------------------------------
 * settings_menu_apply - Apply settings to Moonlight STREAM_CONFIGURATION
 *--------------------------------------------------------------------------*/
void settings_menu_apply(const PspConfig *psp_config, void *stream_config_ptr)
{
    /* Note: We use void* to avoid including Limelight.h in the header
     * The actual type is PSTREAM_CONFIGURATION */
    
    /* This function should be called with a properly initialized
     * STREAM_CONFIGURATION. We only update the fields we control. */
    
    /* The caller should use LiInitializeStreamConfiguration() first,
     * then call this function to apply our settings. */
    
    /* Example usage:
     *   STREAM_CONFIGURATION streamConfig;
     *   LiInitializeStreamConfiguration(&streamConfig);
     *   settings_menu_apply(&psp_config, &streamConfig);
     *   LiStartConnection(&serverInfo, &streamConfig, ...);
     */
    
    /* We'll cast and update - the caller is responsible for proper initialization */
    typedef struct {
        int width;
        int height;
        int fps;
        int bitrate;
        int packetSize;
        int streamingRemotely;
        int audioConfiguration;
        int supportedVideoFormats;
        int clientRefreshRateX100;
        int colorSpace;
        int colorRange;
        int encryptionFlags;
        char remoteInputAesKey[16];
        char remoteInputAesIv[16];
    } StreamConfig;
    
    StreamConfig *sc = (StreamConfig *)stream_config_ptr;
    
    sc->width = psp_config->width;
    sc->height = psp_config->height;
    sc->fps = psp_config->fps;
    sc->bitrate = psp_config->bitrate;
    sc->packetSize = psp_config->packetSize;
    sc->streamingRemotely = psp_config->streamingRemotely;
    sc->audioConfiguration = psp_config->audioConfiguration;
    sc->supportedVideoFormats = psp_config->supportedVideoFormats;
    sc->clientRefreshRateX100 = psp_config->clientRefreshRateX100;
    sc->colorSpace = psp_config->colorSpace;
    sc->colorRange = psp_config->colorRange;
    sc->encryptionFlags = psp_config->encryptionFlags;
    memcpy(sc->remoteInputAesKey, psp_config->remoteInputAesKey, 16);
    memcpy(sc->remoteInputAesIv, psp_config->remoteInputAesIv, 16);
}