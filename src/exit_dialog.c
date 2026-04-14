/*
 * exit_dialog.c - Quit/Exit confirmation dialog for PSP Moonlight
 *
 * Draws a modal "Exit Moonlight?" prompt.  On Yes it performs a safe ordered
 * shutdown of subsystems before calling sceKernelExitGame().
 */

#include <pspkernel.h>
#include <pspthreadman.h>
#include <pspctrl.h>
#include <string.h>

#include "exit_dialog.h"
#include "ui_manager.h"
#include "hud.h"
#include "stream_session.h"

#define FRAME_DELAY_US  (50 * 1000)

/* -------------------------------------------------------------------------
 * exit_dialog_run
 * ------------------------------------------------------------------------- */
int exit_dialog_run(void)
{
    SceCtrlData pad;
    SceCtrlData prev_pad;
    memset(&prev_pad, 0, sizeof(prev_pad));

    extern volatile unsigned int g_remote_buttons;

    while (1) {
        sceCtrlPeekBufferPositive(&pad, 1);
        pad.Buttons |= g_remote_buttons; g_remote_buttons = 0;

        /* ---- Draw dialog ---- */
        ui_begin_frame();
        ui_draw_gradient_bg(UI_COL_BG_TOP, UI_COL_BG_BOT);
        ui_draw_header("Exit Moonlight");

        /* Modal panel: 280 wide × 90 tall, centred */
        int panel_x = (UI_SCREEN_W - 280) / 2;  /* 100 */
        int panel_y = (UI_SCREEN_H -  90) / 2;  /* 91  */
        int t = 2;
        ui_set_blend(1);
        ui_draw_rect_rounded(panel_x, panel_y, 280, 90, 12, UI_COL_BORDER_FOC);
        ui_draw_rect_rounded(panel_x + t, panel_y + t, 280 - 2*t, 90 - 2*t, 12 - t, UI_COL_PANEL_DARK);
        ui_set_blend(0);

        /* Text */
        ui_draw_text_centered(
            (float)panel_x, 280.0f,
            (float)(panel_y + 20), UI_COL_TEXT,
            "Exit Moonlight?");
        ui_draw_text_centered(
            (float)panel_x, 280.0f,
            (float)(panel_y + 45), UI_COL_TEXT_DIM,
            "All streams will be closed.");

        ui_draw_footer_hint("{X}: Exit  {O}: Stay");
        ui_end_frame();

        /* ---- Input ---- */
        int cross  = (pad.Buttons & PSP_CTRL_CROSS)  && !(prev_pad.Buttons & PSP_CTRL_CROSS);
        int circle = (pad.Buttons & PSP_CTRL_CIRCLE) && !(prev_pad.Buttons & PSP_CTRL_CIRCLE);

        if (cross) {
            /* ---- Ordered shutdown ---- */
            /* Stop HUD first (safe even if not initialised) */
            hud_shutdown();
            /* Shut down stream session (network/ME/sockets/decoder/display).
             * end_stream_session() calls sceKernelExitGame() internally.     */
            end_stream_session();
            /* Should not reach here — end_stream_session exits the process.
             * Belt-and-suspenders fallback: */
            sceKernelExitGame();
            /* Not reached */
            return 0;
        }

        if (circle) {
            return 0;   /* User chose to stay */
        }

        prev_pad = pad;
        sceKernelDelayThread(FRAME_DELAY_US);
    }
}
