/*
 * osk_input.c - 3x4 Matrix Keypad IP Address Entry for PSP Moonlight
 *
 * Shows a phone-style flat keypad for entering IPv4 addresses.
 *
 *   [1][2][3]
 *   [4][5][6]
 *   [7][8][9]
 *   [.][0][DEL]
 *
 * Controls:
 *   D-pad    - Move keypad cursor (wraps)
 *   Cross    - Press selected key
 *   Start    - Confirm (accepts valid IPv4)
 *   Triangle - Cancel (returns -1)
 */

#include <pspkernel.h>
#include <pspthreadman.h>
#include <pspctrl.h>
#include <pspdisplay.h>
#include <string.h>
#include <stdio.h>

#include "osk_input.h"
#include "stream_resolution.h"
#include "ui_manager.h"

/* =========================================================================
 * Keypad definition
 * ========================================================================= */
#define KP_COLS  3
#define KP_ROWS  4

/* Sentinel values for special keys */
#define KP_DOT  10
#define KP_DEL  11

static const int s_keys[KP_ROWS][KP_COLS] = {
    {  1,      2,    3    },
    {  4,      5,    6    },
    {  7,      8,    9    },
    { KP_DOT,  0,  KP_DEL }
};

static const char * const s_key_labels[KP_ROWS][KP_COLS] = {
    { "1",  "2",   "3"   },
    { "4",  "5",   "6"   },
    { "7",  "8",   "9"   },
    { ".",  "0",  "DEL"  }
};

/* =========================================================================
 * Layout (all native 480x272 pixels)
 *   [Header bar]            y = 0..27
 *   [IP display box]        y = 50..80
 *   [Keypad  3x4]           y = 100..245
 *   [Footer hint]           y = 252..271
 * ========================================================================= */
#define KEY_W       56
#define KEY_H       32
#define KEY_GAP      6

/* KP_TOTAL_W = 3*56 + 2*6 = 180  KP_LEFT = (480-180)/2 = 150 */
#define KP_TOTAL_W  (KP_COLS * KEY_W + (KP_COLS - 1) * KEY_GAP)
#define KP_LEFT     ((UI_SCREEN_W - KP_TOTAL_W) / 2)
#define KP_TOP      90

#define IP_BOX_X    60
#define IP_BOX_Y    45
#define IP_BOX_W    360
#define IP_BOX_H    30

/* Max string length: "255.255.255.255" = 15 chars + NUL */
#define MAX_IP_LEN  15

/* How many frames to show the invalid-IP flash (at 60fps ≈ 1.5s) */
#define FLASH_FRAMES 90

/* =========================================================================
 * String helpers
 * ========================================================================= */

/* Append one digit (0-9) — no auto-dot logic, user controls dots manually. */
static int append_digit(char *buf, int *len, int digit)
{
    if (*len >= MAX_IP_LEN) return 0;
    buf[(*len)++] = (char)('0' + digit);
    buf[*len]     = '\0';
    return 1;
}

/* Append a dot — reject if buffer is empty, full, or previous char is '.'. */
static int append_dot(char *buf, int *len)
{
    if (*len == 0 || *len >= MAX_IP_LEN) return 0;
    if (buf[*len - 1] == '.') return 0;  /* no consecutive dots */
    buf[(*len)++] = '.';
    buf[*len]     = '\0';
    return 1;
}

static void do_backspace(char *buf, int *len)
{
    if (*len > 0) {
        (*len)--;
        buf[*len] = '\0';
    }
}

/* Returns 1 if buf is a complete, valid IPv4 address */
static int validate_ip(const char *buf)
{
    int  a, b, c, d;
    char rebuilt[16];
    if (sscanf(buf, "%d.%d.%d.%d", &a, &b, &c, &d) != 4) return 0;
    if (a < 0 || a > 255 || b < 0 || b > 255 ||
        c < 0 || c > 255 || d < 0 || d > 255) return 0;
    snprintf(rebuilt, sizeof(rebuilt), "%d.%d.%d.%d", a, b, c, d);
    return (strcmp(rebuilt, buf) == 0) ? 1 : 0;
}

