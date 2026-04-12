/*
 * ui_manager.h - PSP Moonlight Modular UI Manager
 *
 * Hardware-accelerated 2D UI using the PSP GU (Graphics Utility) library
 * and intraFont for text rendering from the PSP's internal PGF font files.
 *
 * Design constraints:
 *   - PSP-1000: 32 MB RAM, 2 MB VRAM
 *   - Screen: 480x272 (exact native, NEVER scale)
 *   - 16-bit RGB565 buffers to halve VRAM usage vs RGBA8888
 *   - No STL containers — fixed-size arrays only (MIPS R4000 friendly)
 *   - No exceptions, no RTTI (matches CXXFLAGS in Makefile)
 *   - GU colours are ABGR (0xAABBGGRR in memory, low byte = R)
 *
 * Usage pattern:
 *   ui_manager_init();          // call once after display_init()
 *   ui_begin_frame();           // open a new GU command list
 *   ui_clear(UI_COL_BG_TOP);    // clear the back buffer
 *   ui_draw_header("Title");    // draw top bar
 *   ui_draw_button(...);        // draw interactive element
 *   ui_end_frame();             // flush GU, swap buffers, wait VBlank
 *   ui_manager_shutdown();      // call on exit
 */

#ifndef UI_MANAGER_H
#define UI_MANAGER_H

#include <psptypes.h>
#include <pspctrl.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * Screen constants — do NOT use any other values
 * ========================================================================= */
#define UI_SCREEN_W     480
#define UI_SCREEN_H     272
#define UI_SCREEN_STRIDE 512    /* Hardware VRAM pitch in pixels */

/* =========================================================================
 * Colour palette (ABGR — PSP GU native format)
 * A is the HIGH byte; R is the LOW byte when stored as u32 little-endian.
 *   0xAABBGGRR
 * ========================================================================= */

/* --- Backgrounds --- */
#define UI_COL_BG_TOP      0xFF100C08u   /* Indigo-black gradient top         */
#define UI_COL_BG_BOT      0xFF201810u   /* Warmer dark at bottom             */

/* --- Panels / cards --- */
#define UI_COL_PANEL       0xCC181818u   /* Dark panel, 80% opaque            */
#define UI_COL_PANEL_DARK  0xE0101010u   /* Deeper overlay panel              */

/* --- Borders --- */
#define UI_COL_BORDER      0xFF383838u   /* Subtle border                     */
#define UI_COL_BORDER_FOC  0xFFFFFFFFu   /* Bright white — focused element    */

/* --- Dynamic UI Colors --- */
#ifdef __cplusplus
extern "C" {
#endif
extern u32 g_ui_accent_color;
extern u32 g_ui_bg_color;
extern u32 g_ui_text_color;
extern u32 g_ui_text_dim_color;
extern u32 g_ui_text_foc_color;
extern u32 g_ui_card_color;
#ifdef __cplusplus
}
#endif
#define UI_COL_ACCENT      g_ui_accent_color
#define UI_COL_TEXT        g_ui_text_color
#define UI_COL_TEXT_DIM    g_ui_text_dim_color
#define UI_COL_TEXT_FOCUS  g_ui_text_foc_color

/* --- Header / footer --- */
extern u32 g_ui_header_color;
#define UI_COL_HEADER      g_ui_header_color
#define UI_COL_FOOTER      g_ui_header_color

/* --- Error modal --- */
#define UI_COL_ERR_OVERLAY 0xC0000010u   /* Dark tinted overlay               */
#define UI_COL_ERR_PANEL   0xFF181818u   /* Error box background              */
#define UI_COL_ERR_BORDER  0xFF2020CCu   /* Red border: R=CC,G=20,B=20        */
#define UI_COL_ERR_TITLE   0xFF2020FFu   /* Red title text                    */

/* --- Game grid cards --- */
#define UI_COL_CARD        g_ui_card_color
extern u32 g_ui_sel_color;
#define UI_COL_CARD_SEL    g_ui_sel_color

/* --- HUD overlay --- */
#define UI_COL_HUD_BG      0xA0000000u   /* 63% opaque black                  */
#define UI_COL_HUD_TEXT    0xFFFFFFFFu   /* White stats text                  */
#define UI_COL_HUD_QUIT    0xFF00FF00u   /* Green: R=0,G=FF,B=0               */

