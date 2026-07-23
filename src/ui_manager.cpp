/*
 * ui_manager.cpp - PSP Moonlight UI Manager Implementation
 *
 * Provides hardware-accelerated 2D rendering via the PSP GU (sceGu*)
 * and intraFont text rendering from the PSP's internal PGF font file.
 *
 * Key performance notes for PSP-1000 (MIPS R4000, 32 MB RAM, 2 MB VRAM):
 *  - We use RGB565 (16-bit) for all textures — half the VRAM of RGBA8888.
 *  - The GU display list lives in MIPS uncached VRAM-mapped memory.
 *  - sceGuGetMemory() is used for all per-frame vertex data; no heap allocs.
 *  - sceDisplayWaitVblankStart() is called once per frame to prevent tearing.
 *  - intraFont caches glyphs into a 256x256 texture that lives in RAM, not
 *    VRAM, preserving the 2 MB VRAM entirely for video frames and UI buffers.
 *
 * ABGR colour format reminder:
 *   GU interprets 32-bit colour as 0xAABBGGRR (A=high byte, R=low byte).
 *   0xFF0000FF = opaque red, 0xFFFF0000 = opaque blue, 0xFF00FF00 = green.
 */

#include <pspkernel.h>
#include <pspdisplay.h>
#include <pspgu.h>
#include <pspgum.h>
#include <pspctrl.h>
#include <psprtc.h>
#include <pspiofilemgr.h>
#include <intraFont.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "ui_manager.h"
#include "diag_log.h"
#include "shared.h"
#include "btn_icons.h"
#include "storage_paths.h"

/* Font handles (PGF) */
static intraFont *s_font_sans  = NULL; /* flash0:/font/ltn0.pgf (Standard) */
static intraFont *s_font_serif = NULL; /* flash0:/font/ltn1.pgf (Serif)   */
static intraFont *s_font_small = NULL; /* flash0:/font/ltn8.pgf (Small)   */
static intraFont **s_active_font = &s_font_sans; /* Pointer to the font handle currently in use */
#define s_font (*s_active_font)

/* Default accent settings */
u32 g_ui_accent_color = 0xFF8B8B2Du; /* Teal */
u32 g_ui_bg_color     = 0xFF32231Au; /* Dark Navy */
u32 g_ui_header_color = 0xE8080808u; /* Dark bar */
u32 g_ui_card_color     = 0xFF242424u; /* Unselected card */
u32 g_ui_sel_color      = 0xFF303030u; /* Gray sel */
u32 g_ui_text_color     = 0xFFFFFFFFu; /* White text */
u32 g_ui_text_dim_color = 0xFF909090u; /* Gray text */
u32 g_ui_text_foc_color = 0xFF00FFFFu; /* Gold text */

/* Theme Palette Definitions (ABGR) */
static const u32 s_theme_accents[10] = {
    0xFF8B8B2Du, 0xFF516FE7u, 0xFF71847Du, 0xFF4F4536u, 0xFF00A9F4u,
    0xFFA56F4Au, 0xFFA5A5D4u, 0xFFFF6600u, 0xFF597C4Au, 0xFFC290A4u
};
static const u32 s_theme_bgs[10] = {
    0xFF32231Au, 0xFF534626u, 0xFF2B4A2Du, 0xFF505050u, 0xFF3A404Au,
    0xFF54443Au, 0xFF462E5Du, 0xFF1E1E1Eu, 0xFF2E3E28u, 0xFF3E1E2Bu
};
static const u32 s_theme_hdrs[10] = {
    0xE8100C08u, 0xE8181010u, 0xE8081808u, 0xE8303030u, 0xE8201810u,
    0xE8201C18u, 0xE8301020u, 0xE8080808u, 0xE8101C10u, 0xE8180818u
};
static const u32 s_theme_sels[10] = {
    0xFF404020u, 0xFF402060u, 0xFF204020u, 0xFF686058u, 0xFF605020u,
    0xFF483828u, 0xFF603050u, 0xFF404040u, 0xFF306020u, 0xFF302040u
};

/* 1, 2, 4, 6, 9 -> Serif (Sunset, Forest, Golden, Desert, Midnight)
 * 0, 3, 5, 7, 8 -> Sans */
void ui_apply_theme(int index) {
    if (index < 0 || index >= 10) index = 0;
    g_ui_accent_color = s_theme_accents[index];
    g_ui_bg_color     = s_theme_bgs[index];
    g_ui_header_color = s_theme_hdrs[index];
    g_ui_sel_color    = s_theme_sels[index];

    if (index == 1 || index == 2 || index == 4 || index == 6 || index == 9) {
        if (s_font_serif) s_active_font = &s_font_serif;
        else s_active_font = &s_font_sans;
    } else {
        s_active_font = &s_font_sans;
    }

    /* All themes use light text on dark backgrounds */
    g_ui_text_color     = 0xFFFFFFFFu;
    g_ui_text_dim_color = 0xFF909090u;
    g_ui_text_foc_color = 0xFF00FFFFu;
    g_ui_card_color     = 0xFF242424u;
}

/* =========================================================================
 * Internal constants
 */
#define HEADER_H 28
#define FOOTER_H 20