/* =========================================================================
 * Rendering
 * ========================================================================= */

static void draw_key(int row, int col, int selected)
{
    int kx     = KP_LEFT + col * (KEY_W + KEY_GAP);
    int ky     = KP_TOP  + row * (KEY_H + KEY_GAP);
    u32 bg     = selected ? UI_COL_CARD_SEL : UI_COL_CARD;
    u32 border = selected ? UI_COL_BORDER_FOC : UI_COL_BORDER;
    u32 tcol   = selected ? UI_COL_TEXT_FOCUS : UI_COL_TEXT_DIM;

    ui_set_blend(1);
    int t = selected ? 2 : 1;
    ui_draw_rect_rounded(kx, ky, KEY_W, KEY_H, 8, border);
    ui_draw_rect_rounded(kx + t, ky + t, KEY_W - 2*t, KEY_H - 2*t, 8 - t, bg);
    ui_set_blend(0);
    ui_draw_text_centered((float)kx, (float)KEY_W,
                           (float)(ky + KEY_H / 2 + 5),
                           tcol,
                           s_key_labels[row][col]);
}

/* =========================================================================
 * Public API
 * ========================================================================= */

int osk_get_ip_input(char *out_ip, int max_len)
{
    char buf[MAX_IP_LEN + 1];
    int  buflen  = 0;
    int  sel_row = 0;   /* Cursor starts on key '1' */
    int  sel_col = 0;
    int  row, col;
    int  flash_timer = 0; /* Counts down from FLASH_FRAMES when invalid IP submitted */
    SceCtrlData pad, prev;

    if (!out_ip || max_len < 16) return -3;

    /* Ensure controller is sampled properly */
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);

    memset(buf, 0, sizeof(buf));
    memset(&pad,  0, sizeof(pad));
    memset(&prev, 0, sizeof(prev));

    /*
     * Flush stale button state: wait until ALL buttons are released.
     * This prevents the Triangle press (that opened this screen) from
     * being re-processed as a key-press inside the keypad loop.
     */
    {
        int flush_iters = 0;
        do {
            sceCtrlPeekBufferPositive(&pad, 1);
            sceKernelDelayThread(16667);
            flush_iters++;
        } while (pad.Buttons != 0 && flush_iters < 60);
    }

    /* Initialise prev to the current (zero) state */
    sceCtrlPeekBufferPositive(&pad, 1);
    memcpy(&prev, &pad, sizeof(pad));

    while (1) {
        u32 pressed;
        extern volatile unsigned int g_remote_buttons;
        memcpy(&prev, &pad, sizeof(pad));
        sceCtrlPeekBufferPositive(&pad, 1);
        pad.Buttons |= g_remote_buttons; g_remote_buttons = 0;
        pressed = pad.Buttons & ~prev.Buttons;

        /* CANCEL: Circle */
        if (pressed & PSP_CTRL_CIRCLE)
            return -1;

        /* CONFIRM: Start — accept if the current buffer is a valid IPv4 */
        if (pressed & PSP_CTRL_START) {
            if (validate_ip(buf)) {
                snprintf(out_ip, (size_t)max_len, "%s", buf);
                return 0;
            }
            /* Not valid — start the error flash */
            flash_timer = FLASH_FRAMES;
        }

        /* Tick down the flash timer */
        if (flash_timer > 0) {
            flash_timer--;
        }

        /* D-pad navigation (wraps around) */
        if (pressed & PSP_CTRL_UP)
            sel_row = (sel_row + KP_ROWS - 1) % KP_ROWS;
        if (pressed & PSP_CTRL_DOWN)
            sel_row = (sel_row + 1) % KP_ROWS;
        if (pressed & PSP_CTRL_LEFT)
            sel_col = (sel_col + KP_COLS - 1) % KP_COLS;
        if (pressed & PSP_CTRL_RIGHT)
            sel_col = (sel_col + 1) % KP_COLS;

        /* PRESS KEY: Cross (X button) */
        if (pressed & PSP_CTRL_CROSS) {
            int k = s_keys[sel_row][sel_col];
            if (k == KP_DOT) {
                append_dot(buf, &buflen);
            } else if (k == KP_DEL) {
                do_backspace(buf, &buflen);
            } else {
                /* Digit 0-9 */
                append_digit(buf, &buflen, k);
            }
        }

        /* --- Draw frame --- */
        ui_begin_frame();
        ui_draw_gradient_bg(UI_COL_BG_TOP, UI_COL_BG_BOT);
        ui_draw_header("Add PC Manually");

        /* IP display box — shows what has been typed so far */
        {
            char display[MAX_IP_LEN + 2];  /* +1 underscore cursor, +1 NUL */
            u32 box_border = (flash_timer > 0) ? 0xFF4040FFu   /* Red on error */
                                               : UI_COL_BORDER_FOC;
            snprintf(display, sizeof(display), "%s_", buf);
            ui_set_blend(1);
            ui_draw_rect_rounded(IP_BOX_X, IP_BOX_Y, IP_BOX_W, IP_BOX_H, 8, box_border);
            ui_draw_rect_rounded(IP_BOX_X + 2, IP_BOX_Y + 2, IP_BOX_W - 4, IP_BOX_H - 4, 6, UI_COL_CARD);
            ui_set_blend(0);
            ui_draw_text_large((float)(IP_BOX_X + 10),
                               (float)(IP_BOX_Y + IP_BOX_H / 2 + 6),
                               (flash_timer > 0) ? 0xFF4040FFu : UI_COL_TEXT_FOCUS,
                               display);
            if (flash_timer > 0) {
                ui_draw_text_centered((float)IP_BOX_X, (float)IP_BOX_W,
                                      (float)(IP_BOX_Y + IP_BOX_H + 4),
                                      0xFF4040FFu,
                                      "Enter full IP: e.g. 192.168.1.100");
            }
        }

        /* 3x4 keypad */
        for (row = 0; row < KP_ROWS; row++) {
            for (col = 0; col < KP_COLS; col++) {
                draw_key(row, col,
                         (row == sel_row && col == sel_col) ? 1 : 0);
            }
        }

        /* Footer */
        ui_draw_footer_hint("{DP}: Move  {X}: Press  {ST}: Confirm  {O}: Cancel");
        ui_end_frame();
        /* No extra sceKernelDelayThread — ui_end_frame syncs vblank (16ms) */
    }
}

