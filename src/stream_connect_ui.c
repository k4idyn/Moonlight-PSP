/*
 * stream_connect_ui.c - "Connecting to Stream" transition screen
 *
 * A 60 fps background thread smoothly animates the progress bar between
 * phases.  The main (network) thread calls stream_connect_draw() to set
 * the target phase; the render thread interpolates toward it.
 */

#include <string.h>
#include <pspkernel.h>
#include <pspdisplay.h>

#include "stream_connect_ui.h"
#include "ui_manager.h"

/* Phase label strings */
static const char *s_phase_labels[4] = {
    "Requesting Stream...",
    "Negotiating RTSP...",
    "Starting Playback...",
    "Stream Ready"
};

/* Target progress percentage for each phase (0-100) */
static const int s_phase_progress[4] = { 25, 60, 90, 100 };

/* ------- shared state (written by main thread, read by render thread) ---- */
static volatile int  s_target_phase    = 0;
static volatile int  s_target_progress = 0;
static char          s_game_title[64]  = "Game";
static volatile int  s_running         = 0;
static SceUID        s_thread_id       = -1;

/* Current animated progress (fixed-point 8.8 for smooth sub-pixel sweep) */
static int s_current_fp = 0;  /* 0..25600 representing 0.00..100.00 */

/* -------------------------------------------------------------------------
 * Render thread — runs at display vsync (~60 fps)
 * ------------------------------------------------------------------------- */
static int connect_render_thread(SceSize args, void *argp)
{
    (void)args; (void)argp;

    while (s_running) {
        int target_fp = s_target_progress * 256;

        /* Smooth sweep: move toward target smoothly with sub-pixel precision */
        int diff = target_fp - s_current_fp;
        if (diff > 0) {
            int step = diff / 20;         /* slightly faster ease-out */
            if (step < 2) step = 2;       /* much smaller minimum step for pixel-perfection */
            s_current_fp += step;
            if (s_current_fp > target_fp)
                s_current_fp = target_fp;
        } else if (diff < 0) {
            s_current_fp = target_fp;     /* never slide backwards */
        }

        float display_pct = (float)s_current_fp / 256.0f;
        if (display_pct > 100.0f) display_pct = 100.0f;

        int phase = s_target_phase;
        if (phase < 0) phase = 0;
        if (phase > 3) phase = 3;

        const char *status = s_phase_labels[phase];

        ui_begin_frame();
        ui_draw_gradient_bg(UI_COL_BG_TOP, UI_COL_BG_BOT);
        ui_draw_header("Connecting to Stream");

        /* ---- Game title ---- */
        {
            float ty = 112.0f;
            ui_draw_text_centered(0.0f, (float)UI_SCREEN_W, ty,
                                  UI_COL_TEXT, s_game_title);
        }

        /* ---- Status text ---- */
        {
            float sy = 146.0f;
            ui_draw_text_centered(0.0f, (float)UI_SCREEN_W, sy,
                                  UI_COL_TEXT_DIM, status);
        }

        /* ---- Progress bar (same position as before) ---- */
        {
            int bar_x = 40;
            int bar_y = 172;
            int bar_w = UI_SCREEN_W - 80;
            int bar_h = 8;
            ui_draw_progress_bar(bar_x, bar_y, bar_w, bar_h,
                                 display_pct, 100.0f, NULL);
        }

        /* ---- Spinner animation ---- */
        ui_draw_spinner(UI_SCREEN_W / 2, 210, "");

        ui_end_frame();
    }

    sceKernelExitDeleteThread(0);
    return 0;
}

/* -------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */
void stream_connect_start(void)
{
    if (s_running) return;          /* already active */

    s_target_phase    = 0;
    s_target_progress = 0;
    s_current_fp      = 0;
    s_running         = 1;

    s_thread_id = sceKernelCreateThread("connect_ui",
                                        connect_render_thread,
                                        0x18,     /* priority */
                                        0x4000,   /* 16 KB stack */
                                        PSP_THREAD_ATTR_USER | PSP_THREAD_ATTR_VFPU,
                                        NULL);
    if (s_thread_id >= 0)
        sceKernelStartThread(s_thread_id, 0, NULL);
}

void stream_connect_draw(const char *game_title, int phase)
{
    if (phase < 0) phase = 0;
    if (phase > 3) phase = 3;

    /* Update shared state (atomic-ish on single-issue MIPS) */
    s_target_phase    = phase;
    s_target_progress = s_phase_progress[phase];

    if (game_title && game_title[0] != '\0') {
        strncpy(s_game_title, game_title, sizeof(s_game_title) - 1);
        s_game_title[sizeof(s_game_title) - 1] = '\0';
    }

    /* If the render thread isn't running, start it now (backwards compat) */
    if (!s_running)
        stream_connect_start();
}

void stream_connect_stop(void)
{
    if (!s_running) return;

    s_running = 0;
    if (s_thread_id >= 0) {
        sceKernelWaitThreadEnd(s_thread_id, NULL);
        sceKernelDeleteThread(s_thread_id);
        s_thread_id = -1;
    }
}
