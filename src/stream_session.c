/*
 * stream_session.c - Stream session management for PSP Moonlight
 */

#include <pspkernel.h>
#include <pspiofilemgr.h>
#include <pspmodulemgr.h>
#include <pspdisplay.h>
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
#include "stream_connect_ui.h"
#include "ui_manager.h"
#include "stream_crypto.h"
#include "psp_avc_build.h"
#if PSP_HARDWARE_AVC
#include "psp_avc_player.h"
#endif

extern volatile int me_running;
extern void sw_decoder_thread_shutdown(void);
extern void network_me_shutdown(void);
extern void network_me_abort(void);
extern int rtsp_session_close(void);
extern void network_cancel_stream_session(void);
extern void input_shutdown(void);
extern int network_wait_for_cancel_thread(void);
extern void wifi_disconnect(void);
extern void wifi_keepalive_stop(void);
extern void control_stream_abort(void);
extern void moonlight_main_mark_exitgame_pending(void);
extern volatile int g_stream_status;

#define LOG_SESSION(fmt, ...) diag_log_write("SESSION", fmt, ##__VA_ARGS__)

static volatile int s_xmb_exit_in_progress = 0;
static volatile int s_process_exit_cleanup_in_progress = 0;
static volatile int s_process_exit_cleanup_done = 0;
static volatile int s_stream_teardown_in_progress = 0;
static volatile int s_stream_teardown_complete = 0;

static int g_stream_input_socket = -1;

void abort_stream_to_menu(void);
int exit_to_xmb(void);

int moonlight_process_exit_in_progress(void)
{
    return s_xmb_exit_in_progress ||
           s_process_exit_cleanup_in_progress ||
           s_process_exit_cleanup_done;
}