/* =========================================================================
 * UI State Machine
 *
 * Maps 1-to-1 with the "UI Flow.txt" state transitions.
 * ========================================================================= */
typedef enum {
    UI_STATE_BOOT        = 0,  /* Initial splash / loading                   */
    UI_STATE_SETTINGS    = 1,  /* Settings screen (pre-connection)           */
    UI_STATE_HOST_LIST   = 2,  /* Host discovery list                        */
    UI_STATE_PAIRING     = 3,  /* 4-digit PIN display                        */
    UI_STATE_LIBRARY     = 4,  /* 3x2 game grid                             */
    UI_STATE_STREAMING   = 5,  /* Active stream (HUD overlay available)      */
    UI_STATE_ERROR       = 6,  /* Error modal on top of current state        */
    UI_STATE_COUNT
} UIState;

/* =========================================================================
 * UI Events — produced by ui_process_input()
 * ========================================================================= */
typedef enum {
    UI_EVT_NONE    = 0,
    UI_EVT_UP,             /* D-pad up (navigate)         */
    UI_EVT_DOWN,           /* D-pad down                  */
    UI_EVT_LEFT,           /* D-pad left (value toggle)   */
    UI_EVT_RIGHT,          /* D-pad right                 */
    UI_EVT_SELECT,         /* Cross button — confirm       */
    UI_EVT_BACK,           /* Circle button — cancel/back  */
    UI_EVT_START,          /* Start button                */
    UI_EVT_SCAN,           /* Square button — scan/action  */
    UI_EVT_MENU,           /* Select/Home — toggle HUD     */
    UI_EVT_L_HELD,         /* L held (combo modifier)      */
    UI_EVT_R_HELD,         /* R held (combo modifier)      */
} UIEvent;

/* =========================================================================
 * Component structures — plain-C, no vtables, fixed sizes
 * ========================================================================= */

/* A focusable button (label + bounding box) */
typedef struct {
    int  x, y, w, h;
    char label[48];
    int  focused;          /* 1 = draw with bright border  */
} UiButton;

/* A background panel / card */
typedef struct {
    int  x, y, w, h;
    u32  bg_color;
    u32  border_color;
    char title[48];        /* Optional title drawn top-left */
} UiPanel;

/* =========================================================================
 * Public API — callable from C and C++
 * ========================================================================= */

/* --- Lifecycle ---------------------------------------------------------- */

/**
 * ui_manager_init - Initialise the UI system.
 *
 * Must be called AFTER display_init() (GU must already be up).
 * Loads the PSP internal Latin font from flash0:/font/ltn0.pgf via
 * intraFont so text can be rendered at any point in the UI.
 *
 * Returns 0 on success, negative error code on failure.
 */
int  ui_manager_init(void);

/**
 * ui_manager_shutdown - Release all UI resources (font, etc.)
 */
void ui_manager_shutdown(void);

/**
 * ui_apply_theme - Switch the theme based on the 10-theme index.
 */
void ui_apply_theme(int index);

/* --- Frame management --------------------------------------------------- */

/**
 * ui_begin_frame - Start a new GU rendering frame.
 *
 * Opens a GU_DIRECT command list. All draw calls must happen between
 * ui_begin_frame() and ui_end_frame().
 */
void ui_begin_frame(void);

/**
 * ui_end_frame - Finish the GU frame, swap buffers, wait for VBlank.
 *
 * Calls sceGuFinish, sceGuSync, sceGuSwapBuffers, sceDisplayWaitVblankStart.
 * VBlank sync prevents horizontal tearing on the PSP LCD.
 */
void ui_end_frame(void);

/**
 * ui_end_frame_no_swap - Finish the GU frame without swapping buffers.
 *
 * Calls sceGuFinish, sceGuSync. Does NOT wait for VBlank or swap buffers.
 * Useful when multiple layers (like the HUD) need to be composited directly
 * over the ui_begin_frame commands before calling display_frame_finish().
 */
void ui_end_frame_no_swap(void);

/* --- State machine ------------------------------------------------------ */

void    ui_set_state(UIState s);
UIState ui_get_state(void);

/* --- Input handling  ---------------------------------------------------- */

