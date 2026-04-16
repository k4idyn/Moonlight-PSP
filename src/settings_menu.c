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
#include <math.h>

#include "settings_menu.h"
#include "config.h"
#include "ui_manager.h"
#include "osk_input.h"

extern volatile unsigned int g_remote_buttons;

/*--------------------------------------------------------------------------\n * Option array definitions (declared extern in settings_menu.h)
 *
 * MATHEMATICAL DERIVATION (706 samples, 14 hardware runs):
 *   CPU decode:   385 µs per 1000 pixels (H.264 baseline, OpenH264 -O2)
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

/* FPS values that evenly divide 60Hz (no VSync judder) */
static char s_custom_fps_label[16] = "Custom";
const char * const FPS_OPTIONS[5] = {
    "15 FPS",
    "20 FPS",
    "30 FPS",
    "60 FPS",
    s_custom_fps_label
};
const int FPS_VALUES[5] = { 15, 20, 30, 60, -1 };

/* Forward declaration needed for fps_update_custom and resolution_update_custom */
static MenuState g_menu_state;

void resolution_update_custom(int width, int height)
{
    RESOLUTION_WIDTHS[RESOLUTION_CUSTOM_INDEX]  = width;
    RESOLUTION_HEIGHTS[RESOLUTION_CUSTOM_INDEX] = height;
    snprintf(s_custom_label, sizeof(s_custom_label), "%dx%d", width, height);
}

void fps_update_custom(int fps)
{
    g_menu_state.customFpsValue = fps;
    snprintf(s_custom_fps_label, sizeof(s_custom_fps_label), "%d FPS", fps);
}

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
#define ROW_Y_START   36    /* First setting row top pixel          */
#define ROW_HEIGHT    32    /* Height of each setting row card      */
#define ROW_GAP        8    /* Gap between rows                     */
#define ROW_X         12    /* Left edge of setting cards           */
#define ROW_W        456    /* Card width                           */
#define VAL_W        120    /* Width of the value box on right side */

/*--------------------------------------------------------------------------
 * Menu Item Indices
 *--------------------------------------------------------------------------*/
#define MENU_ITEM_RESOLUTION    0
#define MENU_ITEM_FPS           1
#define MENU_ITEM_AUDIO         2
#define MENU_ITEM_CONTROL_MODE  3
#define MENU_ITEM_BUTTON_MAP    4
#define MENU_ITEM_THEME         5
#define MENU_ITEM_BITRATE       6
#define MENU_ITEM_COUNT         7

#define BITRATE_MIN             32

/* Bitrate presets: multiples of 64 for clean codec alignment.
 * Increasing step sizes at higher bitrates (64→128→256→512). */
static const int BITRATE_PRESETS[] = {
    64, 128, 256, 384, 512, 768, 1024, 1280, 1536, 2048, 2560
};
#define BITRATE_PRESET_COUNT  (int)(sizeof(BITRATE_PRESETS) / sizeof(BITRATE_PRESETS[0]))

static int bitrate_preset_index(int bitrate)
{
    int i, best = 0, best_diff = 999999;
    for (i = 0; i < BITRATE_PRESET_COUNT; i++) {
        int diff = bitrate > BITRATE_PRESETS[i]
                 ? bitrate - BITRATE_PRESETS[i]
                 : BITRATE_PRESETS[i] - bitrate;
        if (diff < best_diff) { best_diff = diff; best = i; }
    }
    return best;
}

/*--------------------------------------------------------------------------
 * Local State - g_menu_state declared earlier for forward reference
 *--------------------------------------------------------------------------*/
static float s_scroll_curr   = 0.0f;  /* current visible scroll position (integer after snap) */
static float s_target_camera = 0.0f;  /* camera target set by boundary-scroll logic            */
static float s_focus_anim    = 0.0f;  /* lerps toward currentSelection for the pop animation   */

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
 * Helper: draw one setting row as a card + value selector
 *
 * Each row looks like:
 *   [  Label Name              < VALUE >  ]
 * The card background is darker when not focused, brighter when focused.
 *--------------------------------------------------------------------------*/