int moonlight_prepare_process_exit(void)
{
    int stream_already_torn_down;

    if (s_process_exit_cleanup_done) {
        return 1;
    }

    if (s_process_exit_cleanup_in_progress) {
        LOG_SESSION("process-exit cleanup already in progress; refusing duplicate exit\n");
        diag_log_flush();
        return 0;
    }

    s_process_exit_cleanup_in_progress = 1;
    LOG_SESSION("process-exit cleanup begin (me_running=%d stream_status=%d)\n",
                me_running, g_stream_status);
    diag_log_flush();

    if (me_running || g_stream_status != 0) {
        LOG_SESSION("process-exit cleanup: active stream teardown\n");
        diag_log_flush();
        if (s_stream_teardown_in_progress) {
            LOG_SESSION("process-exit cleanup blocked: stream teardown already in progress\n");
            diag_log_flush();
            s_process_exit_cleanup_in_progress = 0;
            return 0;
        }
        abort_stream_to_menu();
    }

    stream_already_torn_down = (s_stream_teardown_complete &&
                                !me_running && g_stream_status == 0);

    if (stream_already_torn_down) {
        LOG_SESSION("process-exit cleanup: stream subsystems already torn down\n");
        diag_log_flush();
    } else if (me_running || g_stream_status != 0) {
        LOG_SESSION("process-exit cleanup blocked: stream teardown incomplete (me_running=%d stream_status=%d complete=%d)\n",
                    me_running, g_stream_status, s_stream_teardown_complete);
        diag_log_flush();
        s_process_exit_cleanup_in_progress = 0;
        return 0;
    } else {
        LOG_SESSION("process-exit cleanup: no active stream\n");
        diag_log_flush();
        hud_shutdown();
        power_handler_shutdown();
        signal_strength_shutdown();
        rtsp_session_close();
        input_shutdown();
        audio_thread_begin_shutdown();
        network_me_abort();
        control_stream_abort();
        audio_thread_shutdown();
        safety_buffer_shutdown();
    }

    if (!network_wait_for_cancel_thread()) {
        LOG_SESSION("process-exit cleanup blocked: server abort thread still active\n");
        diag_log_flush();
        s_process_exit_cleanup_in_progress = 0;
        return 0;
    }

    moonlight_main_mark_exitgame_pending();

    LOG_SESSION("process-exit cleanup: final lightweight handoff cleanup\n");
    diag_log_flush();
#if PSP_HARDWARE_AVC
    /* Host discovery can own a newly warmed session after stream teardown.
     * Release it even when the previous stream is already marked complete. */
    if (psp_avc_player_is_open()) {
        int destroy_result;
        LOG_SESSION("process-exit cleanup: destroying idle AVC player\n");
        diag_log_flush();
        sw_decoder_thread_shutdown();
        /* Stream teardown uses close() and retains the large frame pool for
         * the next host session. Final process exit must release that pool
         * and its semaphore or is_open() remains true and blocks the XMB
         * handoff despite the Sony firmware session already being closed. */
        destroy_result = psp_avc_player_destroy();
        if (destroy_result != 0) {
            LOG_SESSION("process-exit cleanup blocked: AVC player destroy failed=%08X\n",
                        (unsigned)destroy_result);
            diag_log_flush();
            s_process_exit_cleanup_in_progress = 0;
            return 0;
        }
        if (psp_avc_player_is_open()) {
            LOG_SESSION("process-exit cleanup blocked: AVC player resources remain\n");
            diag_log_flush();
            s_process_exit_cleanup_in_progress = 0;
            return 0;
        }
    }
#endif
    stream_connect_stop();
    LOG_SESSION("process-exit cleanup: stopping Wi-Fi keepalive gracefully\n");
    diag_log_flush();
    wifi_keepalive_stop();
    stream_crypto_shutdown();
    LOG_SESSION("process-exit cleanup: Wi-Fi keepalive stopped\n");
    diag_log_flush();
    LOG_SESSION("process-exit cleanup: disconnecting and releasing Wi-Fi stack\n");
    diag_log_flush();
    wifi_disconnect();
    LOG_SESSION("process-exit cleanup: Wi-Fi stack released\n");
    diag_log_flush();
    /*
     * Do not change the CPU/bus clock during the final PSPLink handoff.
     * The clock service can block after the stream has released WLAN and
     * the keepalive thread, leaving the app stuck on host discovery before
     * sceKernelExitGame() gets a chance to return control to PSPLink.
     * The kernel/loader will restore the normal clock after process exit.
     */
    LOG_SESSION("process-exit cleanup: leaving clock unchanged for PSPLink handoff\n");
    diag_log_flush();

    g_stream_status = 0;
    me_running = 0;

    LOG_SESSION("process-exit cleanup complete\n");
    diag_log_flush();
    sceKernelDelayThread(50000);
    s_process_exit_cleanup_done = 1;
    s_process_exit_cleanup_in_progress = 0;
    return 1;
}

int moonlight_exit_process_now(void)
{
    if (s_xmb_exit_in_progress) {
        return 0;
    }

    s_xmb_exit_in_progress = 1;
    LOG_SESSION("process exit: cleanup before final app-close handoff (me_running=%d stream_status=%d)\n",
                me_running, g_stream_status);
    diag_log_flush();

    if (!moonlight_prepare_process_exit()) {
        LOG_SESSION("process exit: blocked because stream teardown is incomplete\n");
        diag_log_flush();
        s_xmb_exit_in_progress = 0;
        return 0;
    }

    LOG_SESSION("process exit: cleanup ready; main thread owns final app-close handoff\n");
    diag_log_flush();
    return 1;
}

void stream_session_set_input_socket(int sock)
{
    g_stream_input_socket = sock;
}

