/*
 * exit_dialog.c - Quit/Exit confirmation dialog for PSP Moonlight
 */

#include <pspctrl.h>
#include <pspthreadman.h>
#include <string.h>

#include "exit_dialog.h"
#include "ui_manager.h"

#define FRAME_DELAY_US (50 * 1000)

int exit_dialog_run(void)
{
    SceCtrlData pad;
    SceCtrlData prev_pad;
    memset(&prev_pad, 0, sizeof(prev_pad));

    extern volatile unsigned int g_remote_buttons;
    extern volatile unsigned int g_remote_app_exit_request;

    while (1) {
        int panel_x;
        int panel_y;
        int t;
        int cross;
        int circle;

        sceCtrlPeekBufferPositive(&pad, 1);
        pad.Buttons |= g_remote_buttons;
        g_remote_buttons = 0;

        if (g_remote_app_exit_request) {
            return 1;
        }

        ui_begin_frame();
        ui_draw_gradient_bg(UI_COL_BG_TOP, UI_COL_BG_BOT);
        ui_draw_header("Exit Moonlight");

        panel_x = (UI_SCREEN_W - 280) / 2;
        panel_y = (UI_SCREEN_H - 90) / 2;
        t = 2;
        ui_set_blend(1);
        ui_draw_rect_rounded(panel_x, panel_y, 280, 90, 12, UI_COL_BORDER_FOC);
        ui_draw_rect_rounded(panel_x + t, panel_y + t, 280 - 2 * t, 90 - 2 * t,
                             12 - t, UI_COL_PANEL_DARK);
        ui_set_blend(0);

        ui_draw_text_centered((float)panel_x, 280.0f,
                              (float)(panel_y + 20), UI_COL_TEXT,
                              "Exit Moonlight?");
        ui_draw_text_centered((float)panel_x, 280.0f,
                              (float)(panel_y + 45), UI_COL_TEXT_DIM,
                              "All streams will be closed.");

        ui_draw_footer_hint("{X}: Exit  {O}: Stay");
        ui_end_frame();

        cross = (pad.Buttons & PSP_CTRL_CROSS) &&
                !(prev_pad.Buttons & PSP_CTRL_CROSS);
        circle = (pad.Buttons & PSP_CTRL_CIRCLE) &&
                 !(prev_pad.Buttons & PSP_CTRL_CIRCLE);

        if (cross) {
            return 1;
        }

        if (circle) {
            return 0;
        }

        prev_pad = pad;
        sceKernelDelayThread(FRAME_DELAY_US);
    }
}
