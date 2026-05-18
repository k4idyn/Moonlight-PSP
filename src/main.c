/*
 * main.c - Entry point for PSP Moonlight streaming client
 */

#include <pspkernel.h>
#include <pspdebug.h>
#include <pspthreadman.h>
#include <pspctrl.h>
#include <psppower.h>
#include <pspdisplay.h>
#include <pspgu.h>
#include <pspnet.h>
#include <pspnet_inet.h>
#include <pspnet_apctl.h>
#include <pspiofilemgr.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <mbedtls/memory_buffer_alloc.h>

#include "shared.h"
#include "sw_decode_pipeline.h"
#include "pairing_pin_ui.h"
#include "settings_menu.h"
#include "config.h"
#include "hud.h"
#include "stream_session.h"
#include "power_handler.h"
#include "signal_strength.h"
#include "safety_buffer.h"
#include "ui_manager.h"
#include "netconf_ui.h"
#include "host_discovery.h"
#include "audio_thread.h"
#include "control_stream.h"
#include "stream_crypto.h"
#include "moonlight_proto.h"
#include "client_identity.h"
#include "rtp_fec.h"
#include "rtp_reassembly.h"
#include "runtime_telemetry.h"
#include "network_me_stats.h"

PSP_MODULE_INFO("PSPMoonlight", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);
PSP_MAIN_THREAD_STACK_SIZE_KB(256);
PSP_HEAP_SIZE_KB(12 * 1024);

/* All logging unified into ms0:/moonlight.log */

PspConfig g_psp_config;
SharedState g_shared;
static volatile int g_running = 1;
volatile int me_running = 0;
volatile int g_is_paired = 0;
volatile int g_stream_status = 0;

/* Remote input via pspsh pokew — write PSP_CTRL_ bitmask to this address */
volatile unsigned int g_remote_buttons = 0;

/* Remote analog injection for hardware automation. Values are PSP analog
 * coordinates (0..255, center 128). Disabled during normal user control. */
volatile unsigned int g_remote_analog_active = 0;
volatile unsigned int g_remote_analog_lx = 128;
volatile unsigned int g_remote_analog_ly = 128;

/* Decode pause flag — pspsh pokew to pause decode threads so psplink can
 * service USB commands (scrshot etc.) without CPU starvation.
 * Set to 1 to pause, 0 to resume. Logged address at startup. */
volatile int g_decode_paused = 0;

/* Remote exit request — pspsh pokew 1 to immediately exit stream back to menu.
 * Avoids needing to hold Start+Select for 500ms, which is impossible via pokew. */
volatile unsigned int g_remote_exit_request = 0;

/* Automation readiness flag — set when the first decoded frame reaches the
 * display path.  The test harness polls this tiny word instead of repeatedly
 * copying moonlight.log while startup video/audio are trying to stabilize. */
volatile unsigned int g_stream_ready_flag = 0;

/* External declarations */
extern int  wifi_connect(void);
extern void wifi_disconnect(void);
extern void wifi_launch_disable_power_save(void);
extern void wifi_keepalive_start(void);
extern void wifi_keepalive_stop(void);
extern int  network_connect_all(void);
extern void network_set_target_host(const char *host_ip);
extern void network_restore_paired_host(const char *paired_ip);
extern void network_set_local_bind_ip(const char *ip);
extern void network_connect_clear_retry_app(void);
extern int  network_auto_bind_for_loopback(const char *target_ip);
extern const char *network_get_paired_host(void);
extern char g_video_server_ip[64];
extern void network_me_init(PacketRingBuffer *rb);
extern void network_me_shutdown(void);
extern void network_me_abort(void);
extern void control_stream_abort(void);
extern void rtsp_session_close(void);
extern void display_init(void);
extern void display_frame(void *frame_data);
extern void display_frame_finish(void);
extern void display_frame_repeat(void);
extern void display_clear(unsigned int color);
extern void display_shutdown(void);
extern int  sw_decoder_thread_init(FrameRingBuffer *frame_rb);
extern void sw_decoder_thread_shutdown(void);
extern void sw_decoder_thread_force_restart(void);
extern int  decoder_is_cabac_detected(void);
extern volatile int g_cabac_dialog_active;
extern void oh264_pipeline_flush_buffers(void);
extern int  safety_buffer_init(void);
extern void safety_buffer_shutdown(void);
extern void input_init(int sock);
extern void input_poll_and_send(void);
extern void input_set_destination(const char *host_ip);
extern void hud_init(void);
extern void hud_update_stats(const HudStats *stats);
extern void hud_render(void);
extern int  hud_handle_input(u32 buttons);
extern int  hud_is_visible(void);
extern int  hud_overlay_visible(void);

/* Watchdog state — file-scope so both frame-display and idle paths can access */
static int s_watchdog_restarts = 0;
static int s_mode_b_soft_count = 0;
static SceSize s_stream_ram_start_free = 0;
static SceSize s_stream_ram_start_largest = 0;
extern void hud_shutdown(void);
extern void abort_stream_to_menu(void);

/* Decode-to-display latency timestamp (written by sw_decoder_thread) */
extern volatile u32 g_last_frame_decode_us;

/* Removed legacy logging init */

#include "diag_log.h"
#include "decode_flags.h"

static int g_gu_active = 0;
#define PSP_MBEDTLS_HEAP_BYTES (1024 * 1024)
static unsigned char s_mbedtls_heap[PSP_MBEDTLS_HEAP_BYTES] __attribute__((aligned(16)));
static int s_mbedtls_heap_ready = 0;

static void LOG(const char *fmt, ...) {
#ifdef RETAIL_BUILD
    (void)fmt;
#else
    char buf[512]; va_list args; va_start(args, fmt); vsnprintf(buf, sizeof(buf), fmt, args); va_end(args);
    diag_log_write("MAIN", "%s", buf);
    if (!g_gu_active) pspDebugScreenPrintf("%s", buf);
#endif
}

static void halt_with_error(const char *step_name, int error_code) {
    SceCtrlData pad; sceGuTerm(); g_gu_active = 0; pspDebugScreenInit();
#ifdef RETAIL_BUILD
    (void)step_name;
    pspDebugScreenPrintf("Moonlight error\nCode: 0x%08X\nPress any button to exit...\n",
                         (unsigned int)error_code);
#else
    LOG("\n=== FATAL ERROR ===\nStep : %s\nCode : 0x%08X (%d)\nPress any button to exit...\n", step_name, (unsigned int)error_code, error_code);
#endif
    sceCtrlSetSamplingCycle(0); sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);
    while (1) { sceCtrlPeekBufferPositive(&pad, 1); if (pad.Buttons != 0) break; sceKernelDelayThread(50 * 1000); }
    sceKernelExitGame();
}

static void setup_callbacks(void);

