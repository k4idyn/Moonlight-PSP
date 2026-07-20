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

extern volatile int me_running;
extern void sw_decoder_thread_shutdown(void);
extern void network_me_shutdown(void);
extern void network_me_abort(void);
extern void rtsp_session_close(void);
extern void input_shutdown(void);
extern int network_wait_for_cancel_thread(void);
extern void wifi_keepalive_abort(void);
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
    stream_connect_stop();
    wifi_keepalive_abort();
    scePowerSetClockFrequency(222, 222, 111);

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
    rtsp_session_close();
    network_me_abort();
    control_stream_abort();

    LOG_SESSION("[STEP 2/8] Shutting down input handlers...\n");
    diag_log_flush();
    input_shutdown();
    g_stream_input_socket = -1;

    LOG_SESSION("[STEP 3/8] Shutting down networking(UDP)...\n");
    diag_log_flush();
    network_me_shutdown();

    LOG_SESSION("[STEP 4/8] Shutting down control stream(TCP)...\n");
    diag_log_flush();
    control_stream_stop();

    LOG_SESSION("[STEP 5/8] Shutting down audio/safety...\n");
    diag_log_flush();
    audio_thread_shutdown();
    safety_buffer_shutdown();

    LOG_SESSION("[STEP 6/8] Waiting for server abort thread to finalize...\n");
    diag_log_flush();
    if (network_wait_for_cancel_thread()) {
        LOG_SESSION("[STEP 6/8] Server abort thread finalized.\n");
    } else {
        LOG_SESSION("[STEP 6/8] Server abort thread still active after bounded wait.\n");
    }
    diag_log_flush();

    LOG_SESSION("[STEP 7/8] Shutting down SW decoder (ME+pipeline)...\n");
    diag_log_flush();
    sw_decoder_thread_shutdown();
    LOG_SESSION("[STEP 7/8] SW decoder shutdown returned\n");
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