extern "C" {

/* GU command list — 256 KB is safer for high-vertex rounded rects           */
#define GU_LIST_WORDS   (256 * 1024 / 4)

#define UI_COL_BORDER      0xFF383838u   /* Subtle border                     */
#define UI_COL_BORDER_FOC  0xFFFFFFFFu   /* Bright white — focused element    */

/* Input debounce: require 180 ms before accepting a repeat key             */
#define INPUT_DEBOUNCE_US   180000u

/* Repeat delay after holding: fires every 120 ms once debounce passed     */
#define INPUT_REPEAT_US     120000u

/* PSP font file on firmware flash                                          */
#define FONT_PATH "flash0:/font/ltn0.pgf"

/* Approximate average character width at scale 0.45 (pixels)             */
#define FONT_AVG_CHAR_W 7

/* =========================================================================
 * GU vertex types — must match the GU format flags passed to sceGuDrawArray
 * ========================================================================= */

/* Coloured sprite vertex (no texture) */
typedef struct {
    u32   color;
    float x, y, z;
} ColorVert;

/* Textured sprite vertex */
typedef struct {
    float u, v;
    float x, y, z;
} TexVert;

/* =========================================================================
 * Embedded Bitmap Font (8x8, ASCII 32-126, 95 characters)
 *
 * Used as fallback when intraFont fails to load (common in PPSSPP).
 * Each character is 8 bytes; each byte is one row, MSB = leftmost pixel.
 * Texture layout: 16 chars per row, 6 rows → 128x48 pixels (padded to
 * 128x64 power-of-two for GU).
 * ========================================================================= */

/* Classic 8x8 bitmap font — printable ASCII 32..126 (95 glyphs) */
static const u8 s_bmf_data[95][8] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /*   (32) */
    {0x18,0x3C,0x3C,0x18,0x18,0x00,0x18,0x00}, /* ! (33) */
    {0x6C,0x6C,0x24,0x00,0x00,0x00,0x00,0x00}, /* " (34) */
    {0x6C,0x6C,0xFE,0x6C,0xFE,0x6C,0x6C,0x00}, /* # (35) */
    {0x18,0x7E,0xC0,0x7C,0x06,0xFC,0x18,0x00}, /* $ (36) */
    {0x00,0xC6,0xCC,0x18,0x30,0x66,0xC6,0x00}, /* % (37) */
    {0x38,0x6C,0x38,0x76,0xDC,0xCC,0x76,0x00}, /* & (38) */
    {0x18,0x18,0x30,0x00,0x00,0x00,0x00,0x00}, /* ' (39) */
    {0x0C,0x18,0x30,0x30,0x30,0x18,0x0C,0x00}, /* ( (40) */
    {0x30,0x18,0x0C,0x0C,0x0C,0x18,0x30,0x00}, /* ) (41) */
    {0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00}, /* * (42) */
    {0x00,0x18,0x18,0x7E,0x18,0x18,0x00,0x00}, /* + (43) */
    {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x30}, /* , (44) */
    {0x00,0x00,0x00,0x7E,0x00,0x00,0x00,0x00}, /* - (45) */
    {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00}, /* . (46) */
    {0x06,0x0C,0x18,0x30,0x60,0xC0,0x80,0x00}, /* / (47) */
    {0x7C,0xC6,0xCE,0xDE,0xF6,0xE6,0x7C,0x00}, /* 0 (48) */
    {0x18,0x38,0x78,0x18,0x18,0x18,0x7E,0x00}, /* 1 (49) */
    {0x7C,0xC6,0x06,0x1C,0x30,0x66,0xFE,0x00}, /* 2 (50) */
    {0x7C,0xC6,0x06,0x3C,0x06,0xC6,0x7C,0x00}, /* 3 (51) */
    {0x1C,0x3C,0x6C,0xCC,0xFE,0x0C,0x1E,0x00}, /* 4 (52) */
    {0xFE,0xC0,0xFC,0x06,0x06,0xC6,0x7C,0x00}, /* 5 (53) */
    {0x38,0x60,0xC0,0xFC,0xC6,0xC6,0x7C,0x00}, /* 6 (54) */
    {0xFE,0xC6,0x0C,0x18,0x30,0x30,0x30,0x00}, /* 7 (55) */
    {0x7C,0xC6,0xC6,0x7C,0xC6,0xC6,0x7C,0x00}, /* 8 (56) */
    {0x7C,0xC6,0xC6,0x7E,0x06,0x0C,0x78,0x00}, /* 9 (57) */
    {0x00,0x18,0x18,0x00,0x00,0x18,0x18,0x00}, /* : (58) */
    {0x00,0x18,0x18,0x00,0x00,0x18,0x18,0x30}, /* ; (59) */
    {0x0C,0x18,0x30,0x60,0x30,0x18,0x0C,0x00}, /* < (60) */
    {0x00,0x00,0x7E,0x00,0x7E,0x00,0x00,0x00}, /* = (61) */
    {0x60,0x30,0x18,0x0C,0x18,0x30,0x60,0x00}, /* > (62) */
    {0x7C,0xC6,0x0C,0x18,0x18,0x00,0x18,0x00}, /* ? (63) */
    {0x7C,0xC6,0xDE,0xDE,0xDC,0xC0,0x7C,0x00}, /* @ (64) */
    {0x38,0x6C,0xC6,0xC6,0xFE,0xC6,0xC6,0x00}, /* A (65) */
    {0xFC,0x66,0x66,0x7C,0x66,0x66,0xFC,0x00}, /* B (66) */
    {0x3C,0x66,0xC0,0xC0,0xC0,0x66,0x3C,0x00}, /* C (67) */
    {0xF8,0x6C,0x66,0x66,0x66,0x6C,0xF8,0x00}, /* D (68) */
    {0xFE,0x62,0x68,0x78,0x68,0x62,0xFE,0x00}, /* E (69) */
    {0xFE,0x62,0x68,0x78,0x68,0x60,0xF0,0x00}, /* F (70) */
    {0x3C,0x66,0xC0,0xC0,0xCE,0x66,0x3E,0x00}, /* G (71) */
    {0xC6,0xC6,0xC6,0xFE,0xC6,0xC6,0xC6,0x00}, /* H (72) */
    {0x3C,0x18,0x18,0x18,0x18,0x18,0x3C,0x00}, /* I (73) */
    {0x1E,0x0C,0x0C,0x0C,0xCC,0xCC,0x78,0x00}, /* J (74) */
    {0xE6,0x66,0x6C,0x78,0x6C,0x66,0xE6,0x00}, /* K (75) */
    {0xF0,0x60,0x60,0x60,0x62,0x66,0xFE,0x00}, /* L (76) */
    {0xC6,0xEE,0xFE,0xD6,0xC6,0xC6,0xC6,0x00}, /* M (77) */
    {0xC6,0xE6,0xF6,0xDE,0xCE,0xC6,0xC6,0x00}, /* N (78) */
    {0x7C,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00}, /* O (79) */
    {0xFC,0x66,0x66,0x7C,0x60,0x60,0xF0,0x00}, /* P (80) */
    {0x7C,0xC6,0xC6,0xC6,0xD6,0xDE,0x7C,0x06}, /* Q (81) */
    {0xFC,0x66,0x66,0x7C,0x6C,0x66,0xE6,0x00}, /* R (82) */
    {0x7C,0xC6,0xC0,0x7C,0x06,0xC6,0x7C,0x00}, /* S (83) */
    {0x7E,0x5A,0x18,0x18,0x18,0x18,0x3C,0x00}, /* T (84) */
    {0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00}, /* U (85) */
    {0xC6,0xC6,0xC6,0xC6,0x6C,0x38,0x10,0x00}, /* V (86) */
    {0xC6,0xC6,0xC6,0xD6,0xFE,0xEE,0xC6,0x00}, /* W (87) */
    {0xC6,0xC6,0x6C,0x38,0x6C,0xC6,0xC6,0x00}, /* X (88) */
    {0x66,0x66,0x66,0x3C,0x18,0x18,0x3C,0x00}, /* Y (89) */
    {0xFE,0xC6,0x8C,0x18,0x32,0x66,0xFE,0x00}, /* Z (90) */
    {0x3C,0x30,0x30,0x30,0x30,0x30,0x3C,0x00}, /* [ (91) */
    {0xC0,0x60,0x30,0x18,0x0C,0x06,0x02,0x00}, /* \ (92) */
    {0x3C,0x0C,0x0C,0x0C,0x0C,0x0C,0x3C,0x00}, /* ] (93) */
    {0x10,0x38,0x6C,0xC6,0x00,0x00,0x00,0x00}, /* ^ (94) */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF}, /* _ (95) */
    {0x30,0x18,0x0C,0x00,0x00,0x00,0x00,0x00}, /* ` (96) */
    {0x00,0x00,0x78,0x0C,0x7C,0xCC,0x76,0x00}, /* a (97) */
    {0xE0,0x60,0x7C,0x66,0x66,0x66,0xDC,0x00}, /* b (98) */
    {0x00,0x00,0x7C,0xC6,0xC0,0xC6,0x7C,0x00}, /* c (99) */
    {0x1C,0x0C,0x7C,0xCC,0xCC,0xCC,0x76,0x00}, /* d (100)*/
    {0x00,0x00,0x7C,0xC6,0xFE,0xC0,0x7C,0x00}, /* e (101)*/
    {0x1C,0x36,0x30,0x78,0x30,0x30,0x78,0x00}, /* f (102)*/
    {0x00,0x00,0x76,0xCC,0xCC,0x7C,0x0C,0x78}, /* g (103)*/
    {0xE0,0x60,0x6C,0x76,0x66,0x66,0xE6,0x00}, /* h (104)*/
    {0x18,0x00,0x38,0x18,0x18,0x18,0x3C,0x00}, /* i (105)*/
    {0x06,0x00,0x0E,0x06,0x06,0x66,0x66,0x3C}, /* j (106)*/
    {0xE0,0x60,0x66,0x6C,0x78,0x6C,0xE6,0x00}, /* k (107)*/
    {0x38,0x18,0x18,0x18,0x18,0x18,0x3C,0x00}, /* l (108)*/
    {0x00,0x00,0xEC,0xFE,0xD6,0xD6,0xC6,0x00}, /* m (109)*/
    {0x00,0x00,0xDC,0x66,0x66,0x66,0x66,0x00}, /* n (110)*/
    {0x00,0x00,0x7C,0xC6,0xC6,0xC6,0x7C,0x00}, /* o (111)*/
    {0x00,0x00,0xDC,0x66,0x66,0x7C,0x60,0xF0}, /* p (112)*/
    {0x00,0x00,0x76,0xCC,0xCC,0x7C,0x0C,0x1E}, /* q (113)*/
    {0x00,0x00,0xDC,0x76,0x60,0x60,0xF0,0x00}, /* r (114)*/
    {0x00,0x00,0x7C,0xC0,0x7C,0x06,0xFC,0x00}, /* s (115)*/
    {0x30,0x30,0x7C,0x30,0x30,0x36,0x1C,0x00}, /* t (116)*/
    {0x00,0x00,0xCC,0xCC,0xCC,0xCC,0x76,0x00}, /* u (117)*/
    {0x00,0x00,0xC6,0xC6,0x6C,0x38,0x10,0x00}, /* v (118)*/
    {0x00,0x00,0xC6,0xD6,0xFE,0x6C,0x44,0x00}, /* w (119)*/
    {0x00,0x00,0xC6,0x6C,0x38,0x6C,0xC6,0x00}, /* x (120)*/
    {0x00,0x00,0xC6,0xC6,0x6C,0x38,0x30,0x60}, /* y (121)*/
    {0x00,0x00,0xFE,0x8C,0x18,0x32,0xFE,0x00}, /* z (122)*/
    {0x0E,0x18,0x18,0x70,0x18,0x18,0x0E,0x00}, /* { (123)*/
    {0x18,0x18,0x18,0x00,0x18,0x18,0x18,0x00}, /* | (124)*/
    {0x70,0x18,0x18,0x0E,0x18,0x18,0x70,0x00}, /* } (125)*/
    {0x76,0xDC,0x00,0x00,0x00,0x00,0x00,0x00}, /* ~ (126)*/
};

static int s_bmf_ready = 0;

/* Track texture state to avoid redundant enable/disable on real PSP HW */
static int s_tex_enabled = 1;  /* GU starts with texture enabled */

/* Texture state helpers — avoid redundant enable/disable on real PSP HW */
static inline void tex_disable(void)
{
    if (s_tex_enabled) {
        sceGuDisable(GU_TEXTURE_2D);
        s_tex_enabled = 0;
    }
}
static inline void tex_enable(void)
{
    if (!s_tex_enabled) {
        sceGuEnable(GU_TEXTURE_2D);
        s_tex_enabled = 1;
    }
}

static void bmf_init(void)
{
    s_bmf_ready = 1;
}

/*
 * Draw a string using colored GU_SPRITES — one tiny sprite per "on" pixel.
 * This uses the SAME vertex format as ui_draw_rect() (ColorVert) which is
 * proven to work in PPSSPP. No textures involved.
 *
 * For efficiency, each character's pixels are batched into a single
 * sceGuDrawArray call.
 */
static float bmf_draw(float x, float y, u32 color, const char *text, float scale)
{
    if (!s_bmf_ready || !text) return x;

    float pw = scale;          /* pixel width  */
    float ph = scale;          /* pixel height */
    float cx = x;

    tex_disable();

    while (*text) {
        int idx = (unsigned char)*text - 32;
        if (idx >= 0 && idx < 95) {
            /* Count "on" pixels in this glyph */
            int pixel_count = 0;
            int row, col;
            for (row = 0; row < 8; row++) {
                u8 bits = s_bmf_data[idx][row];
                u8 tmp = bits;
                while (tmp) { pixel_count++; tmp &= tmp - 1; }
            }

            if (pixel_count > 0) {
                /* Allocate all sprite vertices for this character at once */
                ColorVert *verts = (ColorVert *)sceGuGetMemory(
                    (unsigned int)(2 * pixel_count) * sizeof(ColorVert));
                if (verts) {
                    int vi = 0;
                    for (row = 0; row < 8; row++) {
                        u8 bits = s_bmf_data[idx][row];
                        for (col = 0; col < 8; col++) {
                            if (bits & (0x80 >> col)) {
                                float px = cx + (float)col * pw;
                                float py = (y - 8.0f * ph) + (float)row * ph;
                                verts[vi].color = color;
                                verts[vi].x     = px;
                                verts[vi].y     = py;
                                verts[vi].z     = 0.0f;
                                vi++;
                                verts[vi].color = color;
                                verts[vi].x     = px + pw;
                                verts[vi].y     = py + ph;
                                verts[vi].z     = 0.0f;
                                vi++;
                            }
                        }
                    }
                    sceGuDrawArray(GU_SPRITES,
                        GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D,
                        vi, NULL, verts);
                }
            }
        }
        cx += 8.0f * pw;
        text++;
    }

    tex_enable();
    return cx;
}

/* =========================================================================
 * Module-private state
 * ========================================================================= */

/* GU command list buffer — must be 4-byte aligned */
static u32 __attribute__((aligned(16))) s_gu_list[GU_LIST_WORDS];

/* Current UI state (state machine) */
static UIState s_state = UI_STATE_BOOT;

/* Input debounce state */
static u32  s_prev_buttons  = 0;
static u64  s_last_evt_tick = 0;   /* sceRtcGetCurrentTick timestamp */
static int  s_repeat_armed  = 0;   /* non-zero once holding fires first evt */

/* Analog stick values (debounced / deadzone-corrected) */
/* Analog stick values (debounced / deadzone-corrected) */
static int s_analog_x = 0;
static int s_analog_y = 0;

/* Spinner animation state (dots cycle through 0-3) */
static int  s_spinner_frame        = 0;
static u64  s_spinner_last_tick    = 0;
#define SPINNER_ADVANCE_US 200000u   /* advance animation every 200 ms */

/* =========================================================================
 * Lifecycle
 * ========================================================================= */

/*
 * --- WHAT THIS DOES ---
 * Initialise the intraFont library and load the PSP's built-in Latin font.
 *
 * ROOT CAUSE OF intraFontLoad() RETURNING NULL:
 *   intraFont 0.31 calls sceGuGetMemory() internally to allocate its glyph
 *   texture atlas. sceGuGetMemory() is ONLY valid between sceGuStart() and
 *   sceGuFinish(). After display_init() closes its command list, no list is
 *   active — so sceGuGetMemory() returns NULL inside intraFontLoad(), which
 *   then returns NULL itself.
 *
 * FIX: Open a temporary GU command list AROUND all intraFontLoad() calls so
 *   that intraFont's internal sceGuGetMemory() has a valid allocation context.
 *   The list is closed immediately after loading — it is otherwise empty.
 */
