/*
 * stream_session.c - Stream session management for PSP Moonlight
 *
 * Provides functions to properly end a streaming session and return to main menu.
 */

#include <pspkernel.h>
#include <psppower.h>
#include <pspnet_inet.h>
#include <pspthreadman.h>
#include <sys/socket.h>
#include <stdio.h>
#include <string.h>

#include "shared.h"
#include "hud.h"
#include "power_handler.h"
#include "signal_strength.h"
#include "safety_buffer.h"
#include "audio_thread.h"
#include "control_stream.h"
#include "diag_log.h"
#include "ui_manager.h"

/*============================================================================
 * External Variable and Function Declarations
 *============================================================================*/
extern volatile int me_running;
extern void sw_decoder_thread_shutdown(void);
extern void network_me_shutdown(void);
extern void rtsp_session_close(void);
extern void LiStopConnection(void);
extern void input_shutdown(void);
extern void network_wait_for_cancel_thread(void);
extern void wifi_keepalive_stop(void);
extern void wifi_keepalive_abort(void);
extern void wifi_disconnect(void);
extern void display_shutdown(void);
extern volatile int g_stream_status;

#define LOG_SESSION(fmt, ...) diag_log_write("SESSION", fmt, ##__VA_ARGS__)

static volatile int s_xmb_exit_in_progress = 0;

void exit_to_xmb(void);

/*============================================================================
 * Public API
 *============================================================================*/

void abort_stream_to_menu(void)
{
    LOG_SESSION("abort_stream_to_menu: STARTING CLEAN TEARDOWN\n");

    /* 1. Signal all threads to terminate first (Shared g_running/me_running flags) */
    me_running = 0;
    audio_thread_begin_shutdown();
    sceKernelDelayThread(100000); /* 100ms for threads to see flag */

    /* 2. Inform the server we are leaving via Moonlightcore */
    LOG_SESSION("[STEP 1/8] Terminating Connection (LiStopConnection)...\n");
    LiStopConnection();

    /* 3. Shut down input system */
    LOG_SESSION("[STEP 2/8] Shutting down input handlers...\n");
    input_shutdown();

    /* 4. Shut down networking subsystems (closes UDP/TCP sockets, joins threads) */
    LOG_SESSION("[STEP 3/8] Shutting down networking(UDP)...\n");
    network_me_shutdown();

    LOG_SESSION("[STEP 4/8] Shutting down control stream(TCP)...\n");
    control_stream_stop();

    /* 5. Shut down Audio pipeline */
    LOG_SESSION("[STEP 5/8] Shutting down audio/safety...\n");
    audio_thread_shutdown();
    safety_buffer_shutdown();

    /* 6. Wait for the server-side abort (HTTPS) to finish before hardware release */
    LOG_SESSION("[STEP 6/8] Waiting for server abort thread to finalize...\n");
    network_wait_for_cancel_thread();
    LOG_SESSION("[STEP 6/8] Server abort thread joined successfully.\n");

    /* 7. Shut down Software Video Decoder (ME + pipeline cleanup)
     * This is done LATE to ensure no network jitter or late packets. */
    LOG_SESSION("[STEP 7/8] Shutting down SW decoder (ME+pipeline)...\n");
    sw_decoder_thread_shutdown();

    /* 8. Utility subsystems */
    LOG_SESSION("[STEP 8/8] Releasing UI/Power utilities...\n");
    hud_shutdown();
    power_handler_shutdown();
    signal_strength_shutdown();

    /* 9. Finally, close the RTSP context entirely */
    LOG_SESSION("Closing RTSP session...\n");
    rtsp_session_close();

    LOG_SESSION("TEARDOWN COMPLETE. Returning to host discovery menu.\n");
    diag_log_flush();
    sceKernelDelayThread(50000); /* 50ms settling delay */
    g_stream_status = 0;
}

static int g_stream_input_socket = -1;
void stream_session_set_input_socket(int sock) { g_stream_input_socket = sock; }

void end_stream_session(void)
{
    exit_to_xmb();
}

void exit_to_xmb(void)
{
    if (s_xmb_exit_in_progress) {
        sceKernelExitGame();
        return;
    }

    s_xmb_exit_in_progress = 1;
    LOG_SESSION("exit_to_xmb: requested (me_running=%d stream_status=%d)\n",
                me_running, g_stream_status);

    if (me_running || g_stream_status != 0) {
        LOG_SESSION("exit_to_xmb: active stream detected; running stream teardown\n");
        abort_stream_to_menu();
    } else {
        LOG_SESSION("exit_to_xmb: no active stream; skipping stream teardown\n");
        hud_shutdown();
        power_handler_shutdown();
        signal_strength_shutdown();
        rtsp_session_close();
    }

    input_shutdown();

    /* App exit should not wait on the idle Wi-Fi monitor. HOME exits happen
     * under the XMB "Please wait" overlay, so force-stop the helper before
     * tearing down APCTL/INET. */
    wifi_keepalive_abort();

    wifi_disconnect();

    ui_manager_shutdown();
    display_shutdown();

    /* Final hardware power down */
    scePowerSetClockFrequency(222, 222, 111);

    LOG_SESSION("EXITING TO XMB...\n");
    diag_log_flush();
    sceKernelDelayThread(50000);
    sceKernelExitGame();
}