/* =========================================================================
 * Resolution keypad — "width x height" entry
 *
 * Identical 3x4 grid but bottom-left key is "x" instead of "." and the
 * max length is shorter (e.g. "1024x512" = 8 chars).  A faint tooltip
 * "width x height" sits in the text bar and vanishes on first key press.
 * ========================================================================= */
#define RES_MAX_LEN 9   /* covers "1024x512" plus separators */
#define KP_SEP  12      /* sentinel for the 'x' separator key */

static const int s_res_keys[KP_ROWS][KP_COLS] = {
    {  1,       2,     3     },
    {  4,       5,     6     },
    {  7,       8,     9     },
    { KP_SEP,   0,   KP_DEL }
};

static const char * const s_res_labels[KP_ROWS][KP_COLS] = {
    { "1",  "2",   "3"   },
    { "4",  "5",   "6"   },
    { "7",  "8",   "9"   },
    { "x",  "0",  "DEL"  }
};

static int append_sep(char *buf, int *len)
{
    /* Only allow one 'x' separator, and not at position 0 */
    if (*len == 0 || *len >= RES_MAX_LEN) return 0;
    if (strchr(buf, 'x') != NULL) return 0;  /* already have one */
    buf[(*len)++] = 'x';
    buf[*len]     = '\0';
    return 1;
}