int main(int argc, char *argv[]) {
    int ret; int skip_rescan = 0; char selected_host_ip[16] = {0}; HostPC *selected_host = NULL;
    setup_callbacks();
    diag_log_clear();
    if (!s_mbedtls_heap_ready) {
        mbedtls_memory_buffer_alloc_init(s_mbedtls_heap, sizeof(s_mbedtls_heap));
        s_mbedtls_heap_ready = 1;
    }
    pspDebugScreenInit();
    ret = scePowerSetClockFrequency(333, 333, 166);
    diag_log_write("MAIN", "[REMOTE] g_remote_buttons at 0x%08X\n", (unsigned int)&g_remote_buttons);
    diag_log_write("MAIN", "[REMOTE] g_remote_analog_active at 0x%08X\n", (unsigned int)&g_remote_analog_active);
    diag_log_write("MAIN", "[REMOTE] g_remote_analog_lx at 0x%08X\n", (unsigned int)&g_remote_analog_lx);
    diag_log_write("MAIN", "[REMOTE] g_remote_analog_ly at 0x%08X\n", (unsigned int)&g_remote_analog_ly);
    diag_log_write("MAIN", "[REMOTE] g_decode_paused at 0x%08X\n", (unsigned int)&g_decode_paused);
    diag_log_write("MAIN", "[REMOTE] g_remote_exit_request at 0x%08X\n", (unsigned int)&g_remote_exit_request);
    diag_log_write("MAIN", "[REMOTE] g_stream_ready_flag at 0x%08X\n", (unsigned int)&g_stream_ready_flag);
    diag_log_flush();  /* Force flush so automation can read addresses immediately */
    /* No-op legacy log_open removed */
    sceDisplaySetMode(0, 480, 272);
    display_init(); g_gu_active = 1;
    ret = ui_manager_init();
    if (ret < 0) { LOG("[STEP 0d] ui_manager_init() failed\n"); halt_with_error("UI Manager", ret); return ret; }
    LOG("[PROTO] generation=%d, clientVersion=%d\n", MOONLIGHT_PROTOCOL_GENERATION, MOONLIGHT_CLIENT_VERSION);
    diag_log_flush();
    ret = client_identity_ensure(NULL);
    if (ret < 0) { halt_with_error("Identity", ret); return ret; }

    ui_begin_frame(); ui_draw_gradient_bg(UI_COL_BG_TOP, UI_COL_BG_BOT); ui_draw_header("PSP Moonlight");
    ui_draw_text_centered(0.0f, 480.0f, 130.0f, UI_COL_TEXT, "Initialising..."); ui_end_frame();

settings_menu_entry:
    LOG("[STEP 1] Loading settings...\n");
    diag_log_write("UI", "TRANSITION settings_menu_init t=%u\n", sceKernelGetSystemTimeLow() / 1000);
    diag_log_flush();
    settings_menu_init(&g_psp_config);
    if (g_psp_config.pairedHostCount > 0 && g_psp_config.pairedHostIps[0][0] != '\0')
        network_restore_paired_host(g_psp_config.pairedHostIps[0]);
    network_set_local_bind_ip(g_psp_config.localBindIp);
    if (settings_menu_run(&g_psp_config) < 0) LOG("[STEP 1] Menu cancelled\n");
    diag_log_write("UI", "TRANSITION settings_done t=%u\n", sceKernelGetSystemTimeLow() / 1000);
    diag_log_flush();

    LOG("[STEP 2] Connecting Wi-Fi...\n");
    diag_log_write("UI", "TRANSITION wifi_start t=%u\n", sceKernelGetSystemTimeLow() / 1000);
    diag_log_flush();
    { /* Skip netconf dialog if WiFi is already connected (back-navigation) */
        int apctl_state = 0;
        sceNetApctlGetState(&apctl_state);
        if (apctl_state != 4) {
            ret = netconf_ui_run();
            diag_log_write("UI", "NETCONF returned %d\n", ret);
            if (ret < 0) { halt_with_error("Wi-Fi", -1); return -1; }
        } else {
            diag_log_write("UI", "WiFi already connected, skipping netconf\n");
        }
        wifi_launch_disable_power_save();
        wifi_keepalive_start();
    }
    diag_log_write("UI", "TRANSITION wifi_done t=%u\n", sceKernelGetSystemTimeLow() / 1000);
    diag_log_flush();

host_select_loop:
    diag_log_write("UI", "TRANSITION host_discovery_start t=%u\n", sceKernelGetSystemTimeLow() / 1000);
    diag_log_flush();
    if (!skip_rescan) host_discovery_init();
    skip_rescan = 0;
    while (1) {
        int host_ret = renderHostDiscoveryList();
        if (host_ret == -2) {
            /* Back button: return to settings menu */
            diag_log_write("UI", "HOST back_to_settings t=%u\n", sceKernelGetSystemTimeLow() / 1000);
            host_discovery_shutdown();
            goto settings_menu_entry;
        }
        if (host_ret >= 0) { selected_host = host_discovery_get_selected(); break; }
    }
    if (!selected_host) { halt_with_error("Host Selection", -1); return -1; }
    snprintf(selected_host_ip, sizeof(selected_host_ip), "%s", selected_host->ip);
    network_set_target_host(selected_host_ip);
    /* Trust the server's <PairStatus> from the HTTP probe rather than
     * relying solely on the single paired_host_ip in config.  This lets
     * the PSP work with multiple paired hosts without re-pairing. */
    if (selected_host->paired) g_is_paired = 1;
    diag_log_write("UI", "TRANSITION host_selected ip=%s t=%u\n", selected_host_ip, sceKernelGetSystemTimeLow() / 1000);
    config_add_manual_host(selected_host_ip, selected_host->mac);

    LOG("[STEP 4] Connecting to %s...\n", selected_host_ip);
    diag_log_write("UI", "TRANSITION connect_start t=%u\n", sceKernelGetSystemTimeLow() / 1000);
    g_stream_ready_flag = 0;
    { /* Connection with auto-retry: after cancel+relaunch the server's
       * RTSP listener may not be ready yet.  Two automatic retries with a
       * 5-second backoff avoid dropping the user back to host select. */
        int connect_attempts = 0;
        const int MAX_CONNECT_ATTEMPTS = 3;
        while (1) {
            ret = network_connect_all();
            connect_attempts++;
            if (ret >= 0) break;
            if (ret == -2) break;  /* User cancelled — don't retry */
            if (connect_attempts >= MAX_CONNECT_ATTEMPTS) break;
            LOG("[STEP 4] Connection attempt %d failed (%d), retrying in 5s...\n",
                connect_attempts, ret);
            diag_log_write("MAIN", "Connection attempt %d failed (%d), retrying...\n",
                           connect_attempts, ret);
            network_me_shutdown();
            control_stream_stop();
            audio_thread_shutdown();
            rtsp_session_close();
            sceKernelDelayThread(5000 * 1000);
        }
    }
    diag_log_write("UI", "TRANSITION connect_done ret=%d t=%u\n", ret, sceKernelGetSystemTimeLow() / 1000);
    /* Persist pairing as soon as it succeeds — even if RTSP/launch failed
     * later (ret == -3).  Without this, restarting the app after a partial
     * failure would lose the pairing and prompt a new PIN. */
    if (g_is_paired) {
        const char *ph = network_get_paired_host();
        if (ph && ph[0]) { config_add_paired_host(&g_psp_config, ph); }
    }
    if (ret < 0) {
        LOG("[STEP 4] Connection failed (%d)\n", ret);
        network_me_shutdown();
        control_stream_stop();
        audio_thread_shutdown();
        rtsp_session_close();
        network_connect_clear_retry_app();
        skip_rescan = 1;  /* Keep cached host list — user can Square to rescan */
        goto host_select_loop;
    }

    extern unsigned char g_remote_input_key[16];
    stream_crypto_init(g_remote_input_key);
    s_stream_ram_start_free = sceKernelTotalFreeMemSize();
    s_stream_ram_start_largest = sceKernelMaxFreeMemSize();
    diag_log_write("MAIN", "RAM stream-start free=%uK largest=%uK\n",
                   (unsigned)(s_stream_ram_start_free / 1024),
                   (unsigned)(s_stream_ram_start_largest / 1024));

    extern int g_audio_rtsp_ok;
    if (g_audio_rtsp_ok) {
        int audio_ret;
        diag_log_write("MAIN", "Initializing audio thread...\n");
        audio_ret = audio_thread_init(selected_host_ip);
        if (audio_ret < 0) {
            diag_log_write("MAIN", "Audio init failed (%d); aborting audio-required stream\n",
                           audio_ret);
            diag_log_flush();
            audio_thread_shutdown();
            rtsp_session_close();
            network_connect_clear_retry_app();
            skip_rescan = 1;
            goto host_select_loop;
        }
    } else {
        diag_log_write("MAIN", "Skipping audio init (RTSP audio SETUP failed)\n");
    }

    diag_log_write("MAIN", "Initializing shared memory (~%uKB)...\n",
                   (unsigned)((sizeof(g_shared) + 1023) / 1024));
    memset(&g_shared, 0, sizeof(g_shared));

    diag_log_write("MAIN", "Initializing network ME (D-UDP)...\n");
    diag_log_flush();
    network_me_init(&g_shared.packet_ring);
    diag_log_write("MAIN", "network_me_init done.\n");
    diag_log_flush();

    diag_log_write("MAIN", "Initializing SW decoder (CAVLC+VFPU dual-core)...\n");
    diag_log_flush();
    { g_cabac_detected = 0;
      g_cabac_dialog_active = 0;
    }
    ret = sw_decoder_thread_init(&g_shared.frame_ring);
    if (ret < 0) {
        diag_log_write("MAIN", "SW Decoder Init failed: %d\n", ret);
        halt_with_error("SW Decoder Init", ret); return ret;
    }
    me_running = 1;
    diag_log_write("MAIN", "Threads ready.\n");
    diag_log_flush();

    extern int g_decoder_ready;
    g_decoder_ready = 1;
    ret = control_stream_start();
    if (ret < 0) {
        LOG("[STEP 5] Control stream failed (%d)\n", ret);
        diag_log_write("MAIN", "[STEP 5] Control stream failed (%d); tearing down session before runtime loop\n", ret);
        diag_log_flush();
        g_decoder_ready = 0;
        abort_stream_to_menu();
        memset(&g_shared, 0, sizeof(g_shared));
        skip_rescan = 1;
        goto host_select_loop;
    }

    diag_log_write("MAIN", "Control stream started. Entering main loop.\n");
    diag_log_flush();  /* Flush all handshake/setup logs to disk */
    safety_buffer_init();
    hud_init();
    telemetry_reset();
    {
        int signal_bitrate_kbps = signal_strength_get_launch_bitrate_kbps(g_psp_config.bitrate);
        signal_strength_init(signal_bitrate_kbps);
    }
    power_handler_init();

    int input_socket = sceNetInetSocket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (input_socket >= 0) {
        input_init(input_socket);
        input_set_destination(g_video_server_ip[0] ? g_video_server_ip : selected_host_ip);
        stream_session_set_input_socket(input_socket);
    }

    u32 stream_wait_start = sceKernelGetSystemTimeLow() / 1000;
    while (g_running) {
        if (!me_running) {
            SceCtrlData disc_pad;
            ui_begin_frame();
            ui_draw_gradient_bg(UI_COL_BG_TOP, UI_COL_BG_BOT);
            ui_draw_header("PSP Moonlight");

            const char *status_text = (g_stream_status == 2) ? "Stream Paused" : "Stream Stopped";
            ui_draw_text_centered(0.0f, 480.0f, 130.0f, UI_COL_TEXT, status_text);
            ui_draw_text_centered(0.0f, 480.0f, 166.0f, UI_COL_TEXT_DIM, "Press Circle to return to menu");
            ui_end_frame();

            sceCtrlPeekBufferPositive(&disc_pad, 1);
            disc_pad.Buttons |= g_remote_buttons; g_remote_buttons = 0;
            if (disc_pad.Buttons & (PSP_CTRL_CIRCLE | PSP_CTRL_CROSS)) {
                /* Cleanly shutdown the current streaming session before returning to host menu */
                g_stream_status = 0;

                /* Show immediate "Disconnecting..." feedback so user knows it's working */
                ui_begin_frame();
                ui_draw_gradient_bg(UI_COL_BG_TOP, UI_COL_BG_BOT);
                ui_draw_header("PSP Moonlight");
                ui_draw_text_centered(0.0f, 480.0f, 140.0f, UI_COL_TEXT, "Disconnecting...");
                ui_end_frame();

                /* Debounce: wait for button release to prevent re-trigger in host menu */
                while (1) {
                    SceCtrlData rel;
                    sceCtrlPeekBufferPositive(&rel, 1);
                    if (!(rel.Buttons & (PSP_CTRL_CIRCLE | PSP_CTRL_CROSS))) break;
                    sceKernelDelayThread(16000);
                }

                abort_stream_to_menu();

                /* Reset shared ring state so a fresh stream can re-initialize */
                memset(&g_shared, 0, sizeof(g_shared));

                skip_rescan = 1;  /* Quick relaunch: skip host re-probe */
                goto host_select_loop;
            }
            sceKernelDelayThread(1000);
            continue;
        }

        SceCtrlData pad; void *frame = NULL;
        /* Race-safe ring buffer drain: rely solely on head != tail.
         * The old 'frame_ready' flag had a TOCTOU race: consumer could
         * clear it to 0 right after producer set it to 1, losing a frame
         * and—if the producer was between IDRs—freezing the display. */
        {
            u32 tail = g_shared.frame_ring.tail;
            u32 head = g_shared.frame_ring.head;
            if (tail != head) {
                /* Skip to newest frame — reduces latency when decoder is behind. */
                u32 queued = (head + FRAME_RING_SLOTS - tail) % FRAME_RING_SLOTS;
                while (queued > 1) {
                    tail = (tail + 1) % FRAME_RING_SLOTS;
                    queued--;
                }
                frame = g_shared.frame_ring.frame_data[tail];
                g_shared.frame_ring.tail = (tail + 1) % FRAME_RING_SLOTS;
            }
        }

        static int video_started = 0;
        static int s_disp_count = 0;
        static int s_idle_count = 0;
        static u32 s_fps_last_us = 0;
        static int s_fps_frame_count = 0;
        static float s_display_fps = 0.0f;
        static int s_latency_avg_ms = 0;

        /* CABAC warning dialog: show once per stream when CABAC entropy
         * coding is detected.  User can accept the risk (choppy decode)
         * or go back to the app list to switch encoder settings. */
        {
            static int s_cabac_choice = 0; /* 0=not shown, 1=continue, 2=back */
            static int s_cabac_prompt_logged = 0;
            static u32 s_cabac_notice_start_us = 0;

            /* Auto-reset when a new stream starts (g_cabac_detected resets to 0
             * in sw_decoder_thread_init, so the first frames have it == 0). */
            if (!decoder_is_cabac_detected()) {
                s_cabac_choice = 0;
                s_cabac_prompt_logged = 0;
                s_cabac_notice_start_us = 0;
            }

            if (decoder_is_cabac_detected() && !s_cabac_prompt_logged) {
                diag_log_write("CABAC", "CABAC detected while cabacTestMode=%d -- showing warning dialog\n",
                               g_psp_config.cabacTestMode ? 1 : 0);
                s_cabac_prompt_logged = 1;
            }

            if (decoder_is_cabac_detected() && s_cabac_choice == 0) {
                /* Gate decoder + audio: stop processing until user confirms */
                g_cabac_dialog_active = 1;

                if (!g_psp_config.cabacTestMode) {
                    u32 now_us = sceKernelGetSystemTimeLow();
                    if (s_cabac_notice_start_us == 0) {
                        s_cabac_notice_start_us = now_us;
                    }

                    ui_begin_frame();
                    ui_draw_gradient_bg(UI_COL_BG_TOP, UI_COL_BG_BOT);
                    ui_draw_header("PSP Moonlight");

                    {
                        int pw = 360, ph = 120;
                        int px = (UI_SCREEN_W - pw) / 2;
                        int py = (UI_SCREEN_H - ph) / 2 - 10;
                        int bd = 2;
                        ui_set_blend(1);
                        ui_draw_rect_rounded(px, py, pw, ph, 12, UI_COL_BORDER_FOC);
                        ui_draw_rect_rounded(px + bd, py + bd, pw - 2*bd, ph - 2*bd, 12 - bd, UI_COL_PANEL_DARK);
                        ui_set_blend(0);

                        ui_draw_text_centered((float)px, (float)pw, (float)(py + 30), UI_COL_TEXT, "CABAC Encoding Detected");
                        ui_draw_text_centered((float)px, (float)pw, (float)(py + 54), UI_COL_TEXT_DIM, "Server is using unsupported CABAC entropy coding.");
                        ui_draw_text_centered((float)px, (float)pw, (float)(py + 72), UI_COL_TEXT_DIM, "PSP v1.0 requires CAVLC for stable playback.");
                        ui_draw_text_centered((float)px, (float)pw, (float)(py + 90), UI_COL_TEXT_DIM, "Returning to the host menu. Fix the host encoder and retry.");
                    }

                    ui_draw_footer_hint("Normal mode rejects CABAC streams");
                    ui_end_frame();

                    if ((now_us - s_cabac_notice_start_us) < 1200000) {
                        sceKernelDelayThread(50 * 1000);
                    } else {
                        diag_log_write("CABAC", "CABAC in normal mode -- auto-aborting stream\n");
                        s_cabac_choice = 2;
                    }
                } else {

                    /* Drain any stale button presses before accepting input */
                    {
                        SceCtrlData drain;
                        do {
                            sceCtrlPeekBufferPositive(&drain, 1);
                            sceKernelDelayThread(16 * 1000);
                        } while (drain.Buttons & (PSP_CTRL_CROSS | PSP_CTRL_CIRCLE));
                    }

                    {
                        SceCtrlData cpd, cprev;
                        memset(&cprev, 0, sizeof(cprev));

                        while (1) {
                            sceCtrlPeekBufferPositive(&cpd, 1);
                            cpd.Buttons |= g_remote_buttons; g_remote_buttons = 0;

                            ui_begin_frame();
                            ui_draw_gradient_bg(UI_COL_BG_TOP, UI_COL_BG_BOT);
                            ui_draw_header("PSP Moonlight");

                            int pw = 360, ph = 120;
                            int px = (UI_SCREEN_W - pw) / 2;
                            int py = (UI_SCREEN_H - ph) / 2 - 10;
                            int bd = 2;
                            ui_set_blend(1);
                            ui_draw_rect_rounded(px, py, pw, ph, 12, UI_COL_BORDER_FOC);
                            ui_draw_rect_rounded(px + bd, py + bd, pw - 2*bd, ph - 2*bd, 12 - bd, UI_COL_PANEL_DARK);
                            ui_set_blend(0);

                            ui_draw_text_centered((float)px, (float)pw, (float)(py + 30), UI_COL_TEXT, "CABAC Encoding Detected");
                            ui_draw_text_centered((float)px, (float)pw, (float)(py + 54), UI_COL_TEXT_DIM, "Server is using CABAC entropy coding.");
                            ui_draw_text_centered((float)px, (float)pw, (float)(py + 72), UI_COL_TEXT_DIM, "PSP v1.0 requires CAVLC for stable playback.");
                            ui_draw_text_centered((float)px, (float)pw, (float)(py + 90), UI_COL_TEXT_DIM, "Continue only for testing, or go back and fix the host encoder.");

                            ui_draw_footer_hint("{X}: Continue Anyway    {O}: Back");
                            ui_end_frame();

                            int cx = (cpd.Buttons & PSP_CTRL_CROSS) && !(cprev.Buttons & PSP_CTRL_CROSS);
                            int co = (cpd.Buttons & PSP_CTRL_CIRCLE) && !(cprev.Buttons & PSP_CTRL_CIRCLE);

                            if (cx) {
                                diag_log_write("CABAC", "User chose CONTINUE with CABAC\n");
                                s_cabac_choice = 1;
                                break;
                            }
                            if (co) {
                                diag_log_write("CABAC", "User chose BACK — aborting stream\n");
                                s_cabac_choice = 2;
                                break;
                            }

                            cprev = cpd;
                            sceKernelDelayThread(50 * 1000);
                        }
                    }
                }

                /* Un-gate decoder + audio now that user has decided */
                g_cabac_dialog_active = 0;
                s_cabac_notice_start_us = 0;

                if (s_cabac_choice == 2) {
                    abort_stream_to_menu();
                    memset(&g_shared, 0, sizeof(g_shared));
                    s_cabac_choice = 0;  /* Reset for next stream */
                    skip_rescan = 1;
                    goto host_select_loop;
                }
            }
        }

        /* G-1: Poll input BEFORE display for minimum latency.
         * Input is sampled and sent at the earliest point in the frame. */
        if (video_started && !hud_is_visible()) input_poll_and_send();

        if (frame) {
            if (!video_started) { g_stream_ready_flag = 1; diag_log_write("MAIN", "First video frame displayed\n"); diag_log_flush(); s_fps_last_us = sceKernelGetSystemTimeLow(); }
            video_started = 1;

            /* ── Frame pacing ───────────────────────────────────────
             * If the frame was decoded very recently (< 4 ms ago), it
             * arrived late in the vblank cycle.  Displaying it now
             * risks tearing.  Wait one extra vblank so GU can swap
             * cleanly.  This trades ~16 ms latency for smooth cadence. */
            {
                static u32 s_pace_count = 0;
                u32 decode_ts = g_last_frame_decode_us;
                if (decode_ts > 0) {
                    u32 age_us = sceKernelGetSystemTimeLow() - decode_ts;
                    if (age_us < 4000) {
                        sceDisplayWaitVblankStart();
                        s_pace_count++;
                        if (s_pace_count <= 3 || (s_pace_count % 500) == 0) {
                            diag_log_write("PACE", "held frame: age=%uus total=%u\n",
                                           age_us, s_pace_count);
                        }
                    }
                }
            }

            display_frame(frame);
            s_disp_count++;
            s_idle_count = 0;  /* Reset idle counter — stream is active */
            s_fps_frame_count++;
            /* Per-frame decode-to-display latency (rolling average) */
            {
                u32 disp_us = sceKernelGetSystemTimeLow();
                if (g_last_frame_decode_us > 0) {
                    int frame_lat = (int)((disp_us - g_last_frame_decode_us) / 1000);
                    if (frame_lat >= 0 && frame_lat < 9999) {
                        /* Exponential moving average: alpha=0.1 (90% history, 10% new) */
                        static float s_lat_ema = 0.0f;
                        s_lat_ema = s_lat_ema * 0.9f + (float)frame_lat * 0.1f;
                        s_latency_avg_ms = (int)(s_lat_ema + 0.5f);
                    }
                }
            }
            if ((s_disp_count % 300) == 0) {
                diag_log_write("MAIN", "DISP n=%d idle=%d rdy=%u h=%u t=%u",
                               s_disp_count, s_idle_count,
                               (unsigned)g_shared.frame_ring.frame_ready,
                               (unsigned)g_shared.frame_ring.head,
                               (unsigned)g_shared.frame_ring.tail);
            }
            /* Watchdog credit restoration: stream is healthy (displaying frames).
             * Phase 5: Weight by decode success rate:
             *   >90%% FEC recovery → restore every ~10s (600 frames)
             *   <70%% FEC recovery → restore every ~20s (1200 frames)
             *   default → every ~15s (900 frames) */
            {
                static int s_credit_counter = 0;
                s_credit_counter++;
                {
                    ConnQualityState cq = control_stream_get_quality();
                    int restore_threshold = 900;
                    if (cq.fec_recovery_pct > 90)
                        restore_threshold = 600;
                    else if (cq.fec_recovery_pct < 70)
                        restore_threshold = 1200;
                    if (s_credit_counter >= restore_threshold) {
                        s_credit_counter = 0;
                        if (s_watchdog_restarts > 0) {
                            s_watchdog_restarts--;
                            diag_log_write("MAIN", "[WDG] credit restored (%d/5 used, fec=%u%%)",
                                           s_watchdog_restarts, (unsigned)cq.fec_recovery_pct);
                        }
                        s_mode_b_soft_count = 0;
                    }
                }
            }
        } else if (video_started) {
            s_idle_count++;
            if ((s_idle_count % 3000) == 0) {
                diag_log_write("MAIN", "IDLE disp=%d idle=%d rdy=%u h=%u t=%u",
                               s_disp_count, s_idle_count,
                               (unsigned)g_shared.frame_ring.frame_ready,
                               (unsigned)g_shared.frame_ring.head,
                               (unsigned)g_shared.frame_ring.tail);
            }
            /* Keep the last swapped frame visible during no-frame stalls.
             * Re-blitting stale decoder memory here can upload a recycled
             * black buffer after a pipeline reset. */

            /* Decode watchdog — two modes:
             * MODE A: OpenH264 infinite loop — g_decode_active_us stuck >3s
             *         FULL RESTART: kill thread, abandon decoder, reinit.
             * MODE B: No frames for >5s (idle >300) — likely WiFi packet
             *         loss preventing IDR reception.  SOFT RECOVERY: flush
             *         ring + request IDR without burning a restart slot.
             *         The ring backlog safety net in the decode thread loop
             *         also independently prevents ring overflow. */
            {
                extern volatile u32 g_decode_active_us;

                /* Mode A: OpenH264 hung mid-decode — only case
                 * that truly needs a thread restart.
                 * Phase 5: Dynamic timeout — 5s when bitrate < 300kbps
                 * (large IDR frames take longer at low bitrate).
                 * Try pipeline flush before full restart. */
                if (s_watchdog_restarts < 5) {
                    u32 active = g_decode_active_us;
                    if (active != 0) {
                        u32 now_us = sceKernelGetSystemTimeLow();
                        u32 elapsed = now_us - active;
                        u32 wdg_timeout_us = 3000000; /* 3s default */
                        {
                            int br_kbps = signal_strength_get_bitrate();
                            if (br_kbps > 0 && br_kbps < 300)
                                wdg_timeout_us = 5000000; /* 5s for low bitrate */
                        }
                        if (elapsed > wdg_timeout_us) {
                            /* Phase 5: Try pipeline flush before full restart */
                            diag_log_write("MAIN", "[WDG] Mode A flush attempt (hung %u ms, timeout %u ms)",
                                           (unsigned)(elapsed / 1000), (unsigned)(wdg_timeout_us / 1000));
                            oh264_pipeline_flush_buffers();
                            sceKernelDelayThread(100000); /* 100ms settle */
                            active = g_decode_active_us;
                            if (active != 0) {
                                elapsed = sceKernelGetSystemTimeLow() - active;
                                if (elapsed > wdg_timeout_us) {
                                    s_watchdog_restarts++;
                                    diag_log_write("MAIN", "WATCHDOG-A: decoder hung %u ms -- restart #%d",
                                                   (unsigned)(elapsed / 1000), s_watchdog_restarts);
                                    sw_decoder_thread_force_restart();
                                    control_stream_request_idr_force();
                                    s_idle_count = 0;
                                }
                            }
                        }
                    }
                }

                /* Removed intermediate decoder flush for v1.0 stability.
                 * Mid-stall flushes can invalidate references prematurely and
                 * amplify IDR churn on borderline links. */

                /* Mode B: No frames for ~5s — soft recovery (IDR burst).
                 * Does NOT consume a restart slot.  Repeats every 5s.
                 * After force_restart with no progress, back off to 10s
                 * to avoid flooding Sunshine when the problem is server-side. */
                {
                    static int s_force_restart_no_progress = 0;
                    int backoff_interval = (s_force_restart_no_progress > 0) ? 900 : 600;
                    if (s_idle_count > (unsigned)backoff_interval && (s_idle_count % (unsigned)backoff_interval) < 2) {
                    extern volatile int g_decoder_alive_counter;
                    extern volatile u32 g_last_frame_decode_us;
                    static int s_last_alive_count = 0;
                    int alive_now = g_decoder_alive_counter;
                    u32 now_us = sceKernelGetSystemTimeLow();
                    u32 since_decode_us = (g_last_frame_decode_us != 0)
                        ? (now_us - g_last_frame_decode_us) : 0xFFFFFFFFu;
                    int recent_decode = (since_decode_us < 4000000u); /* <4s */

                    /* Treat only displayed/decode-output progress as healthy.
                     * Callback progress with no new frame means OpenH264 is
                     * rejecting inputs while the display is frozen. */
                    if (recent_decode && alive_now > s_last_alive_count) {
                        if ((alive_now - s_last_alive_count) >= 2 || (s_idle_count % 900) < 2) {
                            diag_log_write("MAIN", "WATCHDOG-B: decoder progressing (counter %d->%d), deferring recovery",
                                           s_last_alive_count, alive_now);
                        }
                        s_mode_b_soft_count = 0;
                        s_force_restart_no_progress = 0;
                    } else if (recent_decode) {
                        diag_log_write("MAIN", "WATCHDOG-B: recent decode %u ms ago, deferring recovery",
                                       (unsigned)(since_decode_us / 1000));
                        s_mode_b_soft_count = 0;
                        s_force_restart_no_progress = 0;
                    } else if (alive_now > s_last_alive_count) {
                        s_mode_b_soft_count++;
                        diag_log_write("MAIN", "WATCHDOG-B: callbacks advanced %d->%d but no frame for %u ms -- soft recovery #%d",
                                       s_last_alive_count, alive_now,
                                       (unsigned)(since_decode_us / 1000),
                                       s_mode_b_soft_count);
                        {
                            extern volatile int g_idr_fully_decoded;
                            g_idr_fully_decoded = 0;
                        }
                        control_stream_request_idr();
                        if (s_mode_b_soft_count >= 3) {
                            diag_log_write("MAIN", "WATCHDOG-B: live decoder still has callback progress; keeping pipeline and forcing IDR/RFI resync");
                            control_stream_request_idr_force();
                            s_idle_count = 0;
                            s_mode_b_soft_count = 0;
                            s_force_restart_no_progress++;
                        }
                    } else {
                        s_mode_b_soft_count++;
                        diag_log_write("MAIN", "WATCHDOG-B: no frames for %u idles -- soft recovery #%d (IDR burst) alive=%d",
                                       (unsigned)s_idle_count, s_mode_b_soft_count, alive_now);
                        /* Reset g_idr_fully_decoded so (1) IDR requests are not
                         * suppressed, and (2) rtp_reassembly dedup allows fresh
                         * frames through even if frame_id wraps/jumps.
                         * moonlight-common-c has no such flag on its IDR path. */
                        {
                            extern volatile int g_idr_fully_decoded;
                            g_idr_fully_decoded = 0;
                        }
                        /* Single IDR request (rate limiter ensures delivery).
                         * Reduced from 3-burst to 1 to prevent Sunshine from
                         * being overwhelmed by rapid IDR requests on lossy WiFi. */
                        control_stream_request_idr();

                        /* After repeated soft recoveries with no alive progress,
                         * escalate to full restart. */
                        if (s_mode_b_soft_count >= 3) {
                            diag_log_write("MAIN", "WATCHDOG-B: no output after %d soft recoveries (alive=%d); keeping decoder resident and forcing IDR",
                                           s_mode_b_soft_count, alive_now);
                            control_stream_request_idr_force();
                            s_idle_count = 0;
                            s_mode_b_soft_count = 0;
                            s_force_restart_no_progress++;
                        }
                    }
                    s_last_alive_count = alive_now;
                    }
                }

                /* Mode C: Decoder confirmed dead — thread creation failed
                 * during a previous force_restart.  g_decoder_ready stays 0,
                 * ring fills permanently, no frames produced.  Retry once
                 * per 10s (~600 idles) as long as restart budget allows. */
                {
                    extern int g_decoder_ready;
                    if (!g_decoder_ready && s_idle_count > 600
                        && (s_idle_count % 600) < 2
                        && s_watchdog_restarts < 3) {
                        s_watchdog_restarts++;
                        diag_log_write("MAIN", "WATCHDOG-C: decoder dead (ready=0), retry restart #%d",
                                       s_watchdog_restarts);
                        sw_decoder_thread_force_restart();
                        control_stream_request_idr_force();
                        s_idle_count = 0;
                    }
                }

                /* Reset watchdog credits — handled in frame display path above */
            }

            /* B-4: Watchdog for ctrl_ping thread hang */
            {
                extern volatile u32 g_ctrl_ping_heartbeat_us;
                if (g_ctrl_ping_heartbeat_us != 0) {
                    u32 ctrl_elapsed = sceKernelGetSystemTimeLow() - g_ctrl_ping_heartbeat_us;
                    if (ctrl_elapsed > 5000000) { /* 5s without ping */
                        static u32 s_last_ctrl_stall_log_us = 0;
                        u32 now_us = sceKernelGetSystemTimeLow();
                        if (s_last_ctrl_stall_log_us == 0 ||
                            (now_us - s_last_ctrl_stall_log_us) >= 1000000) {
                            s_last_ctrl_stall_log_us = now_us;
                            diag_log_write("MAIN", "WATCHDOG: ctrl_ping stalled %u ms",
                                           ctrl_elapsed / 1000);
                        }
                    }
                }
            }
        } else if (!video_started) {
            ui_begin_frame(); ui_draw_gradient_bg(UI_COL_BG_TOP, UI_COL_BG_BOT); ui_draw_header("PSP Moonlight");
            ui_draw_text_centered(0.0f, 480.0f, 112.0f, UI_COL_TEXT, "Starting Stream");
            ui_draw_text_centered(0.0f, 480.0f, 146.0f, UI_COL_TEXT_DIM, "Waiting for Video...");
            float progress = ((float)(sceKernelGetSystemTimeLow()/1000 - stream_wait_start) * 100.0f) / 2000.0f;
            if (progress > 99.0f) progress = 99.0f;
            ui_draw_progress_bar(40, 172, 400, 8, progress, 100.0f, NULL);
            /* The bottom-of-loop compositor performs the single vblank-synced
             * swap for both pre-video UI and active video frames. Swapping here
             * too alternates the freshly drawn loading screen with a stale
             * buffer during slow stream startup. */
            ui_end_frame_no_swap();

            /* Mode D: Video never started — server may be in broken encoder
             * state (e.g. after stale session cancel+relaunch), ignoring IDR
             * requests.  Escalating recovery:
             * Phase 1 (10s): IDR burst to shake loose a key frame
             * Phase 2 (15s): Full pipeline restart + IDR
             * Phase 3 (20s): Tear down entire session, return to menu for
             *                fresh reconnect (creates new server encoder) */
            {
                u32 wait_elapsed_ms = sceKernelGetSystemTimeLow() / 1000 - stream_wait_start;
                static int s_mode_d_phase = 0;

                if (wait_elapsed_ms > 20000 && s_mode_d_phase < 3) {
                    s_mode_d_phase = 3;
                    /* Phase 5: Send graceful termination before abort */
                    diag_log_write("MAIN", "[WDG] Mode D graceful termination before abort");
                    {
                        extern void LiStopConnection(void);
                        LiStopConnection();
                        sceKernelDelayThread(100000); /* 100ms for server ack */
                    }
                    diag_log_write("MAIN", "WATCHDOG-D: no video for %u ms — full session reconnect",
                                   (unsigned)wait_elapsed_ms);
                    diag_log_flush();
                    abort_stream_to_menu();
                    memset(&g_shared, 0, sizeof(g_shared));
                    video_started = 0;
                    s_mode_d_phase = 0;
                    skip_rescan = 0;
                    goto host_select_loop;
                } else if (wait_elapsed_ms > 15000 && s_mode_d_phase < 2) {
                    s_mode_d_phase = 2;
                    diag_log_write("MAIN", "WATCHDOG-D: no video for %u ms — pipeline restart + IDR",
                                   (unsigned)wait_elapsed_ms);
                    sw_decoder_thread_force_restart();
                    control_stream_request_idr_force();
                    sceKernelDelayThread(10000);
                    control_stream_request_idr();
                } else if (wait_elapsed_ms > 10000 && s_mode_d_phase < 1) {
                    s_mode_d_phase = 1;
                    diag_log_write("MAIN", "WATCHDOG-D: no video for %u ms — IDR burst",
                                   (unsigned)wait_elapsed_ms);
                    control_stream_request_idr();
                    sceKernelDelayThread(10000);
                    control_stream_request_idr();
                    sceKernelDelayThread(10000);
                    control_stream_request_idr();
                }
            }
        }
        /* When video_started but no new frame: do nothing here.
         * The PSP display controller keeps showing the last-swapped
         * surface automatically.  Calling display_frame + swap when
         * there's nothing new causes double-buffer flashing because
         * each surface gets drawn at different times. */

        if (s_fps_last_us == 0) {
            s_fps_last_us = sceKernelGetSystemTimeLow();
        } else {
            u32 now_us = sceKernelGetSystemTimeLow();
            u32 elapsed = now_us - s_fps_last_us;
            if (elapsed >= 1000000) {
                int update_hud_stats = hud_is_visible();
                int displayed_frames = s_fps_frame_count;
                s_display_fps = (float)s_fps_frame_count * 1000000.0f / (float)elapsed;
                s_fps_frame_count = 0;
                s_fps_last_us = now_us;
#ifndef RETAIL_BUILD
                update_hud_stats = 1;
#endif
                if (update_hud_stats) {
                    HudStats hs;
                    int hud_video_payload_kbps = 0;
                    int hud_video_fec_kbps = 0;
                    int hud_audio_payload_kbps = 0;
                    int hud_audio_fec_kbps = 0;
                    memset(&hs, 0, sizeof(hs));
                    hs.latency_ms = s_latency_avg_ms;
                    hs.fps = s_display_fps;

                    {
                        static RtpVideoStats s_prev_vs;
                        static int s_prev_vs_valid = 0;
                        RtpVideoStats vs;
                        NetworkRtcpStats rtcp;
                        u32 d_recovered = 0;
                        u32 d_failed = 0;
                        u32 d_dropped = 0;
                        u32 loss_x10 = 0;
                        u32 fec_loss_x10 = 0;
                        u32 frame_loss_x10 = 0;
                        rtp_get_video_stats(&vs);
                        network_me_get_rtcp_stats(&rtcp);
                        if (s_prev_vs_valid) {
                            if (vs.packets_recovered < s_prev_vs.packets_recovered ||
                                vs.packets_failed < s_prev_vs.packets_failed ||
                                vs.frames_dropped < s_prev_vs.frames_dropped ||
                                vs.recovery_attempts < s_prev_vs.recovery_attempts) {
                                s_prev_vs_valid = 0;
                            } else {
                                d_recovered = vs.packets_recovered - s_prev_vs.packets_recovered;
                                d_failed = vs.packets_failed - s_prev_vs.packets_failed;
                                d_dropped = vs.frames_dropped - s_prev_vs.frames_dropped;
                            }
                        }
                        s_prev_vs = vs;
                        s_prev_vs_valid = 1;

                        if (d_recovered + d_failed > 0) {
                            fec_loss_x10 = (d_failed * 1000) / (d_recovered + d_failed);
                        }
                        if ((u32)displayed_frames + d_dropped > 0) {
                            frame_loss_x10 =
                                (d_dropped * 1000) / ((u32)displayed_frames + d_dropped);
                        }
                        loss_x10 = rtcp.fraction_lost_x10;
                        if (fec_loss_x10 > loss_x10) loss_x10 = fec_loss_x10;
                        if (frame_loss_x10 > loss_x10) loss_x10 = frame_loss_x10;
                        hs.packet_loss_pct = (float)loss_x10 / 10.0f;

                        if (d_recovered + d_failed > 0) {
                            hs.fec_recovery_pct =
                                (float)((d_recovered * 100) /
                                        (d_recovered + d_failed));
                        } else if (d_dropped > 0) {
                            hs.fec_recovery_pct = 0.0f;
                        } else {
                            hs.fec_recovery_pct = 100.0f;
                        }
                    }

                    hs.battery_pct = scePowerGetBatteryLifePercent();
                    if (hs.battery_pct < 0) hs.battery_pct = 0;
                    hs.host_proc_ms = (int)(g_host_processing_us / 1000);

                    {
                        extern volatile u32 g_decode_time_us;
                        hs.decode_ms = (int)(g_decode_time_us / 1000);
                    }

                    {
                        u32 cpu_pct = 0, gpu_pct = 0, me_pct = 0;
                        telemetry_sample(elapsed, &cpu_pct, &gpu_pct, &me_pct);
                        hs.cpu_pct = (int)cpu_pct;
                        hs.gpu_pct = (int)gpu_pct;
                        hs.me_pct = (int)me_pct;
                    }

                    {
                        BandwidthTelemetry bw;
                        memset(&bw, 0, sizeof(bw));
                        telemetry_sample_bandwidth(elapsed, &bw);
                        hs.bw_rx_kbps = (int)(bw.video_rx_kbps + bw.audio_rx_kbps);
                        hs.bw_usable_kbps = (int)bw.video_usable_kbps;
                        hs.bw_audio_kbps = (int)bw.audio_rx_kbps;
                        hs.bw_drop_kbps = (int)bw.video_drop_kbps;
                        hs.bw_usable_pct = (int)bw.usable_rx_pct;
                        hs.bw_video_packets_s = (int)bw.video_packets_s;
                        hud_video_payload_kbps =
                            (int)(bw.video_data_kbps + bw.video_fec_kbps);
                        hud_video_fec_kbps = (int)bw.video_fec_kbps;
                        hud_audio_payload_kbps = (int)bw.audio_rx_kbps;
                        hud_audio_fec_kbps = (int)bw.audio_fec_kbps;
#ifndef RETAIL_BUILD
                        diag_log_write("BW",
                                       "cfg=%dkbps pkt=%d res=%dx%d@%d rx=%ukbps accept=%ukbps usable=%ukbps data=%ukbps fec=%ukbps audio=%ukbps audio_data=%ukbps audio_fec=%ukbps drop=%ukbps pkts=%u/s audio_pkts=%u/s accept=%u%% usable=%u%% fec_overhead=%u%% audio_fec_overhead=%u%%\n",
                                       g_psp_config.bitrate,
                                       g_psp_config.packetSize,
                                       g_psp_config.width,
                                       g_psp_config.height,
                                       g_psp_config.fps,
                                       bw.video_rx_kbps,
                                       bw.video_accept_kbps,
                                       bw.video_usable_kbps,
                                       bw.video_data_kbps,
                                       bw.video_fec_kbps,
                                       bw.audio_rx_kbps,
                                       bw.audio_data_kbps,
                                       bw.audio_fec_kbps,
                                       bw.video_drop_kbps,
                                       bw.video_packets_s,
                                       bw.audio_packets_s,
                                       bw.accept_pct,
                                       bw.usable_rx_pct,
                                       bw.fec_overhead_pct,
                                       bw.audio_fec_overhead_pct);
#endif
                    }

                    {
                        SceSize free_mem = sceKernelTotalFreeMemSize();
                        SceSize largest = sceKernelMaxFreeMemSize();
                        SceSize baseline = s_stream_ram_start_free;

                        if (baseline == 0 || free_mem > baseline) {
                            baseline = free_mem;
                        }
                        hs.ram_free_kb = (int)(free_mem / 1024);
                        hs.ram_largest_kb = (int)(largest / 1024);
                        if (baseline > 0 && free_mem < baseline) {
                            unsigned long long used =
                                (unsigned long long)(baseline - free_mem) * 100ULL;
                            hs.ram_used_pct = (int)(used / baseline);
                            if (hs.ram_used_pct > 100) hs.ram_used_pct = 100;
                        }
                    }

                    hud_update_stats(&hs);
#ifndef RETAIL_BUILD
                    diag_log_write("HUD",
                                   "stats fps=%.1f lat=%d dec=%d loss=%.1f fec=%.1f cpu=%d gpu=%d me=%d ram=%d%% free=%dK largest=%dK bw_rx=%dkbps bw_usable=%dkbps bw_audio=%dkbps bw_drop=%dkbps bw_use=%d%% pkts=%d/s v=%d vfec=%d a=%d afec=%d\n",
                                   hs.fps, hs.latency_ms, hs.decode_ms,
                                   hs.packet_loss_pct, hs.fec_recovery_pct,
                                   hs.cpu_pct, hs.gpu_pct, hs.me_pct,
                                   hs.ram_used_pct, hs.ram_free_kb,
                                   hs.ram_largest_kb,
                                   hs.bw_rx_kbps, hs.bw_usable_kbps,
                                   hs.bw_audio_kbps, hs.bw_drop_kbps,
                                   hs.bw_usable_pct, hs.bw_video_packets_s,
                                   hud_video_payload_kbps,
                                   hud_video_fec_kbps,
                                   hud_audio_payload_kbps,
                                   hud_audio_fec_kbps);
#endif
                }
            }
        }

        sceCtrlPeekBufferPositive(&pad, 1);
        unsigned int remote_buttons_snapshot = g_remote_buttons;
        pad.Buttons |= remote_buttons_snapshot;  /* inject; cleared after input_poll_and_send */

        /* Remote exit request — single pokew triggers immediate stream exit */
        if (g_remote_exit_request) {
            diag_log_write("INP", "Remote exit request detected\n");
            g_remote_exit_request = 0;
            abort_stream_to_menu();
            memset(&g_shared, 0, sizeof(g_shared));
            skip_rescan = 1;  /* Quick relaunch: skip host re-probe */
            goto host_select_loop;
        }
        if (!g_decode_paused &&
            (remote_buttons_snapshot & PSP_CTRL_START) &&
            (remote_buttons_snapshot & PSP_CTRL_SELECT)) {
            diag_log_write("INP", "Remote Start+Select exit request detected\n");
            abort_stream_to_menu();
            memset(&g_shared, 0, sizeof(g_shared));
            skip_rescan = 1;  /* Quick relaunch: skip host re-probe */
            goto host_select_loop;
        }

        /* Start+Select combo = exit stream back to menu (skip when decode paused for testing).
         * Both buttons must be held simultaneously for >=500ms to prevent
         * accidental triggers regardless of press order (Select→Start or Start→Select). */
        {
            static u32 s_combo_start_us = 0;
            int combo_held = !g_decode_paused
                             && (pad.Buttons & PSP_CTRL_START)
                             && (pad.Buttons & PSP_CTRL_SELECT);
            if (combo_held) {
                u32 now_us = sceKernelGetSystemTimeLow();
                if (s_combo_start_us == 0) {
                    s_combo_start_us = now_us;
                } else if ((now_us - s_combo_start_us) >= 500000) {
                    diag_log_write("INP", "Start+Select exit combo detected\n");
                    s_combo_start_us = 0;
                    abort_stream_to_menu();
                    memset(&g_shared, 0, sizeof(g_shared));
                    skip_rescan = 1;  /* Quick relaunch: skip host re-probe */
                    goto host_select_loop;
                }
            } else {
                s_combo_start_us = 0;
            }
        }
        int hud_closed_this_loop = 0;
        {
            int hud_overlay_was_visible = hud_overlay_visible();
            int hud_ret = hud_handle_input(pad.Buttons);
            hud_closed_this_loop = hud_overlay_was_visible && !hud_overlay_visible();
            if (hud_ret == 2) {
                /* Pause: return to menu without quitting host session */
                diag_log_write("MAIN", "HUD Pause selected\n");
                g_stream_status = 2; /* paused */
                me_running = 0;
                continue;
            }
            if (hud_ret == 1) {
                /* HUD Quit: full teardown + return to host menu (NOT sceKernelExitGame) */
                diag_log_write("MAIN", "HUD Quit selected — returning to host menu\n");
                abort_stream_to_menu();
                memset(&g_shared, 0, sizeof(g_shared));
                skip_rescan = 1;  /* Quick relaunch: skip host re-probe */
                goto host_select_loop;
            }
        }
        /* input_poll_and_send moved to before display (G-1) */
        g_remote_buttons = 0;  /* clear after all consumers */
        signal_strength_update();

        /* Periodic log flush — ensures buffered decode timing data reaches disk */
        { static int flush_ctr = 0; if (++flush_ctr >= 300) { diag_log_flush(); flush_ctr = 0; } }

        if (frame || !video_started || decoder_is_cabac_detected()) {
            hud_render(); display_frame_finish();
        } else if (hud_overlay_visible()) {
            /* No new frame but HUD is open — re-blit last video frame so
             * the HUD can composite on top without double-buffer flashing. */
            display_frame_repeat(); hud_render(); display_frame_finish();
        } else if (hud_closed_this_loop) {
            display_frame_repeat(); display_frame_finish();
        } else {
            sceDisplayWaitVblankStart();
        }
        /* Always yield CPU when no frame is ready.  display_frame_finish()
         * waits for VBlank (60Hz), so the loop is naturally paced when
         * displaying.  The idle yield prevents CPU spinning at 100%
         * between decoded frames. */
        if (!frame) {
            sceKernelDelayThread(1000);
        }
    }

    wifi_keepalive_stop();
    network_me_shutdown(); control_stream_stop(); audio_thread_shutdown();
    rtsp_session_close();
    sw_decoder_thread_shutdown();
    sceKernelExitGame(); return 0;
}

static int exit_callback(int arg1, int arg2, void *common) {
    g_running = 0; me_running = 0; wifi_keepalive_stop(); network_me_abort(); control_stream_abort(); rtsp_session_close();
    sceKernelDelayThread(500000); sceKernelExitGame(); return 0;
}
static int callback_thread(SceSize args, void *argp) {
    int cbid = sceKernelCreateCallback("Exit Callback", exit_callback, NULL);
    if (cbid >= 0) sceKernelRegisterExitCallback(cbid);
    while (1) sceKernelSleepThreadCB();
    return 0;
}
static void setup_callbacks(void) {
    SceUID thid = sceKernelCreateThread("update_thread", callback_thread, 0x20, 0xFA0, 0, NULL);
    if (thid >= 0) sceKernelStartThread(thid, 0, NULL);
}
