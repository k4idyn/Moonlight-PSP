/*
 * pairing_pin_ui.cpp - PSP UI component for displaying pairing PIN
 *
 * Implements a full-screen PIN display with large centered digits
 * and instructional text. Uses PSP's GU for rendering and
 * sceKernelGetSystemTimeLow() for periodic state checking.
 */

#include <pspkernel.h>
#include <pspctrl.h>
#include <pspdisplay.h>
#include <pspgu.h>
#include <pspgum.h>
#include <stdio.h>
#include <string.h>

#include "pairing_pin_ui.h"
#include "shared.h"
#include "ui_manager.h"

/*============================================================================
 * Internal Constants
 *============================================================================*/

/* Screen dimensions */
#define SCREEN_WIDTH    480
#define SCREEN_HEIGHT   272

/* Colors — use the theme system for cohesion */
#define COLOR_BLACK     UI_COL_BG_TOP
#define COLOR_WHITE     UI_COL_TEXT
#define COLOR_BLUE      UI_COL_ACCENT
#define COLOR_GRAY      UI_COL_TEXT_DIM
#define COLOR_DARK_BG   UI_COL_PANEL_DARK
#define COLOR_ACCENT    UI_COL_ACCENT

/* Font sizing (approximate character dimensions) */
#define CHAR_WIDTH      12
#define CHAR_HEIGHT     20
#define LARGE_CHAR_WIDTH   24
#define LARGE_CHAR_HEIGHT  40

/* Layout positions */
#define PIN_Y_POSITION      90
#define MESSAGE_Y_POSITION  155
#define STATUS_Y_POSITION   210

/* GU display list no longer needed — UIManager owns the GU context */
/* static u32 __attribute__((aligned(16))) gu_display_list[64 * 1024 / 4]; */

/* Vertex structure still used for 7-segment digit drawing */

/*============================================================================
 * Internal Helpers
 *============================================================================*/

/**
 * render_pin - Render the 4-digit PIN using intraFont at 2× scale.
 *
 * WHY INTRAFONT INSTEAD OF 7-SEGMENT RECTS:
 *   On real PSP-1000 hardware the sceGu texture environment left by
 *   intraFont (GU_TFX_MODULATE) can make subsequent untextured coloured
 *   rects appear invisible even after sceGuTexFunc(GU_TFX_REPLACE) is
 *   reset — because tex_enable() inside ui_draw_rect re-binds the font
 *   atlas with GU_TFX_REPLACE, then the NEXT tex_disable/draw cycle
 *   occasionally races the GE pipeline on real silicon.  Using intraFont
 *   directly bypasses all of that: the blend guard path is proven-working
 *   for all other on-screen text.
 */
static void render_pin(const char *pin, int center_x, int y)
{
    /* Contrasting background box so the PIN area is always visible,
     * even if the font has a rendering hiccup.  Also acts as a visual
     * 'card' that makes the PIN look intentional. */
    ui_set_blend(1);
    ui_draw_rect_rounded(center_x - 110, y - 8, 220, 60, 12, COLOR_ACCENT);
    ui_draw_rect_rounded(center_x - 110 + 2, y - 8 + 2, 220 - 4, 60 - 4, 10, UI_COL_PANEL_DARK); /* inner dark box */
    ui_set_blend(0);

    /* Render PIN digits as large intraFont text (2× scale, centred).
     * baseline_y = y + 36 — centers it horizontally inside the 60 px box. */
    ui_draw_pin_large(center_x, y + 36, COLOR_ACCENT, pin);
}

/**
 * render_text_centered - render text using UIManager's intraFont support.
 * Replaces the old pspDebugScreen-based workaround.
 */
static void render_text_centered(const char *text, int y, u32 color)
{
    ui_draw_text_centered(0.0f, (float)SCREEN_WIDTH, (float)y, color, text);
}

/**
 * check_user_cancel - Check if user pressed CIRCLE or TRIANGLE to cancel.
 *
 * CIRCLE (O) is the standard PSP 'back/cancel' button.
 * TRIANGLE is included as a fail-safe for users who accidentally enter
 * the pairing screen and want to leave immediately.
 * HOME is NOT checked — pressing HOME invokes the PSP system overlay
 * (XMB) at the OS level before our code sees it.
 */
static int check_user_cancel(void)
{
    SceCtrlData pad;
    sceCtrlPeekBufferPositive(&pad, 1);

    if (pad.Buttons & (PSP_CTRL_CIRCLE | PSP_CTRL_TRIANGLE)) {
        return 1;
    }
    return 0;
}

/*============================================================================
 * Public API Implementation
 *============================================================================*/