int ui_manager_init(void)
{
    /* Step 1: Initialise the intraFont library. */
    int init_res = intraFontInit();
    diag_log_write("UI", "intraFontInit=%d\n", init_res);

    /* Step 2: Open a GU command list for intraFontLoad's memalign. */
    sceGuStart(GU_DIRECT, s_gu_list);
    sceGuDisable(GU_DEPTH_TEST);
    sceGuEnable(GU_TEXTURE_2D);
    sceGuEnable(GU_BLEND);
    sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);

    /* Step 3: Load fonts from flash0 */
    if (init_res == 1) {
        s_font_sans  = intraFontLoad("flash0:/font/ltn0.pgf", INTRAFONT_CACHE_ASCII);
        s_font_serif = intraFontLoad("flash0:/font/ltn1.pgf", INTRAFONT_CACHE_ASCII);
        s_font_small = intraFontLoad("flash0:/font/ltn8.pgf", INTRAFONT_CACHE_ASCII);
        if (!s_font_sans) s_font_sans = s_font_small;
    }

    /* Step 4: Close the temporary command list */
    sceGuFinish();
    sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE);

    /* Step 5: Log result */
    diag_log_write("UI", "Fonts: sans=%p serif=%p small=%p\n",
                   (void*)s_font_sans, (void*)s_font_serif, (void*)s_font_small);
    diag_log_flush();

    /* Set default style for all fonts */
    if (s_font_sans)  intraFontSetStyle(s_font_sans,  0.45f, UI_COL_TEXT, 0, INTRAFONT_ALIGN_LEFT);
    if (s_font_serif) intraFontSetStyle(s_font_serif, 0.45f, UI_COL_TEXT, 0, INTRAFONT_ALIGN_LEFT);
    if (s_font_small) intraFontSetStyle(s_font_small, 0.45f, UI_COL_TEXT, 0, INTRAFONT_ALIGN_LEFT);

    /* Always initialise the bitmap font fallback. If intraFont loaded
     * successfully we won't use it, but it costs < 16 KB of RAM. */
    bmf_init();

    s_state = UI_STATE_BOOT;
    return 0;   /* Always succeed — font is optional */
}

/* -------------------------------------------------------------------------
 * ui_manager_shutdown
 * ------------------------------------------------------------------------- */
/*
 * --- WHAT THIS DOES ---
 * Unload the font and shut down the intraFont library.
 * Must be called before sceGuTerm() / display_shutdown().
 */
void ui_manager_shutdown(void)
{
    if (s_font_sans)  intraFontUnload(s_font_sans);
    if (s_font_serif) intraFontUnload(s_font_serif);
    if (s_font_small) intraFontUnload(s_font_small);
    s_font_sans = s_font_serif = s_font_small = NULL;
    intraFontShutdown();
}

/* =========================================================================
 * Frame management
 * ========================================================================= */

/*
 * --- WHAT THIS DOES ---
 * Open a new GU command list and fully reset the 2D rendering state.
 *
 * On real PSP-1000 hardware (unlike PPSSPP) the GU does NOT automatically
 * clear the back buffer between frames.  Without an explicit clear the new
 * frame draws on top of the previous one, causing the "overlapping blocks"
 * and "ghost text" artefacts seen in the config UI.
 *
 * We also re-establish every piece of GU state we depend on here rather
 * than assuming it survived from display_init() or a previous frame.  The
 * real PSP GU state machine is not guaranteed to persist across
 * sceGuSwapBuffers() calls in the way PPSSPP emulates.
 *
 * Order of operations (must match PSP GU spec):
 *   1. sceGuStart  — open command list
 *   2. sceGuClear  — paint back buffer solid black (clears leftover pixels)
 *   3. Re-enable scissor + 2D viewport (lost after swap on real HW)
 *   4. Disable depth test (2D UI has no depth)
 *   5. Reset texture state tracking variable to match actual HW state
 *      (GU_TEXTURE_2D is ON after clear)
 */
void ui_begin_frame(void)
{
    sceGuStart(GU_DIRECT, s_gu_list);

    /* Re-establish essential 2D state every frame — real PSP hardware
     * does not guarantee these survive across buffer swaps. */
    sceGuScissor(0, 0, UI_SCREEN_W, UI_SCREEN_H);
    sceGuEnable(GU_SCISSOR_TEST);

    /* Critical state resets for real hardware UI stability */
    sceGuDisable(GU_DEPTH_TEST);
    sceGuDisable(GU_CULL_FACE);
    sceGuDisable(GU_STENCIL_TEST);
    sceGuDisable(GU_ALPHA_TEST);
    sceGuDisable(GU_TEXTURE_2D);
    sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA);
    sceGuShadeModel(GU_FLAT);

    /* Clear the back buffer to solid black every frame. */
    sceGuClearColor(0xFF000000u);   /* ABGR: opaque black */
    sceGuClear(GU_COLOR_BUFFER_BIT);
}

/*
 * --- WHAT THIS DOES ---
 * Close the GU command list, wait for VBlank, then swap front/back buffers.
 *
 * On real PSP-1000 hardware the correct sequence is:
 *   sceGuFinish → sceGuSync → sceDisplayWaitVblankStart → sceGuSwapBuffers
 *
 * Swapping BEFORE the vblank wait causes a partial frame to become visible
 * mid-scan on real hardware, producing the "flickering" artefact.  PPSSPP
 * is lenient about this ordering; real PSP is not.
 */
void ui_end_frame(void)
{
    sceGuFinish();
    sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE);
    sceDisplayWaitVblankStart();
    sceGuSwapBuffers();
}

void ui_end_frame_no_swap(void)
{
    sceGuFinish();
    sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE);
}

/* =========================================================================
 * State machine
 * ========================================================================= */

void ui_set_state(UIState s)
{
    if (s >= 0 && s < UI_STATE_COUNT)
        s_state = s;
}

UIState ui_get_state(void)
{
    return s_state;
}

/* =========================================================================
 * Input handling
 * ========================================================================= */

int ui_get_analog_x(void) { return s_analog_x; }
int ui_get_analog_y(void) { return s_analog_y; }

/*
 * --- WHAT THIS DOES ---
 * Read the PSP controller using sceCtrlPeekBufferPositive (non-blocking).
 * Apply a dead-zone to the analog stick (ignore values within ±20 of centre
 * 128 to prevent accidental drift inputs). Convert to a ±127 range.
 * Debounce digital buttons: only fire once per press, then hold-repeat
 * after INPUT_DEBOUNCE_US with INPUT_REPEAT_US intervals.
 */
UIEvent ui_process_input(void)
{
    SceCtrlData pad;
    sceCtrlPeekBufferPositive(&pad, 1);
    { extern volatile unsigned int g_remote_buttons;
      pad.Buttons |= g_remote_buttons; g_remote_buttons = 0; }

    /* --- Analog stick with dead-zone --- */
    int ax = (int)pad.Lx - 128;  /* -128 .. +127 */
    int ay = (int)pad.Ly - 128;
#define DEADZONE 20
    s_analog_x = (ax > DEADZONE || ax < -DEADZONE) ? ax : 0;
    s_analog_y = (ay > DEADZONE || ay < -DEADZONE) ? ay : 0;
#undef DEADZONE

    /* --- Digital buttons --- */
    u32 buttons = pad.Buttons;

    /* Log button transitions with high-resolution timestamps for latency analysis */
    if (buttons != s_prev_buttons) {
        diag_log_write("UI", "Button transition: 0x%08X -> 0x%08X (XOR: 0x%08X)",
                       s_prev_buttons, buttons, buttons ^ s_prev_buttons);
    }

    u32 pressed  = buttons & ~s_prev_buttons;  /* Newly pressed this frame */

    /* Get current time for repeat logic */
    u64 now_tick = 0;
    sceRtcGetCurrentTick(&now_tick);

    UIEvent evt = UI_EVT_NONE;

    /* Helper macro: map a PSP button mask to a UIEvent */
#define CHECK_BTN(mask, event) \
    if (pressed & (mask)) { evt = (event); goto done; }

    CHECK_BTN(PSP_CTRL_UP,      UI_EVT_UP)
    CHECK_BTN(PSP_CTRL_DOWN,    UI_EVT_DOWN)
    CHECK_BTN(PSP_CTRL_LEFT,    UI_EVT_LEFT)
    CHECK_BTN(PSP_CTRL_RIGHT,   UI_EVT_RIGHT)
    CHECK_BTN(PSP_CTRL_CROSS,   UI_EVT_SELECT)
    CHECK_BTN(PSP_CTRL_CIRCLE,  UI_EVT_BACK)
    CHECK_BTN(PSP_CTRL_START,   UI_EVT_START)
    CHECK_BTN(PSP_CTRL_SQUARE,  UI_EVT_SCAN)
    CHECK_BTN(PSP_CTRL_SELECT,  UI_EVT_MENU)
    CHECK_BTN(PSP_CTRL_NOTE,    UI_EVT_MENU)   /* Home/Note = same action */

#undef CHECK_BTN

    /* --- Hold-repeat for D-pad navigation --- */
    {
        u32 nav_mask = PSP_CTRL_UP | PSP_CTRL_DOWN |
                       PSP_CTRL_LEFT | PSP_CTRL_RIGHT;
        u32 held = buttons & nav_mask;
        if (held) {
            u32 delta_us = (u32)((now_tick - s_last_evt_tick) * 1000000ULL /
                                  sceRtcGetTickResolution());
            if (!s_repeat_armed && delta_us >= INPUT_DEBOUNCE_US) {
                s_repeat_armed = 1;
                s_last_evt_tick = now_tick;
                /* emit the held direction */
                if (held & PSP_CTRL_UP)    evt = UI_EVT_UP;
                else if (held & PSP_CTRL_DOWN)  evt = UI_EVT_DOWN;
                else if (held & PSP_CTRL_LEFT)  evt = UI_EVT_LEFT;
                else if (held & PSP_CTRL_RIGHT) evt = UI_EVT_RIGHT;
            } else if (s_repeat_armed && delta_us >= INPUT_REPEAT_US) {
                s_last_evt_tick = now_tick;
                if (held & PSP_CTRL_UP)    evt = UI_EVT_UP;
                else if (held & PSP_CTRL_DOWN)  evt = UI_EVT_DOWN;
                else if (held & PSP_CTRL_LEFT)  evt = UI_EVT_LEFT;
                else if (held & PSP_CTRL_RIGHT) evt = UI_EVT_RIGHT;
            }
        } else {
            s_repeat_armed = 0;
        }
    }

done:
    /* Track newly pressed buttons regardless of event fired */
    if (pressed) {
        s_last_evt_tick = now_tick;
        s_repeat_armed  = 0;
    }
    s_prev_buttons = buttons;
    return evt;
}

/* =========================================================================
 * Primitive drawing helpers
 * ========================================================================= */