void abort_stream_to_menu(void)
{
    int rtsp_teardown_ok;

    if (s_stream_teardown_in_progress) {
        LOG_SESSION("abort_stream_to_menu: duplicate teardown request ignored while active\n");
        diag_log_flush();
        return;
    }

    s_stream_teardown_in_progress = 1;
    s_stream_teardown_complete = 0;
    LOG_SESSION("abort_stream_to_menu: STARTING CLEAN TEARDOWN\n");
    diag_log_flush();

    me_running = 0;
    audio_thread_begin_shutdown();
    sceKernelDelayThread(20000);

    LOG_SESSION("[STEP 1/8] Terminating RTSP session and aborting live sockets for teardown...\n");
    diag_log_flush();
    rtsp_teardown_ok = rtsp_session_close();

    /* Keep the control socket alive long enough to send the ENet DISCONNECT.
     * The old order called control_stream_abort() first, which closed the
     * socket and made the later control_stream_stop() a no-op.  Apollo then
     * saw only a dead client and could leave its capture/encoder session
     * hanging until its 10-second watchdog fired. */
    control_stream_stop();
    network_me_abort();

    LOG_SESSION("[STEP 2/8] Shutting down input handlers...\n");
    diag_log_flush();
    input_shutdown();
    g_stream_input_socket = -1;

    LOG_SESSION("[STEP 3/8] Shutting down networking(UDP)...\n");
    diag_log_flush();
    network_me_shutdown();

    LOG_SESSION("[STEP 4/8] Shutting down control stream(TCP)...\n");
    diag_log_flush();
    /* control_stream_stop() was completed before the UDP shutdown so the
     * graceful ENet DISCONNECT could use its live socket. */

    LOG_SESSION("[STEP 5/8] Shutting down audio/safety...\n");
    diag_log_flush();
    audio_thread_shutdown();
    stream_crypto_shutdown();
    safety_buffer_shutdown();

#if PSP_HARDWARE_AVC
    LOG_SESSION("[STEP 6/8] Shutting down Sony AVC video worker...\n");
#else
    LOG_SESSION("[STEP 6/8] Shutting down OpenH264 software decoder and Media Engine...\n");
#endif
    diag_log_flush();
    sw_decoder_thread_shutdown();
#if PSP_HARDWARE_AVC
    LOG_SESSION("[STEP 6/8] Sony AVC video worker shutdown returned\n");
#else
    LOG_SESSION("[STEP 6/8] SW decoder shutdown returned\n");
#endif
    diag_log_flush();

    LOG_SESSION("[STEP 7/8] Waiting for server abort thread to finalize...\n");
    diag_log_flush();
    /* A complete RTSP TEARDOWN already ends the active host session. Avoid
     * opening a second TLS connection on this normal path: its handshake can
     * outlive the stream teardown and block the main PSP thread. If RTSP could
     * not be sent, attempt the authenticated fallback asynchronously and
     * never force-kill its worker from inside TLS. */
    if (rtsp_teardown_ok) {
        LOG_SESSION("[STEP 7/8] RTSP TEARDOWN sent; authenticated cancel not needed.\n");
    } else {
        LOG_SESSION("[STEP 7/8] RTSP TEARDOWN unconfirmed; starting authenticated fallback.\n");
        network_cancel_stream_session();
        if (network_wait_for_cancel_thread()) {
            LOG_SESSION("[STEP 7/8] Server abort thread finalized.\n");
        } else {
            LOG_SESSION("[STEP 7/8] Authenticated cancel remains asynchronous; local teardown continues.\n");
        }
    }
    diag_log_flush();

    LOG_SESSION("[STEP 8/8] Releasing UI/Power utilities...\n");
    diag_log_flush();
    hud_shutdown();
    power_handler_shutdown();
    signal_strength_shutdown();

    g_stream_status = 0;
    me_running = 0;
    s_stream_teardown_complete = 1;
    s_stream_teardown_in_progress = 0;

    LOG_SESSION("TEARDOWN COMPLETE. Returning to host discovery menu\n");
    diag_log_flush();
    sceKernelDelayThread(50000);
}

void end_stream_session(void)
{
    (void)exit_to_xmb();
}

int exit_to_xmb(void)
{
    return moonlight_exit_process_now();
}