static void draw_res_key(int row, int col, int selected)
{
    int kx     = KP_LEFT + col * (KEY_W + KEY_GAP);
    int ky     = KP_TOP  + row * (KEY_H + KEY_GAP);
    u32 bg     = selected ? UI_COL_CARD_SEL : UI_COL_CARD;
    u32 border = selected ? UI_COL_BORDER_FOC : UI_COL_BORDER;
    u32 tcol   = selected ? UI_COL_TEXT_FOCUS : UI_COL_TEXT_DIM;

    ui_set_blend(1);
    int t = selected ? 2 : 1;
    ui_draw_rect_rounded(kx, ky, KEY_W, KEY_H, 8, border);
    ui_draw_rect_rounded(kx + t, ky + t, KEY_W - 2*t, KEY_H - 2*t, 8 - t, bg);
    ui_set_blend(0);
    ui_draw_text_centered((float)kx, (float)KEY_W,
                           (float)(ky + KEY_H / 2 + 5),
                           tcol,
                           s_res_labels[row][col]);
}

static int validate_resolution(const char *buf, int *out_w, int *out_h)
{
    int w = 0, h = 0;
    int norm_w, norm_h;
    const char *sep = strchr(buf, 'x');
    if (!sep || sep == buf || *(sep + 1) == '\0') return 0;
    if (sscanf(buf, "%dx%d", &w, &h) != 2) return 0;
    norm_w = w;
    norm_h = h;
    stream_resolution_normalize(&norm_w, &norm_h);

    /* Reject values that would be silently altered at runtime. */
    if (w != norm_w || h != norm_h) return 0;

    *out_w = w;
    *out_h = h;
    return 1;
}