/*
 * --- WHAT THIS DOES ---
 * Enable or disable GU alpha blending.
 * Use GU_ADD blend mode with standard SRC_ALPHA / ONE_MINUS_SRC_ALPHA
 * to get proper transparency for dark semi-transparent panels and overlays.
 */
void ui_set_blend(int enable)
{
    if (enable) {
        sceGuEnable(GU_BLEND);
        sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
    } else {
        sceGuDisable(GU_BLEND);
    }
}

void ui_set_scissor(int x, int y, int w, int h)
{
    /* sceGuScissor takes (x1, y1, x2, y2) stop-coordinates, not dimensions.
     * Convert (x, y, width, height) → (x, y, x+w, y+h) so the scissor rect
     * correctly spans from (x,y) to (x+w, y+h). */
    sceGuScissor(x, y, x + w, y + h);
    sceGuEnable(GU_SCISSOR_TEST);
}

void ui_clear_scissor(void)
{
    sceGuScissor(0, 0, UI_SCREEN_W, UI_SCREEN_H);
    sceGuEnable(GU_SCISSOR_TEST);
}

/*
 * --- WHAT THIS DOES ---
 * Fill the entire 480x272 screen with one solid colour.
 * This MUST be the first draw call each frame to reset the back buffer.
 * Disables blending to ensure full overwrite.
 */
void ui_clear(u32 color)
{
    ColorVert *v = (ColorVert *)sceGuGetMemory(4 * sizeof(ColorVert));
    if (!v) return;

    tex_disable();
    sceGuDisable(GU_BLEND);

    v[0].color = color; v[0].x = 0.0f;          v[0].y = 0.0f;          v[0].z = 0.0f;
    v[1].color = color; v[1].x = 0.0f;          v[1].y = (float)UI_SCREEN_H; v[1].z = 0.0f;
    v[2].color = color; v[2].x = (float)UI_SCREEN_W; v[2].y = 0.0f;          v[2].z = 0.0f;
    v[3].color = color; v[3].x = (float)UI_SCREEN_W; v[3].y = (float)UI_SCREEN_H; v[3].z = 0.0f;

    sceGuDrawArray(GU_TRIANGLE_STRIP,
                   GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D,
                   4, NULL, v);

    /* Restore texture mode */
    tex_enable();
}

/*
 * --- WHAT THIS DOES ---
 * Draw a gradient background using two horizontal bands.
 * The top 136 px are filled with top_color, the bottom 136 px with
 * bottom_color. No interpolation hardware — just two flat rectangles.
 * This gives a subtle depth illusion typical of XMB-style UIs.
 */
void ui_draw_gradient_bg(u32 top_color, u32 bottom_color)
{
    (void)top_color;
    (void)bottom_color;
    /* Rich gradient: slightly darker version of bg at top, lighter at bottom
     * for a natural depth illusion. Extract ABGR channels and darken/lighten. */
    u32 bg = g_ui_bg_color;
    u8 a = (bg >> 24) & 0xFF;
    u8 b_ch = (bg >> 16) & 0xFF;
    u8 g_ch = (bg >> 8) & 0xFF;
    u8 r_ch = bg & 0xFF;

    /* Top: 20% darker */
    u32 dark_bg = ((u32)a << 24)
               | ((u32)(b_ch * 4 / 5) << 16)
               | ((u32)(g_ch * 4 / 5) << 8)
               | (u32)(r_ch * 4 / 5);
    /* Bottom: 10% lighter (capped at 255) */
    u8 b_l = (u8)((b_ch + 25 > 255) ? 255 : b_ch + 25);
    u8 g_l = (u8)((g_ch + 25 > 255) ? 255 : g_ch + 25);
    u8 r_l = (u8)((r_ch + 25 > 255) ? 255 : r_ch + 25);
    u32 lite_bg = ((u32)a << 24)
               | ((u32)b_l << 16)
               | ((u32)g_l << 8)
               | (u32)r_l;

    /* Draw gradient using 4-vertex strip with per-vertex color */
    tex_disable();
    ColorVert *v = (ColorVert *)sceGuGetMemory(4 * sizeof(ColorVert));
    if (!v) { ui_clear(bg); return; }
    v[0].color = dark_bg; v[0].x = 0.0f;   v[0].y = 0.0f;   v[0].z = 0.0f;
    v[1].color = dark_bg; v[1].x = 480.0f; v[1].y = 0.0f;   v[1].z = 0.0f;
    v[2].color = lite_bg; v[2].x = 0.0f;   v[2].y = 272.0f; v[2].z = 0.0f;
    v[3].color = lite_bg; v[3].x = 480.0f; v[3].y = 272.0f; v[3].z = 0.0f;
    sceGuShadeModel(GU_SMOOTH);
    sceGuDrawArray(GU_TRIANGLE_STRIP,
        GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D,
        4, NULL, v);
    sceGuShadeModel(GU_FLAT);
}

/*
 * --- WHAT THIS DOES ---
 * Draw a single filled rectangle using a GU_SPRITES draw call.
 * GU_SPRITES takes exactly 2 vertices: top-left and bottom-right.
 * Color blending must already be configured via ui_set_blend().
 */
void ui_draw_rect(int x, int y, int w, int h, u32 color)
{
    /* Use 4 vertices for a standard triangle strip (robust across all PSP models) */
    ColorVert *v = (ColorVert *)sceGuGetMemory(4 * sizeof(ColorVert));
    if (!v) return;

    /* Force strict state reset to prevent intraFont leaks from hiding UI elements */
    tex_disable();
    sceGuDisable(GU_DEPTH_TEST);
    sceGuDisable(GU_STENCIL_TEST);
    sceGuDisable(GU_ALPHA_TEST);
    sceGuDisable(GU_CULL_FACE);
    sceGuEnable(GU_BLEND);
    sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
    sceGuShadeModel(GU_FLAT);

    /* Triangle strip order: Top-Left, Bottom-Left, Top-Right, Bottom-Right */
    v[0].color = color; v[0].x = (float)x;     v[0].y = (float)y;     v[0].z = 0.0f;
    v[1].color = color; v[1].x = (float)x;     v[1].y = (float)(y+h); v[1].z = 0.0f;
    v[2].color = color; v[2].x = (float)(x+w); v[2].y = (float)y;     v[2].z = 0.0f;
    v[3].color = color; v[3].x = (float)(x+w); v[3].y = (float)(y+h); v[3].z = 0.0f;

    sceGuDrawArray(GU_TRIANGLE_STRIP,
                   GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D,
                   4, NULL, v);

    /* Restore texture mode for subsequent intraFont calls */
    tex_enable();
}

void ui_draw_rect_batch(int num_rects, const int *rects, u32 color)
{
    if (num_rects <= 0) return;

    ColorVert *v = (ColorVert *)sceGuGetMemory(num_rects * 2 * sizeof(ColorVert));
    if (!v) return;

    for (int i = 0; i < num_rects; i++) {
        v[2*i].color = color;
        v[2*i].x = (float)rects[i*4 + 0];
        v[2*i].y = (float)rects[i*4 + 1];
        v[2*i].z = 0.0f;

        v[2*i+1].color = color;
        v[2*i+1].x = (float)(rects[i*4 + 0] + rects[i*4 + 2]);
        v[2*i+1].y = (float)(rects[i*4 + 1] + rects[i*4 + 3]);
        v[2*i+1].z = 0.0f;
    }

    tex_disable();
    sceGuDisable(GU_DEPTH_TEST);
    sceGuDisable(GU_STENCIL_TEST);
    sceGuDisable(GU_ALPHA_TEST);
    sceGuDisable(GU_CULL_FACE);
    sceGuEnable(GU_BLEND);
    sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
    sceGuShadeModel(GU_FLAT);

    sceGuDrawArray(GU_SPRITES, GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D, num_rects * 2, NULL, v);

    tex_enable();
}

void ui_draw_rect_rounded(int x, int y, int w, int h, int r, u32 color)
{
    if (r <= 0) { ui_draw_rect(x, y, w, h, color); return; }
    if (r > w/2) r = w/2;
    if (r > h/2) r = h/2;

    static int rects[(3 + 4 * 140) * 4];
    int count = 0;

#define ADD_RECT(rx, ry, rw, rh) do { \
        if (count < (3 + 4 * 140)) { \
            rects[count*4+0] = (rx); \
            rects[count*4+1] = (ry); \
            rects[count*4+2] = (rw); \
            rects[count*4+3] = (rh); \
            count++; \
        } \
    } while(0)

    /* Base Body (Floating Glass) */
    ADD_RECT(x + r, y, w - 2*r, h);
    ADD_RECT(x, y + r, r, h - 2*r);
    ADD_RECT(x + w - r, y + r, r, h - 2*r);

    /* Simple, fast hardware-friendly corner plotting */
    for (int i = 0; i < r; i++) {
        int dx = r - 1;
        while (dx * dx + (r - i) * (r - i) > r * r && dx > 0) dx--;

        ADD_RECT(x + r - dx, y + i, dx, 1); /* TL */
        ADD_RECT(x + w - r, y + i, dx, 1); /* TR */
        ADD_RECT(x + r - dx, y + h - 1 - i, dx, 1); /* BL */
        ADD_RECT(x + w - r, y + h - 1 - i, dx, 1); /* BR */
    }

    ui_draw_rect_batch(count, rects, color);

    /* Wii-Style 3D Specular Highlight (Top Inner edge) */
    ui_set_blend(1);
    ui_draw_rect(x + r, y + 1, w - 2*r, 1, 0x40FFFFFFu); // Top edge sparkle
    // Small corner glisten bits
    ui_draw_rect(x + r - 2, y + 2, 2, 1, 0x30FFFFFFu);
    ui_draw_rect(x + w - r, y + 2, 2, 1, 0x30FFFFFFu);

    /* Wii-Style Soft Bottom Shadow (Depth illusion) */
    ui_draw_rect(x + r, y + h - 2, w - 2*r, 1, 0x30000000u); // Bottom edge bevel
    ui_set_blend(0);

#undef ADD_RECT
}

/*
 * --- WHAT THIS DOES ---
 * Draw a hollow border by composing four thin filled rectangles:
 * top edge, bottom edge, left edge, right edge.
 * This avoids the need for GU line primitives which have platform quirks.
 */
void ui_draw_border(int x, int y, int w, int h, int t, u32 color)
{
    /* Enable blending for border colors with alpha */
    ui_set_blend(1);
    /* Top edge */
    ui_draw_rect(x,       y,       w,      t,      color);
    /* Bottom edge */
    ui_draw_rect(x,       y+h-t,   w,      t,      color);
    /* Left edge */
    ui_draw_rect(x,       y+t,     t,      h-2*t,  color);
    /* Right edge */
    ui_draw_rect(x+w-t,   y+t,     t,      h-2*t,  color);
    ui_set_blend(0);
}

/**
 * Draw a hollow rounded border. It draws a thick border of thickness 't'
 * with inner corners perfectly masking an inscribed rectangle.
 */