/**
 * ui_process_input - Sample PSP controller and return a debounced event.
 *
 * Call once per frame BEFORE your render logic.
 * Returns UI_EVT_NONE if no actionable button was pressed this frame.
 *
 * Also updates analog stick values accessible via ui_get_analog_x/y().
 */
UIEvent ui_process_input(void);

/** Raw analog stick values [-128 .. +127] after calling ui_process_input. */
int ui_get_analog_x(void);
int ui_get_analog_y(void);

/* --- Primitives --------------------------------------------------------- */

/**
 * ui_clear - Fill the back buffer with a solid colour.
 * Call this as the first draw call inside ui_begin_frame / ui_end_frame.
 */
void ui_clear(u32 color);

/**
 * ui_draw_gradient_bg - Draw a 2-stop vertical gradient background.
 * Simulates the XMB style dark-to-slightly-lighter bottom wash.
 *
 * @top_color:    colour at y=0
 * @bottom_color: colour at y=UI_SCREEN_H
 */
void ui_draw_gradient_bg(u32 top_color, u32 bottom_color);

/**
 * ui_draw_rect - Draw a filled axis-aligned rectangle.
 * Respects alpha (blending must be enabled via ui_set_blend(1) first
 * for translucent colours to work correctly).
 */
void ui_draw_rect(int x, int y, int w, int h, u32 color);

/**
 * ui_draw_border - Draw a hollow border rectangle (4 lines).
 *
 * @thickness: border width in pixels (1–4 recommended)
 */
void ui_draw_border(int x, int y, int w, int h, int thickness, u32 color);

/**
 * ui_set_blend - Enable or disable GU alpha blending.
 * Enable before drawing translucent elements, disable afterwards.
 *
 * @enable: 1 = enable blend, 0 = disable
 */
void ui_set_blend(int enable);

/* --- Text (intraFont) --------------------------------------------------- */

/**
 * ui_draw_text - Render a null-terminated ASCII string at (x, y).
 *
 * Uses the PSP's internal ltn0.pgf font via intraFont.
 * Font scale is 0.45 (looks sharp at 480x272 without going too small).
 *
 * @color: ABGR colour for the text
 * Returns the X position after the last character (for chaining).
 */
float ui_draw_text(float x, float y, u32 color, const char *text);

/**
 * ui_draw_text_centered - Render text horizontally centered within [cx, cx+cw].
 *
 * Calculates character width * len / 2 to centre text without requiring
 * a full measure pass (uses a fixed average char width approximation that
 * is accurate enough for the PSP's limited screen width).
 */
void  ui_draw_text_centered(float cx, float cw, float y, u32 color, const char *text);

void  ui_draw_text_medium_centered(float cx, float cw, float y, u32 color, const char *text);

float ui_draw_text_right(float right_margin_x, float y, u32 color, const char *text);

/**
 * ui_draw_text_large - Render text at 0.8 scale (for PIN digits / titles).
 */
float ui_draw_text_large(float x, float y, u32 color, const char *text);

/**
 * ui_draw_text_small - Render text at 0.35 scale (for hints / labels).
 */
float ui_draw_text_small(float x, float y, u32 color, const char *text);

/**
 * ui_draw_pin_large - Render a short string (e.g. 4-digit PIN) at 2× scale,
 * horizontally centred on center_x (uses INTRAFONT_ALIGN_CENTER internally).
 *
 * Uses the proven-working intraFont blend path.  Preferred over drawing
 * coloured rects because real PSP-1000 GE state after font rendering can
 * leave the texture environment in a mode that makes untextured rects
 * invisible.
 *
 * @center_x:    horizontal centre of the rendered string (pixels)
 * @baseline_y:  intraFont baseline Y coordinate
 * @color:       ABGR text colour
 * @text:        null-terminated string (typically a 4-digit PIN)
 */
void ui_draw_pin_large(int center_x, int baseline_y, u32 color, const char *text);

/* --- Composite components ---------------------------------------------- */

/**
 * ui_draw_header - Draw the top navigation bar (height = 28 px).
 *
 * Renders a dark semi-transparent bar across the full width with the
 * given title text left-aligned and an accent underline.
 * Leaves 28 px at the top of the screen consumed; draw content below y=32.
 *
 * @title: screen/section title string
 */
void ui_draw_header(const char *title);