int pairing_pin_ui_init(PairingPINUI *ui, const char *pin,
                        volatile int *isPaired, volatile int *threadDone)
{
    if (!ui || !pin || !isPaired || !threadDone) {
        return -1;
    }

    /* Validate PIN is 4 digits */
    int pin_len = strlen(pin);
    if (pin_len != PIN_DIGITS) {
        /* Log to debug file; no debug screen calls */
        SceUID fd = sceIoOpen("ms0:/moonlight_debug.log",
                              PSP_O_WRONLY | PSP_O_APPEND | PSP_O_CREAT, 0777);
        if (fd >= 0) {
            char msg[64];
            snprintf(msg, sizeof(msg), "[PAIR] PIN must be %d digits\n", PIN_DIGITS);
            sceIoWrite(fd, msg, strlen(msg));
            sceIoClose(fd);
        }
        return -1;
    }

    /* Validate PIN contains only digits */
    for (int i = 0; i < PIN_DIGITS; i++) {
        if (pin[i] < '0' || pin[i] > '9') {
            return -1;
        }
    }

    /* Initialize structure */
    memset(ui, 0, sizeof(PairingPINUI));
    strncpy(ui->pin, pin, PIN_DIGITS);
    ui->pin[PIN_DIGITS] = '\0';
    ui->isPaired = isPaired;
    ui->threadDone = threadDone;
    ui->state = PAIRING_PIN_STATE_ACTIVE;
    ui->last_check_time = sceKernelGetSystemTimeLow();
    ui->initialized = 1;

    return 0;
}

PairingPINState pairing_pin_ui_run(PairingPINUI *ui)
{
    if (!ui || !ui->initialized) {
        return PAIRING_PIN_STATE_CANCELLED;
    }

    /* --- WHAT THIS DOES ---
     * Render loop for the pairing PIN screen.
     * Uses UIManager's GU frame management (ui_begin_frame / ui_end_frame)
     * instead of directly calling sceGuStart / sceGuTerm.
     * The large 7-segment digits are drawn via draw_filled_rect (which now
     * delegates to ui_draw_rect so no separate GU state needed).
     */

    /* Flush stale button state so a held Circle/Home doesn't immediately cancel */
    {
        SceCtrlData flush_pad;
        int flush_iters = 0;
        do {
            sceCtrlPeekBufferPositive(&flush_pad, 1);
            sceKernelDelayThread(16667);
            flush_iters++;
        } while (flush_pad.Buttons != 0 && flush_iters < 60);
    }

    while (ui->state == PAIRING_PIN_STATE_ACTIVE) {
        /* Periodic pairing status check every PIN_CHECK_INTERVAL_US */
        u32 current_time = sceKernelGetSystemTimeLow();
        if (current_time - ui->last_check_time >= PIN_CHECK_INTERVAL_US) {
            ui->last_check_time = current_time;
            if (ui->isPaired && *(ui->isPaired)) {
                ui->state = PAIRING_PIN_STATE_PAIRED;
                break;
            }

            /* Pairing worker ended without setting isPaired => failure/cancel. */
            if (ui->threadDone && *(ui->threadDone) &&
                !(ui->isPaired && *(ui->isPaired))) {
                ui->state = PAIRING_PIN_STATE_CANCELLED;
                break;
            }
        }

        /* Check for user cancellation */
        if (check_user_cancel()) {
            ui->state = PAIRING_PIN_STATE_CANCELLED;
            break;
        }

        /* --- Draw pairing PIN screen via UIManager --- */
        ui_begin_frame();

        /* Gradient background */
        ui_draw_gradient_bg(UI_COL_BG_TOP, UI_COL_BG_BOT);

        /* Header bar */
        ui_draw_header("Pairing");

        /* "Pairing Code" label */
        render_text_centered("Pairing Code", 44, UI_COL_TEXT);

        /* Large 7-segment PIN */
        render_pin(ui->pin, SCREEN_WIDTH / 2, PIN_Y_POSITION);

        /* Instruction text */
        render_text_centered("Enter this on Sunshine Web UI.",
                             MESSAGE_Y_POSITION, UI_COL_TEXT);
        render_text_centered("Waiting for pairing...",
                             STATUS_Y_POSITION, UI_COL_TEXT_DIM);

        /* Footer hint — show both cancel keys so users know how to exit */
        ui_draw_footer_hint("O / Triangle: Cancel pairing");

        ui_end_frame();

        sceKernelDelayThread(16000); /* yield ~60 fps */
    }

    /* Show brief success message before returning to library screen */
    if (ui->state == PAIRING_PIN_STATE_PAIRED) {
        for (int i = 0; i < 30; i++) {   /* ~0.5 seconds at 60 fps */
            ui_begin_frame();
            ui_draw_gradient_bg(UI_COL_BG_TOP, UI_COL_BG_BOT);
            ui_draw_header("Pairing");
            render_text_centered("Pairing Successful!",  SCREEN_HEIGHT/2 - 10, UI_COL_ACCENT);
            render_text_centered("Connecting to Library...", SCREEN_HEIGHT/2 + 14, UI_COL_TEXT);
            ui_end_frame();
            sceKernelDelayThread(16000);
        }
    }
    /* GU / UIManager is already initialised by display_init() + ui_manager_init().
     * We do NOT call sceGuInit/Term here — that would clobber the shared context. */

    return ui->state;
}

void pairing_pin_ui_shutdown(PairingPINUI *ui)
{
    if (ui) {
        ui->initialized = 0;
        ui->state = PAIRING_PIN_STATE_CANCELLED;
    }
}

PairingPINState pairing_pin_ui_get_state(const PairingPINUI *ui)
{
    if (!ui) {
        return PAIRING_PIN_STATE_CANCELLED;
    }
    return ui->state;
}