void ui_draw_hollow_rect_rounded(int x, int y, int w, int h, int r, int t, u32 color)
{
    if (r <= 0 || t <= 0) return;
    ui_set_blend(1);

    /* The 4 straight bezel edges */
    ui_draw_rect(x + r, y, w - 2*r, t, color); /* Top */
    ui_draw_rect(x + r, y + h - t, w - 2*r, t, color); /* Bottom */
    ui_draw_rect(x, y + r, t, h - 2*r, color); /* Left */
    ui_draw_rect(x + w - t, y + r, t, h - 2*r, color); /* Right */

    /* The 4 corner cutouts (drawing the OUTSIDE of the radius r to form the bezel) */
    int rects[4 * 140 * 4];
    int count = 0;

#define ADD_CORNER_RECT(rx, ry, rw, rh) do { \
        if (count < 4 * 140) { \
            rects[count*4+0] = (rx); \
            rects[count*4+1] = (ry); \
            rects[count*4+2] = (rw); \
            rects[count*4+3] = (rh); \
            count++; \
        } \
    } while(0)

    for (int i = 0; i < r; i++) {
        /* Outer circle width at this y-offset */
        int dx_out = r - 1;
        while (dx_out * dx_out + (r - i) * (r - i) > r * r && dx_out > 0) dx_out--;

        /* Inner circle width at this y-offset */
        int dx_in = 0;
        int r_in = r - t;
        if (r_in > 0 && (r - i) <= r_in) {
            dx_in = r_in - 1;
            while (dx_in * dx_in + (r - i) * (r - i) > r_in * r_in && dx_in > 0) dx_in--;
        }

        /* The bezel is the difference between inner and outer circles */
        int start_x = r - dx_out;
        int width   = dx_out - dx_in;

        if (width > 0) {
            ADD_CORNER_RECT(x + start_x, y + i, width, 1); /* TL */
            ADD_CORNER_RECT(x + w - r + dx_in, y + i, width, 1); /* TR */
            ADD_CORNER_RECT(x + start_x, y + h - 1 - i, width, 1); /* BL */
            ADD_CORNER_RECT(x + w - r + dx_in, y + h - 1 - i, width, 1); /* BR */
        }
    }
#undef ADD_CORNER_RECT

    ui_draw_rect_batch(count, rects, color);

    /* Subtle 3D highlight on inner cut-out edge of bezel */
    ui_draw_rect(x + r, y + t - 1, w - 2*r, 1, 0x30000000u); /* Inner dark shadow */

    /* Wii-Style 3D Specular Highlight (Top Outer edge) */
    ui_draw_rect(x + r, y + 1, w - 2*r, 1, 0x40FFFFFFu); /* Top edge sparkle */
    /* Small corner glisten bits */
    ui_draw_rect(x + r - 2, y + 2, 2, 1, 0x30FFFFFFu);
    ui_draw_rect(x + w - r, y + 2, 2, 1, 0x30FFFFFFu);

    /* Wii-Style Soft Bottom Shadow (Depth illusion) */
    ui_draw_rect(x + r, y + h - 2, w - 2*r, 1, 0x30000000u); /* Bottom edge bevel */

    ui_set_blend(0);
}
/* =========================================================================
 * Text rendering (intraFont)
 * ========================================================================= */

/*
 * --- WHAT THIS DOES ---
 * Render a UTF-8/ASCII string using the PSP internal PGF Latin font.
 * intraFont requires GU texture mode to be active; it manages its own
 * internal texture state. We restore GU_TEXTURE_2D after the call.
 */
float ui_draw_text(float x, float y, u32 color, const char *text)
{
    if (!text) return x;
    if (!s_font) {
        static int logged = 0;
        if (!logged) {
#ifndef RETAIL_BUILD
            moonlight_storage_ensure_data_dir();
            SceUID fd = sceIoOpen(MOONLIGHT_SAVE_DEBUG_LOG_PATH,
                                  PSP_O_WRONLY | PSP_O_APPEND | PSP_O_CREAT, 0644);
            if (fd >= 0) {
                const char *msg = "[CRITICAL] s_font is NULL in ui_draw_text — falling back to BMF\n";
                sceIoWrite(fd, msg, strlen(msg));
                sceIoClose(fd);
            }
#endif
            logged = 1; /* Only log once to avoid flooding Log */
        }
        return bmf_draw(x, y, color, text, 1.0f);
    }
    float scale = (s_font == s_font_serif) ? 0.50f : 0.45f;
    sceGuEnable(GU_BLEND);
    sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
    intraFontSetStyle(s_font, scale, color, 0, INTRAFONT_ALIGN_LEFT);
    intraFontActivate(s_font);
    float nx = intraFontPrint(s_font, x, y, text);
    sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA);
    sceGuDisable(GU_BLEND);
    return nx;
}

/*
 * --- WHAT THIS DOES ---
 * Centre text within a horizontal band [cx .. cx+cw].
 */
void ui_draw_text_centered(float cx, float cw, float y, u32 color, const char *text)
{
    if (!text) return;
    float scale = (s_font == s_font_serif) ? 0.55f : 0.48f;
    sceGuEnable(GU_BLEND);
    sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
    intraFontSetStyle(s_font, scale, color, 0, INTRAFONT_ALIGN_LEFT);
    intraFontActivate(s_font);
    float text_w = intraFontMeasureText(s_font, text);
    float tx = cx + (cw - text_w) * 0.5f;
    if (tx < cx) tx = cx;
    intraFontPrint(s_font, tx, y, text);
    sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA);
    return;
}

float ui_draw_text_right(float right_margin_x, float y, u32 color, const char *text)
{
    if (!text) return right_margin_x;
    if (!s_font) return right_margin_x;

    sceGuEnable(GU_BLEND);
    sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
    intraFontSetStyle(s_font, 0.48f, color, 0, INTRAFONT_ALIGN_LEFT); /* Match header font size */
    intraFontActivate(s_font);
    float text_w = intraFontMeasureText(s_font, text);
    float tx = right_margin_x - text_w;
    float nx = intraFontPrint(s_font, tx, y, text);
    sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA);
    sceGuDisable(GU_BLEND);
    intraFontSetStyle(s_font, 0.45f, UI_COL_TEXT, 0, INTRAFONT_ALIGN_LEFT);
    return nx;
}

void ui_draw_text_medium_centered(float cx, float cw, float y, u32 color, const char *text)
{
    if (!text) return;
    float scale = (s_font == s_font_serif) ? 0.48f : 0.40f;
    sceGuEnable(GU_BLEND);
    sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
    intraFontSetStyle(s_font, scale, color, 0, INTRAFONT_ALIGN_LEFT);
    intraFontActivate(s_font);
    float text_w = intraFontMeasureText(s_font, text);
    float tx = cx + (cw - text_w) * 0.5f;
    if (tx < cx) tx = cx;
    intraFontPrint(s_font, tx, y, text);
    sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA);
    sceGuDisable(GU_BLEND);
    return;
}

float ui_draw_text_large(float x, float y, u32 color, const char *text)
{
    if (!text) return x;
    if (!s_font)
        return bmf_draw(x, y, color, text, 1.5f);
    sceGuEnable(GU_BLEND);
    sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
    intraFontSetStyle(s_font, 0.80f, color, 0, INTRAFONT_ALIGN_LEFT);
    intraFontActivate(s_font);
    float nx = intraFontPrint(s_font, x, y, text);
    sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA);
    sceGuDisable(GU_BLEND);
    /* Restore default style */
    intraFontSetStyle(s_font, 0.45f, UI_COL_TEXT, 0, INTRAFONT_ALIGN_LEFT);
    return nx;
}

float ui_draw_text_small(float x, float y, u32 color, const char *text)
{
    if (!text) return x;
    if (!s_font)
        return bmf_draw(x, y, color, text, 1.0f);
    sceGuEnable(GU_BLEND);
    sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
    intraFontSetStyle(s_font, 0.35f, color, 0, INTRAFONT_ALIGN_LEFT);
    intraFontActivate(s_font);
    float nx = intraFontPrint(s_font, x, y, text);
    sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA);
    sceGuDisable(GU_BLEND);
    intraFontSetStyle(s_font, 0.45f, UI_COL_TEXT, 0, INTRAFONT_ALIGN_LEFT);
    return nx;
}

float ui_draw_text_scaled(float x, float y, u32 color, const char *text, float scale)
{
    if (!text) return x;
    if (!s_font)
        return bmf_draw(x, y, color, text, scale / 0.45f);
    sceGuEnable(GU_BLEND);
    sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
    intraFontSetStyle(s_font, scale, color, 0, INTRAFONT_ALIGN_LEFT);
    intraFontActivate(s_font);
    float nx = intraFontPrint(s_font, x, y, text);
    sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA);
    sceGuDisable(GU_BLEND);
    intraFontSetStyle(s_font, 0.45f, UI_COL_TEXT, 0, INTRAFONT_ALIGN_LEFT);
    return nx;
}

float ui_draw_text_medium(float x, float y, u32 color, const char *text)
{
    if (!text) return x;
    if (!s_font)
        return bmf_draw(x, y, color, text, 1.2f);
    sceGuEnable(GU_BLEND);
    sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
    intraFontSetStyle(s_font, 0.41f, color, 0, INTRAFONT_ALIGN_LEFT);
    intraFontActivate(s_font);
    float nx = intraFontPrint(s_font, x, y, text);
    sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA);
    sceGuDisable(GU_BLEND);
    intraFontSetStyle(s_font, 0.45f, UI_COL_TEXT, 0, INTRAFONT_ALIGN_LEFT);
    return nx;
}

/*
 * ui_draw_pin_large - Render a short string (typically 4 digits) at 2×
 * display scale, horizontally centred at center_x.
 *
 * WHY THIS EXISTS:
 *   The 7-segment rect approach is unreliable on real PSP-1000 hardware:
 *   tex_enable/disable toggling inside ui_draw_rect leaves the GE in an
 *   ambiguous state when the font atlas is still bound.  Using intraFont at
 *   a large scale is guaranteed-visible because it uses the exact same blend
 *   guard path that we KNOW works for all other on-screen text.
 *
 *   INTRAFONT_ALIGN_CENTER makes intraFont treat center_x as the horizontal
 *   mid-point of the rendered string, so no manual centering arithmetic is
 *   required.
 */
void ui_draw_pin_large(int center_x, int baseline_y, u32 color, const char *text)
{
    if (!text) return;
    if (s_font) {
        sceGuEnable(GU_BLEND);
        sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
        intraFontSetStyle(s_font, 2.0f, color, 0, INTRAFONT_ALIGN_CENTER);
        intraFontActivate(s_font);
        intraFontPrint(s_font, (float)center_x, (float)baseline_y, text);
        sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA);
        sceGuDisable(GU_BLEND);
        /* Restore default style so subsequent text calls are unaffected */
        intraFontSetStyle(s_font, 0.45f, UI_COL_TEXT, 0, INTRAFONT_ALIGN_LEFT);
    } else {
        /* Bitmap-font fallback: 3× scale, manually centred */
        float w = (float)(int)strlen(text) * 8.0f * 3.0f;
        bmf_draw((float)center_x - w * 0.5f, (float)baseline_y, color, text, 3.0f);
    }
}