/**
 * ui_draw_footer_hint - Draw a single button hint in the bottom bar.
 *
 * The footer bar is rendered at y=252 (height=20). Place hints by
 * calling this function for each hint from left to right.
 * Typical use: ui_draw_footer_hint(8, "[X] Select  [O] Back  [[]Scan");
 *
 * @hint_text: the complete hint string (e.g. "X:Select  O:Back")
 */
void ui_draw_footer_hint(const char *hint_text);

/**
 * ui_draw_panel - Draw a titled background panel (card).
 */
void ui_draw_panel(const UiPanel *p);

/**
 * ui_draw_button - Draw a labelled interactive button.
 *
 * Renders a filled rounded-ish rectangle with text centred inside.
 * When focused=1 it draws with a bright white border and accent bg.
 *
 * @b: pointer to UiButton describing position, size, label, focus
 */
void ui_draw_button(const UiButton *b);

/**
 * ui_draw_error_modal - Draw a full-screen error overlay.
 *
 * Renders a dark overlay then a centred error box with red border,
 * an error title and a detail message. The caller must call ui_end_frame()
 * to display it.
 *
 * @title:   short error title (e.g. "Connection Timeout")
 * @message: longer explanation (e.g. "Host 192.168.1.100 not reachable.")
 * @hint:    button hint (e.g. "Press X to retry, O to quit")
 */
void ui_draw_error_modal(const char *title, const char *message, const char *hint);

/**
 * ui_show_fatal_error - Blocking fatal-error modal.
 *
 * Draws gradient background + error modal overlay on every frame and
 * loops until the user presses Circle (UI_EVT_BACK) or Cross.
 * Use this for non-recoverable errors (network watchdog timeout, etc.)
 * that require the user's acknowledgement before returning to the host list.
 *
 * @title:  short error title, e.g. "Stream Timeout"
 * @msg:    detail message, e.g. "No video received for 30 s."
 */
void ui_show_fatal_error(const char *title, const char *msg);

/**
 * ui_draw_progress_bar - Draw a horizontal progress bar.
 *
 * Useful for showing Wi-Fi signal strength, download progress, etc.
 *
 * @x, @y, @w, @h:  bounding box
 * @value:          current value (0 .. max)
 * @max:            maximum value
 * @label:          optional label drawn to the right (NULL = no label)
 */
void ui_draw_progress_bar(int x, int y, int w, int h,
                          float value, float max, const char *label);

/**
 * ui_draw_spinner - Draw an animated "loading" dot row.
 * Uses sceKernelGetSystemTimeLow() internally for animation timing.
 * Call once per frame.
 *
 * @x, @y:  top-left of the spinner area
 * @label:  text to the right of the dots (e.g. "Connecting...")
 */
void ui_draw_spinner(int x, int y, const char *label);

/**
 * ui_draw_texture_placeholder - Draw a placeholder game-icon card.
 *
 * Used when a game's artwork PNG has not finished downloading yet.
 * Draws a dark card with "[ title ]" text centred. Falls back to
 * this automatically when imageData is NULL in the game grid.
 *
 * @x, @y:   top-left position
 * @w, @h:   card dimensions (should match ICON_WIDTH / ICON_HEIGHT)
 * @title:   game title string
 * @focused: 1 = draw focus border
 */
void ui_draw_texture_placeholder(int x, int y, int w, int h,
                                  const char *title, int focused);

void ui_draw_rect_rounded(int x, int y, int w, int h, int r, u32 color);

/**
 * ui_draw_hollow_rect_rounded - Draw a hollow rounded border frame.
 * Used to draw thick cookie-cutter bezels over inset rectangles.
 */
void ui_draw_hollow_rect_rounded(int x, int y, int w, int h, int r, int t, u32 color);

/**
 * ui_draw_texture_rgb565 - Blit and SCALE an RGB565 texture via GU.
 * 
 * dw, dh = destination screen size
 * sw, sh = source pixels to read (fixed at 100x150 for icons)
 * tw, th = texture buffer size (must be power of 2)
 */
void ui_draw_texture_rgb565(int x, int y, int dw, int dh,
                             void *tex_data, int sw, int sh,
                             int tw, int th, int focused);

void ui_draw_texture_rounded_rgb565(int x, int y, int dw, int dh, int r,
                                    void *tex_data, int sw, int sh, 
                                    int tw, int th, int focused);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* UI_MANAGER_H */