int osk_get_resolution_input(int *out_width, int *out_height)
{
    char buf[RES_MAX_LEN + 1];
    int  buflen  = 0;
    int  sel_row = 0;
    int  sel_col = 0;
    int  row, col;
    int  flash_timer = 0;
    int  tooltip_visible = 1;  /* disappears on first keypress */
    SceCtrlData pad, prev;

    if (!out_width || !out_height) return -3;

    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);

    memset(buf, 0, sizeof(buf));
    memset(&pad,  0, sizeof(pad));
    memset(&prev, 0, sizeof(prev));

    /* Flush stale button state */
    {
        int flush_iters = 0;
        do {
            sceCtrlPeekBufferPositive(&pad, 1);
            sceKernelDelayThread(16667);
            flush_iters++;
        } while (pad.Buttons != 0 && flush_iters < 60);
    }

    sceCtrlPeekBufferPositive(&pad, 1);
    memcpy(&prev, &pad, sizeof(pad));

    while (1) {
        u32 pressed;
        extern volatile unsigned int g_remote_buttons;
        memcpy(&prev, &pad, sizeof(pad));
        sceCtrlPeekBufferPositive(&pad, 1);
        pad.Buttons |= g_remote_buttons; g_remote_buttons = 0;
        pressed = pad.Buttons & ~prev.Buttons;

        /* CANCEL: Circle */
        if (pressed & PSP_CTRL_CIRCLE)
            return -1;

        /* CONFIRM: Start */
        if (pressed & PSP_CTRL_START) {
            int w, h;
            if (validate_resolution(buf, &w, &h)) {
                *out_width  = w;
                *out_height = h;
                return 0;
            }
            flash_timer = FLASH_FRAMES;
        }

        if (flash_timer > 0) flash_timer--;

        /* D-pad navigation */
        if (pressed & PSP_CTRL_UP)
            sel_row = (sel_row + KP_ROWS - 1) % KP_ROWS;
        if (pressed & PSP_CTRL_DOWN)
            sel_row = (sel_row + 1) % KP_ROWS;
        if (pressed & PSP_CTRL_LEFT)
            sel_col = (sel_col + KP_COLS - 1) % KP_COLS;
        if (pressed & PSP_CTRL_RIGHT)
            sel_col = (sel_col + 1) % KP_COLS;

        /* PRESS KEY: Cross (X button) */
        if (pressed & PSP_CTRL_CROSS) {
            int k = s_res_keys[sel_row][sel_col];
            if (k == KP_SEP) {
                append_sep(buf, &buflen);
            } else if (k == KP_DEL) {
                do_backspace(buf, &buflen);
            } else {
                append_digit(buf, &buflen, k);
            }
            tooltip_visible = 0;  /* hide tooltip on first keypress */
        }

        /* --- Draw frame --- */
        ui_begin_frame();
        ui_draw_gradient_bg(UI_COL_BG_TOP, UI_COL_BG_BOT);
        ui_draw_header("Custom Resolution");

        /* Resolution display box */
        {
            char display[RES_MAX_LEN + 2];
            u32 box_border = (flash_timer > 0) ? 0xFF4040FFu
                                               : UI_COL_BORDER_FOC;
            snprintf(display, sizeof(display), "%s_", buf);
            ui_set_blend(1);
            ui_draw_rect_rounded(IP_BOX_X, IP_BOX_Y, IP_BOX_W, IP_BOX_H, 8, box_border);
            ui_draw_rect_rounded(IP_BOX_X + 2, IP_BOX_Y + 2, IP_BOX_W - 4, IP_BOX_H - 4, 6, UI_COL_CARD);
            ui_set_blend(0);

            if (tooltip_visible && buflen == 0) {
                /* Subtle tooltip: "width x height" in dim text */
                ui_draw_text_large((float)(IP_BOX_X + 10),
                                   (float)(IP_BOX_Y + IP_BOX_H / 2 + 6),
                                   UI_COL_TEXT_DIM,
                                   "width x height");
            } else {
                ui_draw_text_large((float)(IP_BOX_X + 10),
                                   (float)(IP_BOX_Y + IP_BOX_H / 2 + 6),
                                   (flash_timer > 0) ? 0xFF4040FFu : UI_COL_TEXT_FOCUS,
                                   display);
            }

            if (flash_timer > 0) {
                char limit_text[64];
                snprintf(limit_text, sizeof(limit_text),
                         "Use mod-16. Min %dx%d, Max %dx%d",
                         STREAM_MIN_WIDTH, STREAM_MIN_HEIGHT,
                         STREAM_MAX_WIDTH, STREAM_MAX_HEIGHT);
                ui_draw_text_centered((float)IP_BOX_X, (float)IP_BOX_W,
                                      (float)(IP_BOX_Y + IP_BOX_H + 4),
                                      0xFF4040FFu,
                                      limit_text);
            }
        }

        /* 3x4 keypad (with 'x' instead of '.') */
        for (row = 0; row < KP_ROWS; row++) {
            for (col = 0; col < KP_COLS; col++) {
                draw_res_key(row, col,
                             (row == sel_row && col == sel_col) ? 1 : 0);
            }
        }

        ui_draw_footer_hint("{DP}: Move  {X}: Press  {ST}: Confirm  {O}: Cancel");
        ui_end_frame();
    }
}

/* =========================================================================
 * FPS keypad
 * ========================================================================= */
static int validate_fps(const char *buf, int *out_fps)
{
    int fps = 0;
    if (sscanf(buf, "%d", &fps) != 1) return 0;
    if (fps < 1 || fps > 144) return 0;
    *out_fps = fps;
    return 1;
}