/* =========================================================================
 * Composite UI components
 * ========================================================================= */

/*
 * --- WHAT THIS DOES ---
 * Draw the top header bar: a 28 px tall dark panel across the full width
 * with a 2 px accent-colour underline and the section title left-aligned.
 * This creates the XMB-style branding strip at pixel y=0..27.
 */
void ui_draw_header(const char *title)
{
    /* Drop shadow beneath the header for depth */
    ui_set_blend(1);
    ui_draw_rect_rounded(6 + 2, 6 + 2, UI_SCREEN_W - 12, 24, 10, 0x20000000u);
    ui_draw_rect_rounded(6 + 1, 6 + 1, UI_SCREEN_W - 12, 24, 10, 0x30000000u);
    /* Wii-style Floating Header Panel */
    ui_draw_rect_rounded(6, 6, UI_SCREEN_W - 12, 24, 10, 0xCC181818u);
    ui_set_blend(0);

    if (title) {
        ui_draw_text(18.0f, 22.0f, UI_COL_TEXT, title);
    }
}

/*
 * --- WHAT THIS DOES ---
 * Draw the footer bar at the bottom of the 480x272 screen (y=252..271).
 * Shows a semi-transparent band with button-hint text that supports
 * embedded PSP button bitmap sprites via {X}, {O}, {SQ}, {TR}, {L}, {R},
 * {ST}, {SE}, {UP}, {DN}, {LF}, {RF}, {DP}, {AN}, {AU}, {AD}, {AL}, {AR}
 * tokens.  Unrecognised tokens are passed through as plain text.
 */

/* Badge button-type indices — 16x16 RGBA textures from btn_icons.h (Xelu CC0) */
#define BTN_BADGE_X    0   /* Cross     (PS Vita)         */
#define BTN_BADGE_O    1   /* Circle    (PS Vita)         */
#define BTN_BADGE_SQ   2   /* Square    (PS Vita)         */
#define BTN_BADGE_TR   3   /* Triangle  (PS Vita)         */
#define BTN_BADGE_L    4   /* L bumper  (PS Vita)         */
#define BTN_BADGE_R    5   /* R bumper  (PS Vita)         */
#define BTN_BADGE_ST   6   /* Start     (PS Move)         */
#define BTN_BADGE_SE   7   /* Select    (PS Move)         */
#define BTN_BADGE_UP   8   /* D-pad up  (PS3)             */
#define BTN_BADGE_DN   9   /* D-pad down (PS3)            */
#define BTN_BADGE_LF   10  /* D-pad left (PS3)            */
#define BTN_BADGE_RF   11  /* D-pad right (PS3)           */
#define BTN_BADGE_DP   12  /* D-pad full  (PS3)           */
#define BTN_BADGE_AN   13  /* Analog nub  (Oculus Remote) */
#define BTN_BADGE_AUP  14  /* Analog up                   */
#define BTN_BADGE_ADN  15  /* Analog down                 */
#define BTN_BADGE_ALF  16  /* Analog left                 */
#define BTN_BADGE_ARF  17  /* Analog right                */
#define BTN_BADGE_CNT  18

/*
 * draw_btn_badge — render one PSP button icon as a 16x16 textured sprite.
 * Returns x immediately after the icon (x + 16 + 2) for text chaining.
 * Source textures are embedded in btn_icons.h (Xelu CC0 button prompt pack).
 */
static float draw_btn_badge(float x, float y, int btn)
{
    if (btn < 0 || btn >= BTN_BADGE_CNT) return x;

    const unsigned char *tex = s_btn_textures[btn];
    /* Start/Select: render wider (24x14) to reflect oval pill shape */
    int render_w = 16;
    int render_h = 16;
    int bx = (int)x + 1;                              /* 1px left margin */
    int by = (int)(y - (float)render_h * 0.74f);      /* icon position relative to text baseline */

    sceGuEnable(GU_BLEND);
    sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
    sceGuTexMode(GU_PSM_8888, 0, 0, 0);
    sceGuTexImage(0, 16, 16, 16, tex);
    sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA);
    sceGuTexFilter(GU_LINEAR, GU_LINEAR);
    sceGuTexWrap(GU_CLAMP, GU_CLAMP);
    sceGuTexScale(1.0f, 1.0f);
    sceGuTexOffset(0.0f, 0.0f);
    sceGuEnable(GU_TEXTURE_2D);

    typedef struct { float u, v, x, y, z; } BtnV;
    BtnV *v = (BtnV *)sceGuGetMemory(2 * sizeof(BtnV));
    if (v) {
        v[0].u = 0.0f;  v[0].v = 0.0f;
        v[0].x = (float)bx;              v[0].y = (float)by;              v[0].z = 0.0f;
        v[1].u = 16.0f; v[1].v = 16.0f;
        v[1].x = (float)(bx + render_w); v[1].y = (float)(by + render_h); v[1].z = 0.0f;
        sceGuDrawArray(GU_SPRITES,
                       GU_TEXTURE_32BITF | GU_VERTEX_32BITF | GU_TRANSFORM_2D,
                       2, NULL, v);
    }
    sceGuDisable(GU_TEXTURE_2D);
    return x + (float)render_w + 3.0f;  /* 1px left + icon + 2px right gap */
}

void ui_draw_footer_hint(const char *hint_text)
{
    /* Drop shadow beneath footer for depth */
    ui_set_blend(1);
    ui_draw_rect_rounded(6 + 2, 244 + 2, UI_SCREEN_W - 12, 22, 10, 0x20000000u);
    ui_draw_rect_rounded(6 + 1, 244 + 1, UI_SCREEN_W - 12, 22, 10, 0x30000000u);
    /* Modern Floating Glass Footer (Wii Rounded) */
    ui_draw_rect_rounded(6, 244, UI_SCREEN_W - 12, 22, 10, 0xCC181818u);
    ui_set_blend(0);

    if (!hint_text) return;

    float px = 18.0f;
    float py = 259.0f;
    const char *p = hint_text;
    char seg[80];

    while (*p) {
        if (*p == '{') {
            const char *end = p + 1;
            while (*end && *end != '}') ++end;
            if (*end == '}') {
                char tok[8];
                int tlen = (int)(end - p - 1);
                if (tlen > 0 && tlen < (int)sizeof(tok)) {
                    int k;
                    for (k = 0; k < tlen; ++k) tok[k] = p[1 + k];
                    tok[tlen] = '\0';
                    int btn = -1;
                    if (tok[0]=='X'  && !tok[1])             btn = BTN_BADGE_X;
                    else if (tok[0]=='O'  && !tok[1])        btn = BTN_BADGE_O;
                    else if (tok[0]=='S' && tok[1]=='Q' && !tok[2]) btn = BTN_BADGE_SQ;
                    else if (tok[0]=='T' && tok[1]=='R' && !tok[2]) btn = BTN_BADGE_TR;
                    else if (tok[0]=='L'  && !tok[1])        btn = BTN_BADGE_L;
                    else if (tok[0]=='R'  && !tok[1])        btn = BTN_BADGE_R;
                    else if (tok[0]=='S' && tok[1]=='T' && !tok[2]) btn = BTN_BADGE_ST;
                    else if (tok[0]=='S' && tok[1]=='E' && !tok[2]) btn = BTN_BADGE_SE;
                    else if (tok[0]=='U' && tok[1]=='P' && !tok[2]) btn = BTN_BADGE_UP;
                    else if (tok[0]=='D' && tok[1]=='N' && !tok[2]) btn = BTN_BADGE_DN;
                    else if (tok[0]=='L' && tok[1]=='F' && !tok[2]) btn = BTN_BADGE_LF;
                    else if (tok[0]=='R' && tok[1]=='F' && !tok[2]) btn = BTN_BADGE_RF;
                    else if (tok[0]=='D' && tok[1]=='P' && !tok[2]) btn = BTN_BADGE_DP;
                    else if (tok[0]=='A' && tok[1]=='N' && !tok[2]) btn = BTN_BADGE_AN;
                    else if (tok[0]=='A' && tok[1]=='U' && !tok[2]) btn = BTN_BADGE_AUP;
                    else if (tok[0]=='A' && tok[1]=='D' && !tok[2]) btn = BTN_BADGE_ADN;
                    else if (tok[0]=='A' && tok[1]=='L' && !tok[2]) btn = BTN_BADGE_ALF;
                    else if (tok[0]=='A' && tok[1]=='R' && !tok[2]) btn = BTN_BADGE_ARF;
                    if (btn >= 0) {
                        px = draw_btn_badge(px, py, btn);
                        p = end + 1;
                        continue;
                    }
                }
            }
        }
        /* Copy up to next '{' or end of string */
        const char *next = p;
        while (*next && *next != '{') ++next;
        int seglen = (int)(next - p);
        if (seglen > 0) {
            if (seglen >= (int)sizeof(seg)) seglen = (int)sizeof(seg) - 1;
            int j;
            for (j = 0; j < seglen; ++j) seg[j] = p[j];
            seg[seglen] = '\0';
            /* Footer text: 0.42f (one step larger than medium) and
             * no upward offset so text sits slightly lower than icons */
            sceGuEnable(GU_BLEND);
            sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
            intraFontSetStyle(s_font, 0.42f, UI_COL_TEXT_DIM, 0, INTRAFONT_ALIGN_LEFT);
            intraFontActivate(s_font);
            px = intraFontPrint(s_font, px, py, seg);
            sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA);
            sceGuDisable(GU_BLEND);
            intraFontSetStyle(s_font, 0.45f, UI_COL_TEXT, 0, INTRAFONT_ALIGN_LEFT);
        }
        p = next;
    }
}