static void draw_setting_row(int row_idx, const char *label,
                             const char *value, int is_selected)
{
    /* Focus pop: the selected row grows 2% while the focus animation lerps.
     * Cards further from the animated focus position stay at normal size. */
    float dist  = fabsf((float)row_idx - s_focus_anim);
    float scale = (dist < 1.0f) ? (1.0f + (1.0f - dist) * 0.02f) : 1.0f;
    int item_h  = (int)((float)ROW_HEIGHT * scale);
    int item_w  = (int)((float)ROW_W      * scale);

    /* Row Y from smooth scroll; centre the grown card on its natural edge */
    int ry_start = 41;
    float ry_f   = ry_start + ((float)row_idx - s_scroll_curr) * (float)(ROW_HEIGHT + ROW_GAP);
    int ry_base  = (int)ry_f;
    int ry = ry_base - (item_h - ROW_HEIGHT) / 2;
    int rx = ROW_X  - (item_w - ROW_W)  / 2;

    /* Hard-cull rows fully outside the safe zone */
    #define HEADER_BOTTOM 32
    #define FOOTER_TOP    244
    if (ry + item_h <= HEADER_BOTTOM || ry >= FOOTER_TOP) {
        return;
    }

    UiButton card;
    card.x = rx;
    card.y = ry;
    card.w = item_w;
    card.h = item_h;
    card.focused = is_selected;
    card.label[0] = '\0';
    ui_draw_button(&card);

    /* Label on the left — selected row text is 2px taller */
    float label_scale = is_selected ? 0.50f : 0.45f;
    ui_draw_text_scaled((float)(rx + 10),
                 (float)(ry + item_h / 2 + 4),
                 is_selected ? UI_COL_TEXT_FOCUS : UI_COL_TEXT,
                 label, label_scale);

    /* Value selector: toggle items get < > arrows; action items (Button Map)
     * just highlight the label in focus colour — no left/right toggle arrows. */
    int vx = rx + item_w - VAL_W - 8;
    if (is_selected && row_idx != MENU_ITEM_BUTTON_MAP) {
        float vy = (float)(ry + item_h / 2 + 4);
        float x2 = ui_draw_text_scaled((float)vx, vy, UI_COL_ACCENT, "< ", label_scale);
        x2 = ui_draw_text_scaled(x2, vy, UI_COL_TEXT_FOCUS, value, label_scale);
        ui_draw_text_scaled(x2, vy, UI_COL_ACCENT, " >", label_scale);
    } else {
        ui_draw_text_scaled((float)(vx + 16),
                     (float)(ry + item_h / 2 + 4),
                     is_selected ? UI_COL_TEXT_FOCUS : UI_COL_TEXT_DIM,
                     value, label_scale);
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

    /* Frame-delta compensation for consistent animation speed */
    static u32 s_last_anim_us = 0;
    u32 now_us = sceKernelGetSystemTimeLow();
    float dt = 1.0f;
    if (s_last_anim_us != 0) {
        u32 elapsed = now_us - s_last_anim_us;
        dt = (float)elapsed / 16667.0f;
        if (dt > 4.0f) dt = 4.0f;
        if (dt < 0.1f) dt = 0.1f;
    }
    s_last_anim_us = now_us;

    /* Bounded step-ladder camera: scroll ONLY when cursor hits visible edge.
     * While currentSelection stays within [camera, camera+4], camera is fixed.
     * Pushing past either edge advances the camera exactly 1 slot. */
    float sel = (float)g_menu_state.currentSelection;
    if (sel < s_target_camera) {
        s_target_camera = sel;
    } else if (sel > s_target_camera + 4.0f) {
        s_target_camera = sel - 4.0f;
    }

    /* Focus pop animation: lerps toward current selection for item scale-up */
    s_focus_anim  += ((float)g_menu_state.currentSelection - s_focus_anim)  * 0.20f * dt;
    /* Smooth scroll: camera lerps toward target each VBlank (continuous draw loop) */
    s_scroll_curr += (s_target_camera - s_scroll_curr) * 0.15f * dt;

    ui_begin_frame();

    /* Gradient background: dark indigo top, slightly warmer bottom */
    ui_draw_gradient_bg(UI_COL_BG_TOP, UI_COL_BG_BOT);

    /* Constrain all row drawing to the safe zone BETWEEN header and footer.
     * This scissor rect is the hardware guarantee that no pill pixel ever
     * bleeds into the header (y 0-31) or footer (y 244-271) strips. */
    ui_set_scissor(0, 30, 480, 214); /* y=30..244: fills the 2px gap below header pill */

    /* --- Setting rows --- */
    draw_setting_row(MENU_ITEM_RESOLUTION,
                     "Resolution",
                     RESOLUTION_LABELS[g_menu_state.resolutionIndex],
                     g_menu_state.currentSelection == MENU_ITEM_RESOLUTION);

    draw_setting_row(MENU_ITEM_FPS,
                     "Frame Rate",
                     FPS_OPTIONS[g_menu_state.fpsIndex],
                     g_menu_state.currentSelection == MENU_ITEM_FPS);

    draw_setting_row(MENU_ITEM_AUDIO,
                     "Audio",
                     g_menu_state.audioEnabled ? "Enabled" : "Disabled",
                     g_menu_state.currentSelection == MENU_ITEM_AUDIO);

    draw_setting_row(MENU_ITEM_CONTROL_MODE,
                     "Control Mode",
                     CONTROL_MODE_OPTIONS[g_menu_state.controlModeIndex],
                     g_menu_state.currentSelection == MENU_ITEM_CONTROL_MODE);

    draw_setting_row(MENU_ITEM_BUTTON_MAP,
                     "Button Mapping",
                     "Edit >",
                     g_menu_state.currentSelection == MENU_ITEM_BUTTON_MAP);

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

    /* Restore full-screen scissor so header and footer draw unclipped */
    ui_clear_scissor();

    /* Top header bar — drawn AFTER rows so it always sits on top */
    ui_draw_header("Moonlight Settings");

    /* Footer hint — context-sensitive for Custom resolution/fps */
    if ((g_menu_state.currentSelection == MENU_ITEM_RESOLUTION &&
         g_menu_state.resolutionIndex == RESOLUTION_CUSTOM_INDEX) ||
        (g_menu_state.currentSelection == MENU_ITEM_FPS &&
         g_menu_state.fpsIndex == FPS_CUSTOM_INDEX)) {
        ui_draw_footer_hint(
            "{UP}/{DN}: Navigate  {LF}/{RF}: Change  {X}: Edit  {ST}: Save  {TR}: Skip");
    } else {
        ui_draw_footer_hint(
            "{UP}/{DN}: Navigate  {LF}/{RF}: Change  {X}: Save  {TR}: Skip");
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

    /* Reset scroll state so the list always starts at the top when the
     * menu is opened (avoids stale camera position from a prior session). */
    s_scroll_curr   = 0.0f;
    s_target_camera = 0.0f;
    s_focus_anim    = 0.0f;

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
    if (g_menu_state.fpsIndex == FPS_CUSTOM_INDEX) {
        fps_update_custom(config->fps);
    }
    g_menu_state.audioEnabled = config->audioEnabled;
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
    if (config->fpsIndex == FPS_CUSTOM_INDEX) {
        config->fps = g_menu_state.customFpsValue;
    } else {
        config->fps = FPS_VALUES[g_menu_state.fpsIndex];
    }

    config->audioEnabled = g_menu_state.audioEnabled;
    
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
                g_menu_state.currentSelection = 0;
            }
            g_menu_state.needsRedraw = 1;
        }
        
        /* Check for D-pad DOWN */
        if ((pad.Buttons & PSP_CTRL_DOWN) && !(prev_pad.Buttons & PSP_CTRL_DOWN)) {
            g_menu_state.currentSelection++;
            if (g_menu_state.currentSelection >= MENU_ITEM_COUNT) {
                g_menu_state.currentSelection = MENU_ITEM_COUNT - 1;
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
                    break;
                case MENU_ITEM_FPS:
                    g_menu_state.fpsIndex--;
                    if (g_menu_state.fpsIndex < 0) {
                        g_menu_state.fpsIndex = FPS_COUNT - 1;
                    }
                    if (g_menu_state.fpsIndex == FPS_CUSTOM_INDEX && g_menu_state.customFpsValue == 0) g_menu_state.customFpsValue = 24;
                    break;
                case MENU_ITEM_AUDIO:
                    g_menu_state.audioEnabled = !g_menu_state.audioEnabled;
                    break;
                case MENU_ITEM_THEME:
                    g_menu_state.uiThemeIndex = clamp_index(g_menu_state.uiThemeIndex - 1, 0, 9);
                    ui_apply_theme(g_menu_state.uiThemeIndex);
                    break;
                case MENU_ITEM_BUTTON_MAP:
                    /* nothing to change with left/right */
                    break;
                case MENU_ITEM_CONTROL_MODE:
                    g_menu_state.controlModeIndex--;
                    if (g_menu_state.controlModeIndex < 0) {
                        g_menu_state.controlModeIndex = 1; /* CONTROL_MODE_COUNT-1 */
                    }
                    break;
                case MENU_ITEM_BITRATE:
                    {
                        int idx = bitrate_preset_index(g_menu_state.bitrate);
                        if (idx > 0) idx--;
                        g_menu_state.bitrate = BITRATE_PRESETS[idx];
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
                    break;
                case MENU_ITEM_FPS:
                    g_menu_state.fpsIndex++;
                    if (g_menu_state.fpsIndex >= FPS_COUNT) {
                        g_menu_state.fpsIndex = 0;
                    }
                    if (g_menu_state.fpsIndex == FPS_CUSTOM_INDEX && g_menu_state.customFpsValue == 0) g_menu_state.customFpsValue = 24;
                    break;
                case MENU_ITEM_AUDIO:
                    g_menu_state.audioEnabled = !g_menu_state.audioEnabled;
                    break;
                case MENU_ITEM_CONTROL_MODE:
                    g_menu_state.controlModeIndex++;
                    if (g_menu_state.controlModeIndex >= CONTROL_MODE_COUNT) {
                        g_menu_state.controlModeIndex = 0;
                    }
                    break;
                case MENU_ITEM_BUTTON_MAP:
                    /* nothing to change with left/right */
                    break;
                case MENU_ITEM_THEME:
                    g_menu_state.uiThemeIndex = clamp_index(g_menu_state.uiThemeIndex + 1, 0, 9);
                    ui_apply_theme(g_menu_state.uiThemeIndex);
                    break;
                case MENU_ITEM_BITRATE:
                    {
                        int idx = bitrate_preset_index(g_menu_state.bitrate);
                        if (idx < BITRATE_PRESET_COUNT - 1) idx++;
                        g_menu_state.bitrate = BITRATE_PRESETS[idx];
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
                }
                g_menu_state.needsRedraw = 1;
            } else if (g_menu_state.currentSelection == MENU_ITEM_FPS &&
                       g_menu_state.fpsIndex == FPS_CUSTOM_INDEX) {
                int cfps;
                if (osk_get_fps_input(&cfps) == 0) {
                    fps_update_custom(cfps);
                }
                g_menu_state.needsRedraw = 1;
            } else if (g_menu_state.currentSelection == MENU_ITEM_BUTTON_MAP) {
                extern void button_mapping_ui_run(void);
                button_mapping_ui_run();
                /* reload config to pick up any changes, or just redraw */
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
        
        /* Always redraw — smooth scroll and focus-pop animations need
         * every VBlank; ui_end_frame() syncs to display VBlank. */
        settings_menu_draw(config);
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