int osk_get_fps_input(int *out_fps)
{
    char buf[RES_MAX_LEN + 1];
    int  buflen  = 0;
    int  sel_row = 0;
    int  sel_col = 0;
    int  row, col;
    int  flash_timer = 0;
    SceCtrlData pad, prev;

    if (!out_fps) return -3;

    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);

    memset(buf, 0, sizeof(buf));
    memset(&pad,  0, sizeof(pad));
    memset(&prev, 0, sizeof(prev));

    /* Flush stale button state */
    {
        int flush_iters = 0;
        do {
            sceCtrlPeekBufferPositive(&pad, 1);
            sceKernelDelayThread(16667);
            flush_iters++;
        } while (pad.Buttons != 0 && flush_iters < 60);
    }

    sceCtrlPeekBufferPositive(&pad, 1);
    memcpy(&prev, &pad, sizeof(pad));

    while (1) {
        u32 pressed;
        extern volatile unsigned int g_remote_buttons;
        memcpy(&prev, &pad, sizeof(pad));
        sceCtrlPeekBufferPositive(&pad, 1);
        pad.Buttons |= g_remote_buttons; g_remote_buttons = 0;
        pressed = pad.Buttons & ~prev.Buttons;

        /* CANCEL: Circle */
        if (pressed & PSP_CTRL_CIRCLE)
            return -1;

        /* CONFIRM: Start */
        if (pressed & PSP_CTRL_START) {
            int fps;
            if (validate_fps(buf, &fps)) {
                *out_fps = fps;
                return 0;
            }
            flash_timer = FLASH_FRAMES;
        }

        if (flash_timer > 0) flash_timer--;

        /* D-pad navigation */
        if (pressed & PSP_CTRL_UP)
            sel_row = (sel_row + KP_ROWS - 1) % KP_ROWS;
        if (pressed & PSP_CTRL_DOWN)
            sel_row = (sel_row + 1) % KP_ROWS;
        if (pressed & PSP_CTRL_LEFT)
            sel_col = (sel_col + KP_COLS - 1) % KP_COLS;
        if (pressed & PSP_CTRL_RIGHT)
            sel_col = (sel_col + 1) % KP_COLS;

        /* PRESS KEY: Cross (X button) */
        if (pressed & PSP_CTRL_CROSS) {
            int k = s_keys[sel_row][sel_col];
            if (k == KP_DEL) {
                do_backspace(buf, &buflen);
            } else if (k != KP_DOT) {
                append_digit(buf, &buflen, k);
            }
        }

        /* --- Draw frame --- */
        ui_begin_frame();
        ui_draw_gradient_bg(UI_COL_BG_TOP, UI_COL_BG_BOT);
        ui_draw_header("Custom FPS");

        /* Display box */
        {
            char display[RES_MAX_LEN + 2];
            u32 box_border = (flash_timer > 0) ? 0xFF4040FFu : UI_COL_BORDER_FOC;
            snprintf(display, sizeof(display), "%s_", buf);
            ui_set_blend(1);
            ui_draw_rect_rounded(IP_BOX_X, IP_BOX_Y, IP_BOX_W, IP_BOX_H, 8, box_border);
            ui_draw_rect_rounded(IP_BOX_X + 2, IP_BOX_Y + 2, IP_BOX_W - 4, IP_BOX_H - 4, 6, UI_COL_CARD);
            ui_set_blend(0);

            ui_draw_text_large((float)(IP_BOX_X + 10),
                               (float)(IP_BOX_Y + IP_BOX_H / 2 + 6),
                               (flash_timer > 0) ? 0xFF4040FFu : UI_COL_TEXT_FOCUS,
                               display);

            if (flash_timer > 0) {
                ui_draw_text_centered((float)IP_BOX_X, (float)IP_BOX_W,
                                      (float)(IP_BOX_Y + IP_BOX_H + 4),
                                      0xFF4040FFu,
                                      "Min 1, Max 144");
            }
        }

        /* 3x4 keypad */
        for (row = 0; row < KP_ROWS; row++) {
            for (col = 0; col < KP_COLS; col++) {
                draw_key(row, col, (row == sel_row && col == sel_col) ? 1 : 0);
            }
        }

        ui_draw_footer_hint("{DP}: Move  {X}: Press  {ST}: Confirm  {O}: Cancel");
        ui_end_frame();
    }
}