float ui_draw_text_inline(float x, float y, u32 col, const char *text)
{
    if (!text) return x;
    float px = x;
    const char *p = text;
    char seg[80];

    while (*p) {
        if (*p == '{') {
            const char *end = p + 1;
            while (*end && *end != '}') ++end;
            if (*end == '}') {
                char tok[8];
                int tlen = (int)(end - p - 1);
                if (tlen > 0 && tlen < (int)sizeof(tok)) {
                    int k;
                    for (k = 0; k < tlen; ++k) tok[k] = p[1 + k];
                    tok[tlen] = '\0';
                    int btn = -1;
                    if (tok[0]=='X'  && !tok[1])             btn = BTN_BADGE_X;
                    else if (tok[0]=='O'  && !tok[1])        btn = BTN_BADGE_O;
                    else if (tok[0]=='S' && tok[1]=='Q' && !tok[2]) btn = BTN_BADGE_SQ;
                    else if (tok[0]=='T' && tok[1]=='R' && !tok[2]) btn = BTN_BADGE_TR;
                    else if (tok[0]=='L'  && !tok[1])        btn = BTN_BADGE_L;
                    else if (tok[0]=='R'  && !tok[1])        btn = BTN_BADGE_R;
                    else if (tok[0]=='S' && tok[1]=='T' && !tok[2]) btn = BTN_BADGE_ST;
                    else if (tok[0]=='S' && tok[1]=='E' && !tok[2]) btn = BTN_BADGE_SE;
                    else if (tok[0]=='U' && tok[1]=='P' && !tok[2]) btn = BTN_BADGE_UP;
                    else if (tok[0]=='D' && tok[1]=='N' && !tok[2]) btn = BTN_BADGE_DN;
                    else if (tok[0]=='L' && tok[1]=='F' && !tok[2]) btn = BTN_BADGE_LF;
                    else if (tok[0]=='R' && tok[1]=='F' && !tok[2]) btn = BTN_BADGE_RF;
                    else if (tok[0]=='D' && tok[1]=='P' && !tok[2]) btn = BTN_BADGE_DP;
                    else if (tok[0]=='A' && tok[1]=='N' && !tok[2]) btn = BTN_BADGE_AN;
                    else if (tok[0]=='A' && tok[1]=='U' && !tok[2]) btn = BTN_BADGE_AUP;
                    else if (tok[0]=='A' && tok[1]=='D' && !tok[2]) btn = BTN_BADGE_ADN;
                    else if (tok[0]=='A' && tok[1]=='L' && !tok[2]) btn = BTN_BADGE_ALF;
                    else if (tok[0]=='A' && tok[1]=='R' && !tok[2]) btn = BTN_BADGE_ARF;
                    if (btn >= 0) {
                        px = draw_btn_badge(px, y, btn);
                        p = end + 1;
                        continue;
                    }
                }
            }
        }
        const char *next = p;
        while (*next && *next != '{') ++next;
        int seglen = (int)(next - p);
        if (seglen > 0) {
            if (seglen >= (int)sizeof(seg)) seglen = (int)sizeof(seg) - 1;
            int j;
            for (j = 0; j < seglen; ++j) seg[j] = p[j];
            seg[seglen] = '\0';
            px = ui_draw_text_medium(px, y, col, seg);
        }
        p = next;
    }
    return px;
}

/*
 * --- WHAT THIS DOES ---
 * Draw a background panel (card) with an optional title.
 * The panel uses alpha blending so it sits over a background image or
 * gradient without fully obscuring it.
 */
void ui_draw_panel(const UiPanel *p)
{
    if (!p) return;

    /* Draw panel background (border + fill) */
    ui_set_blend(1);
    ui_draw_rect_rounded(p->x, p->y, p->w, p->h, 8, p->border_color);
    ui_draw_rect_rounded(p->x + 1, p->y + 1, p->w - 2, p->h - 2, 7, p->bg_color);
    ui_set_blend(0);

    /* Optional title */
    if (p->title[0]) {
        ui_draw_text_small((float)(p->x + 4), (float)(p->y + 11),
                           UI_COL_TEXT_DIM, p->title);
    }
}

/*
 * --- WHAT THIS DOES ---
 * Draw a button rectangle with the label centred inside.
 * Focused buttons get: bright white border (2 px), lighter background,
 * and yellow-tinted text to visually separate them from idle buttons.
 * This mirrors the "XMB selection highlight" without requiring sprites.
 */
void ui_draw_button(const UiButton *b)
{
    if (!b) return;

    u32 bg  = b->focused ? UI_COL_CARD_SEL   : UI_COL_CARD;
    u32 brd = b->focused ? UI_COL_BORDER_FOC : UI_COL_BORDER;
    u32 tc  = b->focused ? UI_COL_TEXT_FOCUS  : UI_COL_TEXT;

    /* === Drop Shadow (3-layer, grows +2px when focused for hover effect) === */
    ui_set_blend(1);
    {
        int so = b->focused ? 1 : 0;
        /* Layer 1: outermost faint shadow */
        ui_draw_rect_rounded(b->x + 3 + so, b->y + 3 + so, b->w, b->h, 12, 0x18000000u);
        /* Layer 2: middle shadow */
        ui_draw_rect_rounded(b->x + 2 + so, b->y + 2 + so, b->w, b->h, 12, 0x28000000u);
        /* Layer 3: inner shadow */
        ui_draw_rect_rounded(b->x + 1 + so, b->y + 1 + so, b->w, b->h, 12, 0x38000000u);
    }

    /* Card panel with border */
    int t = b->focused ? 2 : 1;
    ui_draw_rect_rounded(b->x, b->y, b->w, b->h, 12, brd);
    ui_draw_rect_rounded(b->x + t, b->y + t, b->w - 2*t, b->h - 2*t, 12 - t, bg);
    ui_set_blend(0);

    /* Centred label */
    ui_draw_text_centered((float)b->x, (float)b->w,
                          (float)(b->y + b->h / 2 + 4),
                          tc, b->label);
}

/*
 * --- WHAT THIS DOES ---
 * Draw a full-screen error notification with a dark overlay + centred box.
 * Used whenever a fatal or recoverable error needs user attention
 * (e.g. connection timeout, pairing failure).
 *
 * Layout:
 *   Full-screen dark overlay (translucent)
 *   Centred box 300x100:
 *     Red-bordered panel
 *     "ERROR" label in red at top
 *     Title in white
 *     Message in dimmed text
 *     Hint in small dim text at bottom
 */
void ui_draw_error_modal(const char *title, const char *message,
                         const char *hint)
{
    /* Semi-transparent dark overlay over everything */
    ui_set_blend(1);
    ui_draw_rect(0, 0, UI_SCREEN_W, UI_SCREEN_H, UI_COL_ERR_OVERLAY);
    ui_set_blend(0);

    /* Error box dimensions: 300 wide, 120 tall, centred */
    int bw = 300, bh = 120;
    int bx = (UI_SCREEN_W - bw) / 2;
    int by = (UI_SCREEN_H - bh) / 2;

    /* Wii-style Rounded Error Box */
    ui_draw_rect_rounded(bx, by, bw, bh, 12, UI_COL_ERR_PANEL);

    /* Red border (2 px) */
    ui_draw_hollow_rect_rounded(bx, by, bw, bh, 12, 2, UI_COL_ERR_BORDER);

    /* "ERROR" label in red */
    ui_draw_text_centered((float)bx, (float)bw,
                          (float)(by + 16), UI_COL_ERR_TITLE, "! ERROR !");

    /* Title in white */
    if (title)
        ui_draw_text_centered((float)bx, (float)bw,
                              (float)(by + 40), UI_COL_TEXT, title);

    /* Message in dim grey */
    if (message)
        ui_draw_text_small((float)(bx + 10), (float)(by + 62),
                           UI_COL_TEXT_DIM, message);

    /* Button hint in small dim text */
    if (hint)
        ui_draw_text_small((float)(bx + 10), (float)(by + 110),
                           UI_COL_TEXT_DIM, hint);
}

/*
 * --- WHAT THIS DOES ---
 * Draw a horizontal progress bar with optional right-side label.
 * The bar is split into an unfilled background and a filled foreground
 * clamped to [0 .. w] based on value/max ratio.
 */
void ui_draw_progress_bar(int x, int y, int w, int h,
                          float value, float max, const char *label)
{
    int t = 1;
    ui_draw_rect_rounded(x, y, w, h, h/2, UI_COL_BORDER);
    ui_draw_rect_rounded(x + t, y + t, w - 2*t, h - 2*t, (h - 2*t)/2, UI_COL_PANEL);

    /* Clamp fill - use float for pixel-perfect sub-pixel width */
    if (max > 0.0f) {
        float fval = (value < 0.0f) ? 0.0f : (value > max ? max : value);
        float ratio = fval / max;
        int fw = (int)(ratio * (float)w);

        if (fw > 2) {
            /* Draw rounded fill: width must be at least h for proper rounding,
               but we can draw a clipped version for small progresses. */
            ui_draw_rect_rounded(x + 1, y + 1, fw - 2, h - 2, (h-2)/2, UI_COL_ACCENT);
        }
    }

    /* Optional label to the right */
    if (label)
        ui_draw_text_small((float)(x + w + 6), (float)(y + h - 3),
                           UI_COL_TEXT_DIM, label);
}

/*
 * --- WHAT THIS DOES ---
 * Animate a row of 4 dots to indicate background activity.
 * The active dot (bright white) advances left-to-right every
 * SPINNER_ADVANCE_US microseconds using the RTC tick counter.
 */
void ui_draw_spinner(int x, int y, const char *label)
{
    static const char *dot_chars[4] = { "●···", "·●··", "··●·", "···●" };

    u64 now = 0;
    sceRtcGetCurrentTick(&now);
    u32 delta_us = (u32)((now - s_spinner_last_tick) * 1000000ULL /
                          sceRtcGetTickResolution());
    if (delta_us >= SPINNER_ADVANCE_US) {
        s_spinner_frame = (s_spinner_frame + 1) & 3;
        s_spinner_last_tick = now;
    }

    /* If x is negative, center everything horizontally */
    float dx = (float)x;
    float dw = 32.0f; /* Fallback width for dots */
    float lw = 0.0f;
    float scale = (s_font == s_font_serif) ? 0.50f : 0.45f;

    if (s_font) {
        intraFontSetStyle(s_font, scale, UI_COL_TEXT_DIM, 0, INTRAFONT_ALIGN_LEFT);
        dw = intraFontMeasureText(s_font, dot_chars[s_spinner_frame]);
        if (label) lw = intraFontMeasureText(s_font, label);
    } else {
        if (label) lw = (float)(strlen(label) * 8);
    }

    float total_w = dw + (label ? 8.0f : 0.0f) + lw;
    if (x < 0) {
        dx = (UI_SCREEN_W - total_w) / 2.0f;
    }

    /* Vertical centering: intraFontPrint draws at baseline, so we adjust y */
    ui_draw_text(dx, (float)y, UI_COL_ACCENT, dot_chars[s_spinner_frame]);

    if (label)
        ui_draw_text((float)(dx + dw + 8.0f), (float)y,
                     UI_COL_TEXT_DIM, label);
}

/*
 * --- WHAT THIS DOES ---
 * Render a placeholder game card when artwork is not yet available.
 * Draws a dark card with a centred game title and a subtle dashed border
 * to communicate "placeholder" visually. If focused, the border becomes
 * bright white to show selection.
 */
void ui_draw_texture_placeholder(int x, int y, int w, int h,
                                  const char *title, int focused)
{
    u32 bg  = focused ? UI_COL_CARD_SEL : UI_COL_CARD;
    u32 brd = focused ? UI_COL_BORDER_FOC : UI_COL_BORDER;
    u32 tc  = focused ? UI_COL_TEXT_FOCUS  : UI_COL_TEXT_DIM;
    int is_desktop = 0;

    int t = focused ? 2 : 1;
    ui_draw_rect_rounded(x, y, w, h, 12, brd); /* Outer border */
    ui_draw_rect_rounded(x + t, y + t, w - 2*t, h - 2*t, 12 - t, bg); /* Inner body */

    if (title && title[0]) {
        const char *d = "desktop";
        int k;
        is_desktop = 1;
        for (k = 0; d[k] != '\0'; k++) {
            char a = title[k];
            if (a >= 'A' && a <= 'Z')
                a = (char)(a + 32);
            if (a != d[k]) {
                is_desktop = 0;
                break;
            }
        }
        if (title[k] != '\0')
            is_desktop = 0;
    }

    if (is_desktop) {
        int mon_w = (w * 58) / 100;
        int mon_h = (h * 28) / 100;
        int mon_x = x + (w - mon_w) / 2;
        int mon_y = y + h / 2 - mon_h / 2 - 10;
        int stand_w = mon_w / 7;
        int stand_h = 12;
        int stand_x = x + w / 2 - stand_w / 2;
        int stand_y = mon_y + mon_h + 3;
        int base_w = mon_w / 2;
        int base_x = x + w / 2 - base_w / 2;

        ui_draw_rect_rounded(mon_x, mon_y, mon_w, mon_h, 5, brd);
        ui_draw_rect_rounded(mon_x + 3, mon_y + 3, mon_w - 6, mon_h - 6, 3, 0xFF182321u);
        ui_draw_rect(mon_x + 8, mon_y + 9, mon_w - 16, 2, UI_COL_ACCENT);
        ui_draw_rect(mon_x + 8, mon_y + 16, (mon_w - 20) / 2, 2, UI_COL_TEXT_DIM);
        ui_draw_rect(mon_x + mon_w / 2 + 2, mon_y + 16, (mon_w - 20) / 2, 2, UI_COL_TEXT_DIM);
        ui_draw_rect_rounded(stand_x, stand_y, stand_w, stand_h, 2, brd);
        ui_draw_rect_rounded(base_x, stand_y + stand_h - 1, base_w, 5, 2, brd);
    } else {
        /* Centered fallback glyph for non-desktop placeholders. */
        int icon_x = x + w/2 - 8;
        int icon_y = y + h/2 - 10;
        ui_draw_rect_rounded(icon_x,     icon_y,     16, 10, 4, UI_COL_BORDER);
        ui_draw_rect_rounded(icon_x + 4, icon_y - 3,  4, 4,  2, UI_COL_BORDER);
    }

    /* Game title below the icon */
    if (title && title[0]) {
        /* Truncate title if too long for the card width */
        char buf[24];
        int max_chars = (w - 8) / FONT_AVG_CHAR_W;
        if (max_chars < 1) max_chars = 1;
        if (max_chars > 23) max_chars = 23;
        strncpy(buf, title, (size_t)max_chars);
        buf[max_chars] = '\0';

        ui_draw_text_small(
            (float)(x + w/2 - (int)(strlen(buf) * FONT_AVG_CHAR_W * 0.35f * 0.5f)),
            (float)(y + h - 10),
            tc, buf);
    }
}

/*
 * --- WHAT THIS DOES ---
 * Blit a preloaded RGB565 texture using the GU hardware blitter.
 * RGB565 uses 2 bytes per pixel (vs 4 for RGBA8888), saving VRAM.
 * tex_w and tex_h must be powers-of-two as required by the GU texture unit.
 *
 * The source icon data is packed at w-pixel stride, but the GU needs the
 * texture buffer width to match tex_w (a power-of-two >= w).  We copy into
 * a static staging buffer with correct stride, flush the data cache, then
 * upload.  UV coords sample only the actual (w x h) region.
 */

void ui_draw_texture_rgb565(int x, int y, int dw, int dh,
                             void *tex_data, int sw, int sh,
                             int tw, int th, int focused)
{
    if (!tex_data) {
        ui_draw_texture_placeholder(x, y, dw, dh, NULL, focused);
        return;
    }

    /* Use the pre-packed tw x th buffer directly to avoid out-of-memory in the GE display list */
    unsigned int tex_size = (unsigned int)tw * (unsigned int)th * sizeof(unsigned short);
    unsigned short *vram_buf = (unsigned short *)tex_data;

    /* Flush data cache so the GE reads the updated pixel data from RAM */
    sceKernelDcacheWritebackRange(vram_buf, tex_size);

    /* Upload RGB565 texture (GU_PSM_5650 = 16-bit, no alpha) */
    sceGuTexMode(GU_PSM_5650, 0, 0, 0);
    sceGuTexImage(0, (unsigned int)tw, (unsigned int)th,
                  (unsigned int)tw, vram_buf);
    sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGB);
    sceGuTexFilter(GU_LINEAR, GU_LINEAR);
    sceGuTexWrap(GU_CLAMP, GU_CLAMP);
    /* Draw the icon using a standard 4-vertex triangle strip (robust across HW) */
    TexVert *v = (TexVert *)sceGuGetMemory(4 * sizeof(TexVert));
    if (!v) return;

    /* UV coords cover exactly the (w, h) icon pixels in the (tw, th) buffer */
    v[0].u = 0.0f;       v[0].v = 0.0f;
    v[0].x = (float)x;   v[0].y = (float)y;       v[0].z = 0.0f;

    v[1].u = 0.0f;       v[1].v = (float)sh;
    v[1].x = (float)x;   v[1].y = (float)(y+dh);  v[1].z = 0.0f;

    v[2].u = (float)sw;   v[2].v = 0.0f;
    v[2].x = (float)(x+dw); v[2].y = (float)y;    v[2].z = 0.0f;

    v[3].u = (float)sw;   v[3].v = (float)sh;
    v[3].x = (float)(x+dw); v[3].y = (float)(y+dh); v[3].z = 0.0f;

    /* Force GE state reset for the icon draw */
    sceGuDisable(GU_BLEND);
    sceGuDisable(GU_ALPHA_TEST);
    sceGuDisable(GU_STENCIL_TEST);
    sceGuDisable(GU_DEPTH_TEST);
    tex_enable();

    sceGuDrawArray(GU_TRIANGLE_STRIP,
                   GU_TEXTURE_32BITF | GU_VERTEX_32BITF | GU_TRANSFORM_2D,
                   4, NULL, v);

    tex_disable();

    /* Textures are unclipped sharp rectangles.
       If we are focused, the game_grid_ui draws the glow ring BEHIND the texture
       so we don't draw a border on TOP. */
}

void ui_draw_texture_rounded_rgb565(int x, int y, int dw, int dh, int r,
                                    void *tex_data, int sw, int sh,
                                    int tw, int th, int focused)
{
    if (!tex_data) {
        ui_draw_texture_placeholder(x, y, dw, dh, NULL, focused);
        return;
    }

    if (r <= 0) {
        ui_draw_texture_rgb565(x, y, dw, dh, tex_data, sw, sh, tw, th, focused);
        return;
    }
    if (r > dw/2) r = dw/2;
    if (r > dh/2) r = dh/2;

    int max_rects = 3 + 4 * r;
    TexVert *v = (TexVert *)sceGuGetMemory(max_rects * 2 * sizeof(TexVert));
    if (!v) return;

    int count = 0;
    float u_scale = (float)sw / (float)dw;
    float v_scale = (float)sh / (float)dh;

#define ADD_TEX_RECT(rx, ry, rw, rh) do { \
        if (count < max_rects) { \
            v[2*count].u = (float)((rx) - x) * u_scale; \
            v[2*count].v = (float)((ry) - y) * v_scale; \
            v[2*count].x = (float)(rx); \
            v[2*count].y = (float)(ry); \
            v[2*count].z = 0.0f; \
            v[2*count+1].u = (float)((rx) + (rw) - x) * u_scale; \
            v[2*count+1].v = (float)((ry) + (rh) - y) * v_scale; \
            v[2*count+1].x = (float)((rx) + (rw)); \
            v[2*count+1].y = (float)((ry) + (rh)); \
            v[2*count+1].z = 0.0f; \
            count++; \
        } \
    } while(0)

    ADD_TEX_RECT(x + r, y, dw - 2*r, dh);
    ADD_TEX_RECT(x, y + r, r, dh - 2*r);
    ADD_TEX_RECT(x + dw - r, y + r, r, dh - 2*r);

    for (int i = 0; i < r; i++) {
        int dx = r - 1;
        while (dx * dx + (r - i) * (r - i) > r * r && dx > 0) dx--;

        ADD_TEX_RECT(x + r - dx, y + i, dx, 1);
        ADD_TEX_RECT(x + dw - r, y + i, dx, 1);
        ADD_TEX_RECT(x + r - dx, y + dh - 1 - i, dx, 1);
        ADD_TEX_RECT(x + dw - r, y + dh - 1 - i, dx, 1);
    }
#undef ADD_TEX_RECT

    unsigned int tex_size = (unsigned int)tw * (unsigned int)th * sizeof(unsigned short);
    unsigned short *vram_buf = (unsigned short *)tex_data;

    sceKernelDcacheWritebackRange(vram_buf, tex_size);

    sceGuTexMode(GU_PSM_5650, 0, 0, 0);
    sceGuTexImage(0, (unsigned int)tw, (unsigned int)th, (unsigned int)tw, vram_buf);
    sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGB);
    sceGuTexFilter(GU_LINEAR, GU_LINEAR);
    sceGuTexWrap(GU_CLAMP, GU_CLAMP);

    sceGuDisable(GU_BLEND);
    sceGuDisable(GU_ALPHA_TEST);
    sceGuDisable(GU_STENCIL_TEST);
    sceGuDisable(GU_DEPTH_TEST);
    sceGuEnable(GU_TEXTURE_2D);

    sceGuDrawArray(GU_SPRITES, GU_TEXTURE_32BITF | GU_VERTEX_32BITF | GU_TRANSFORM_2D, count * 2, NULL, v);
}


/*
 * --- WHAT THIS DOES ---
 * Blocking fatal-error modal overlay.
 * Draws a full-screen dark overlay + centred error panel with red border,
 * title, message, and "Press Circle to continue" hint.
 * Loops calling ui_process_input() until UI_EVT_BACK (Circle) is received.
 * Call this when a non-recoverable network or decode error occurs so the
 * user can read the error before being returned to the host list.
 */
void ui_show_fatal_error(const char *title, const char *msg)
{
    while (1) {
        ui_begin_frame();

        /* Draw whatever was already on screen (gradient bg as clean slate) */
        ui_draw_gradient_bg(UI_COL_BG_TOP, UI_COL_BG_BOT);

        /* Draw the blocking error modal */
        ui_draw_error_modal(title, msg, "Press O to continue");

        ui_end_frame();

        UIEvent evt = ui_process_input();
        if (evt == UI_EVT_BACK || evt == UI_EVT_SELECT) {
            break;
        }
        sceKernelDelayThread(50 * 1000);
    }
}

} /* extern "C" */
