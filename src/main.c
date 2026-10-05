/*
 * main.c - Entry point for PSP Moonlight streaming client
 */

#include <pspkernel.h>
#include <pspmodulemgr.h>
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
#include "app_startup.h"
#include "storage_paths.h"
#include "psp_avc_build.h"
#if PSP_HARDWARE_AVC
#include "psp_avc_player.h"
#if defined(PSP_AVC_ENTRY_REPLAY) && PSP_AVC_ENTRY_REPLAY
extern int psp_avc_entry_replay(void);
#endif
#endif
#include "decoder_pipeline.h"
#include "stream_resolution.h"
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
#include "me.h"

PSP_MODULE_INFO("PSPMoonlight", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);
PSP_MAIN_THREAD_STACK_SIZE_KB(256);
PSP_HEAP_SIZE_KB(12 * 1024);

/* All logging unified into savedata moonlight.log */

PspConfig g_psp_config;
SharedState g_shared;
static volatile int g_running = 1;
volatile int me_running = 0;
volatile int g_is_paired = 0;
volatile int g_stream_status = 0;
static SceUID g_callback_thread_id = -1;
static SceUID g_exit_callback_id = -1;
static volatile int g_exit_callback_seen = 0;
static volatile int g_exit_callback_thread_stop = 0;

/* Remote input via pspsh pokew — write PSP_CTRL_ bitmask to this address */
volatile unsigned int g_remote_buttons = 0;
#ifndef RETAIL_BUILD
/* Diagnostics can hold stream input until the harness explicitly releases it.
 * UI readers retain their one-shot behavior. Normal/retail input is unchanged. */
volatile unsigned int g_remote_buttons_hold = 0;
#endif

static void consume_stream_remote_buttons(void)
{
#ifndef RETAIL_BUILD
    if (g_remote_buttons_hold)
        return;
#endif
    g_remote_buttons = 0;
}

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

volatile unsigned int g_remote_app_exit_request = 0;

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
extern void wifi_keepalive_abort(void);
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
extern int rtsp_session_close(void);
extern void display_init(void);
extern void display_frame(void *frame_data);
extern void display_frame_finish(void);
extern void display_frame_repeat(void);
extern volatile u32 g_display_gu_submit_sync_us;
extern void display_clear(unsigned int color);
extern void display_shutdown(void);
extern int  sw_decoder_thread_init(FrameRingBuffer *frame_rb);
extern void sw_decoder_thread_shutdown(void);
extern void sw_decoder_thread_force_restart(void);
extern int  decoder_is_cabac_detected(void);
extern volatile int g_avc_entropy_mode;
extern volatile int g_cabac_dialog_active;
#if !PSP_HARDWARE_AVC
extern void oh264_pipeline_flush_buffers(void);
#endif
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
static int s_force_restart_no_progress = 0;
static int s_video_seen_this_session = 0;
static SceSize s_stream_ram_start_free = 0;
static SceSize s_stream_ram_start_largest = 0;
extern void hud_shutdown(void);
extern void abort_stream_to_menu(void);

/* Decode-to-display latency timestamp (written by sw_decoder_thread) */
extern volatile u32 g_last_frame_decode_us;
extern volatile u32 g_decode_time_us;

/* Removed legacy logging init */

#include "diag_log.h"
#include "decode_flags.h"

static int g_gu_active = 0;
static void *s_entry_display_framebuf = NULL;
static int s_entry_display_bufferwidth = 0;
static int s_entry_display_pixelformat = 0;
static int s_entry_display_valid = 0;
#define PSP_DISPLAY_HEIGHT_PIXELS 272
#define PSP_DISPLAY_MAX_STRIDE 512
#define PSP_DISPLAY_MAX_COPY_BYTES (PSP_DISPLAY_MAX_STRIDE * PSP_DISPLAY_HEIGHT_PIXELS * 4)
#define PSP_VRAM_UNCACHED_BASE ((u32)0x44000000u)
#define PSP_VRAM_ADDR_MASK     ((u32)0x001FFFFFu)
/*
 * The GU uses two 512-pixel RGBA8888 buffers at offsets 0 and 0x88000 and
 * the 16-bit depth buffer at 0x110000.  Keep the entry PSPLink image in the
 * remaining VRAM tail as RGB565 instead of adding a 557 KB RAM BSS object.
 * The snapshot is only the visible 480-pixel area, so it fits before 2 MB
 * VRAM ends and is never touched by Moonlight's GU setup.
 */
#define PSP_ENTRY_SNAPSHOT_VRAM_OFFSET ((u32)0x00198000u)
#define PSP_ENTRY_SNAPSHOT_WIDTH       480
#define PSP_ENTRY_SNAPSHOT_BYTES       (PSP_ENTRY_SNAPSHOT_WIDTH * PSP_DISPLAY_HEIGHT_PIXELS * 2)
static int s_entry_display_snapshot_bytes = 0;
static int s_entry_display_snapshot_valid = 0;
static int cabac_present_pacing_enabled(void)
{
    return g_psp_config.cabacTestMode &&
           g_psp_config.width > 0 &&
           g_psp_config.width <= 480 &&
           g_psp_config.height > 0 &&
           g_psp_config.height <= 272 &&
           g_psp_config.fps > 0 &&
           g_psp_config.fps <= 60;
}

static int cabac_performance_video_only_mode(void)
{
    return g_psp_config.cabacTestMode &&
           !g_psp_config.audioEnabled &&
           g_psp_config.fps >= 30 &&
           g_psp_config.width > 0 &&
           g_psp_config.width <= 320 &&
           g_psp_config.height > 0 &&
           g_psp_config.height <= 180;
}

#define CABAC_PRESENT_FINE_WAIT_US 10000U
static u32 cabac_present_fine_wait_us(u32 interval_us)
{
    (void)interval_us;
    return CABAC_PRESENT_FINE_WAIT_US;
}

static u32 cabac_present_resync_threshold_us(u32 interval_us)
{
    return interval_us;
}

static void cabac_store_pending_present_frame(void *incoming_frame,
                                              int *pending_valid,
                                              void **pending_frame)
{
    int copy_bytes;

    if (!incoming_frame || !pending_valid || !pending_frame) {
        return;
    }

    copy_bytes = FRAME_STRIDE * g_psp_config.height * PIXEL_SIZE;
    if (copy_bytes <= 0 || copy_bytes > PSP_DISPLAY_MAX_COPY_BYTES) {
        *pending_frame = incoming_frame;
        *pending_valid = 1;
        return;
    }

    *pending_frame = incoming_frame;
    *pending_valid = 1;
}

static void *cabac_pace_present_frame(void *incoming_frame,
                                      int video_started)
{
    static int s_enabled_prev = 0;
    static int s_pending_valid = 0;
    static void *s_pending_frame = NULL;
    static u32 s_next_present_us = 0;
    static u32 s_hold_count = 0;
    static u32 s_replace_count = 0;
    static u32 s_present_count = 0;
    static u32 s_resync_count = 0;
    int enabled = cabac_present_pacing_enabled();
    u32 now_us;
    u32 interval_us;
    u32 fine_wait_us;
    u32 resync_threshold_us;
    void *present_frame = NULL;

    if (!enabled) {
        s_enabled_prev = 0;
        s_pending_valid = 0;
        s_pending_frame = NULL;
        s_next_present_us = 0;
        return incoming_frame;
    }

    now_us = sceKernelGetSystemTimeLow();
    interval_us = 1000000U / (u32)g_psp_config.fps;
    if (interval_us < 16666U) {
        interval_us = 16666U;
    }
    fine_wait_us = cabac_present_fine_wait_us(interval_us);
    resync_threshold_us = cabac_present_resync_threshold_us(interval_us);

    if (!s_enabled_prev || !video_started) {
        s_pending_valid = 0;
        s_pending_frame = NULL;
        s_next_present_us = 0;
        s_enabled_prev = 1;
    }

    if (incoming_frame) {
        if (s_pending_valid) {
            s_replace_count++;
            if (s_replace_count <= 4 || (s_replace_count % 120U) == 0) {
                diag_log_write("PACE",
                               "CABAC present replace pending frame count=%u",
                               (unsigned)s_replace_count);
            }
        }
        if (s_next_present_us == 0) {
            s_next_present_us = now_us;
        }
    }

    if (s_next_present_us == 0) {
        s_next_present_us = now_us;
    }

    now_us = sceKernelGetSystemTimeLow();
    if ((s32)(now_us - s_next_present_us) < 0) {
        u32 wait_us = s_next_present_us - now_us;
        s_hold_count++;
        if (s_hold_count <= 4 || (s_hold_count % 120U) == 0) {
            diag_log_write("PACE",
                           "CABAC present hold wait=%uus interval=%uus count=%u",
                           (unsigned)wait_us,
                           (unsigned)interval_us,
                           (unsigned)s_hold_count);
        }
        if (wait_us > fine_wait_us) {
            if (incoming_frame) {
                cabac_store_pending_present_frame(incoming_frame,
                                                  &s_pending_valid,
                                                  &s_pending_frame);
            }
            return NULL;
        }
        sceKernelDelayThread(wait_us);
        now_us = sceKernelGetSystemTimeLow();
        if ((s32)(now_us - s_next_present_us) < 0) {
            if (incoming_frame) {
                cabac_store_pending_present_frame(incoming_frame,
                                                  &s_pending_valid,
                                                  &s_pending_frame);
            }
            return NULL;
        }
    }

    if (incoming_frame) {
        s_pending_valid = 0;
        s_pending_frame = NULL;
        present_frame = incoming_frame;
    } else if (s_pending_valid && s_pending_frame) {
        s_pending_valid = 0;
        present_frame = s_pending_frame;
        s_pending_frame = NULL;
    } else {
        return NULL;
    }

    {
        u32 late_us = now_us - s_next_present_us;
        if (late_us > resync_threshold_us) {
            s_resync_count++;
            if (s_resync_count <= 8 || (s_resync_count % 120U) == 0) {
                diag_log_write("PACE",
                               "CABAC present resync late=%uus interval=%uus threshold=%uus count=%u",
                               (unsigned)late_us,
                               (unsigned)interval_us,
                               (unsigned)resync_threshold_us,
                               (unsigned)s_resync_count);
            }
        }
    }

    s_present_count++;
    if (s_present_count <= 4 || (s_present_count % 120U) == 0) {
        diag_log_write("PACE",
                       "CABAC present frame interval=%uus count=%u",
                       (unsigned)interval_us,
                       (unsigned)s_present_count);
    }

    {
        u32 deadline_us = s_next_present_us;
        u32 late_us = now_us - deadline_us;
        if (late_us > resync_threshold_us) {
            s_next_present_us = now_us + interval_us;
        } else {
            s_next_present_us = deadline_us + interval_us;
        }
    }
    return present_frame;
}
#define MAIN_PRESENT_THREAD_PRIORITY 0x1A
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

static int moonlight_main_exit_to_psplink(const char *reason);
static void moonlight_main_prepare_psplink_prompt_framebuffer(void);
void moonlight_main_prepare_for_process_exit(void);
void moonlight_main_mark_exitgame_pending(void);
int moonlight_main_notify_exit_callback(void);
static void moonlight_main_shutdown_exit_callback_thread(void);

static int moonlight_main_display_bpp(int pixelformat)
{
    if (pixelformat == PSP_DISPLAY_PIXEL_FORMAT_8888) {
        return 4;
    }
    if (pixelformat >= PSP_DISPLAY_PIXEL_FORMAT_565 &&
        pixelformat <= PSP_DISPLAY_PIXEL_FORMAT_4444) {
        return 2;
    }
    return 0;
}

static void *moonlight_main_display_cpu_addr(void *topaddr)
{
    u32 addr = (u32)topaddr;
    return (void *)(PSP_VRAM_UNCACHED_BASE + (addr & PSP_VRAM_ADDR_MASK));
}

static void *moonlight_main_entry_snapshot_addr(void)
{
    return (void *)(PSP_VRAM_UNCACHED_BASE + PSP_ENTRY_SNAPSHOT_VRAM_OFFSET);
}

static u16 moonlight_main_pack_rgb565(u32 pixel)
{
    unsigned int r = pixel & 0xFFu;
    unsigned int g = (pixel >> 8) & 0xFFu;
    unsigned int b = (pixel >> 16) & 0xFFu;
    return (u16)((r >> 3) | ((g >> 2) << 5) | ((b >> 3) << 11));
}

static u32 moonlight_main_unpack_rgb565(u16 pixel)
{
    unsigned int r = (pixel & 0x001Fu) << 3;
    unsigned int g = ((pixel >> 5) & 0x003Fu) << 2;
    unsigned int b = ((pixel >> 11) & 0x001Fu) << 3;
    return r | (g << 8) | (b << 16) | 0xFF000000u;
}

static void moonlight_main_capture_entry_display(void)
{
    void *topaddr = NULL;
    int bufferwidth = 0;
    int pixelformat = 0;
    int bpp;
    int ret;
    int y;

    ret = sceDisplayGetFrameBuf(&topaddr, &bufferwidth, &pixelformat,
                                PSP_DISPLAY_SETBUF_IMMEDIATE);
    bpp = moonlight_main_display_bpp(pixelformat);
    if (ret == 0 && bufferwidth > 0 &&
        bufferwidth <= PSP_DISPLAY_MAX_STRIDE && bpp > 0) {
        s_entry_display_framebuf = topaddr;
        s_entry_display_bufferwidth = bufferwidth;
        s_entry_display_pixelformat = pixelformat;
        s_entry_display_valid = 1;

        {
            u8 *src = (u8 *)moonlight_main_display_cpu_addr(topaddr);
            u16 *snapshot = (u16 *)moonlight_main_entry_snapshot_addr();
            for (y = 0; y < PSP_DISPLAY_HEIGHT_PIXELS; y++) {
                int x;
                if (pixelformat == PSP_DISPLAY_PIXEL_FORMAT_8888) {
                    u32 *src_row = (u32 *)(src + (size_t)y * (size_t)bufferwidth * 4u);
                    for (x = 0; x < PSP_ENTRY_SNAPSHOT_WIDTH; x++) {
                        snapshot[y * PSP_ENTRY_SNAPSHOT_WIDTH + x] =
                            moonlight_main_pack_rgb565(src_row[x]);
                    }
                } else {
                    u16 *src_row = (u16 *)(src + (size_t)y * (size_t)bufferwidth * 2u);
                    memcpy(snapshot + y * PSP_ENTRY_SNAPSHOT_WIDTH,
                           src_row,
                           PSP_ENTRY_SNAPSHOT_WIDTH * sizeof(u16));
                }
            }
            s_entry_display_snapshot_bytes = PSP_ENTRY_SNAPSHOT_BYTES;
            s_entry_display_snapshot_valid = 1;
        }
    }

    diag_log_write("MAIN", "entry display capture ret=0x%08X valid=%d fb=0x%08X bw=%d fmt=%d snapshot=%d storage=vram565\n",
                   (unsigned)ret,
                   s_entry_display_valid,
                   (unsigned)s_entry_display_framebuf,
                   s_entry_display_bufferwidth,
                   s_entry_display_pixelformat,
                   s_entry_display_snapshot_bytes);
    diag_log_flush();
}

void moonlight_main_mark_exitgame_pending(void)
{
    diag_log_write("MAIN", "process-exit exitgame mark begin\n");
    diag_log_flush();
    g_running = 0;
    g_remote_buttons = 0;
    g_remote_analog_active = 0;
    g_remote_exit_request = 0;
    g_remote_app_exit_request = 0;
    diag_log_write("MAIN", "process-exit exitgame mark complete\n");
    diag_log_flush();
}

int moonlight_main_notify_exit_callback(void)
{
    int ret;

    if (g_exit_callback_id < 0) {
        diag_log_write("MAIN", "process-exit callback notify unavailable (cbid=%d)\n",
                       (int)g_exit_callback_id);
        diag_log_flush();
        return 0;
    }

    ret = sceKernelNotifyCallback(g_exit_callback_id, 0x4D4C);
    diag_log_write("MAIN", "process-exit callback notify cbid=0x%08X ret=0x%08X\n",
                   (unsigned)g_exit_callback_id,
                   (unsigned)ret);
    diag_log_flush();
    return ret == 0;
}

static void moonlight_main_shutdown_exit_callback_thread(void)
{
    SceUID tid = g_callback_thread_id;
    int wake_ret = 0;
    int wait_ret = 0;

    g_exit_callback_thread_stop = 1;

    if (tid < 0 || tid == sceKernelGetThreadId()) {
        diag_log_write("MAIN", "process-exit callback thread shutdown skipped tid=0x%08X\n",
                       (unsigned)tid);
        diag_log_flush();
        return;
    }

    wake_ret = sceKernelWakeupThread(tid);
    {
        SceUInt timeout = 250000;
        wait_ret = sceKernelWaitThreadEnd(tid, &timeout);
    }

    if (wait_ret < 0) {
        int term_ret = sceKernelTerminateDeleteThread(tid);
        (void)term_ret;
        diag_log_write("MAIN", "process-exit callback thread forced stop tid=0x%08X wake=0x%08X wait=0x%08X term=0x%08X\n",
                       (unsigned)tid,
                       (unsigned)wake_ret,
                       (unsigned)wait_ret,
                       (unsigned)term_ret);
    } else {
        diag_log_write("MAIN", "process-exit callback thread stopped tid=0x%08X wake=0x%08X wait=0x%08X\n",
                       (unsigned)tid,
                       (unsigned)wake_ret,
                       (unsigned)wait_ret);
    }

    g_callback_thread_id = -1;
    g_exit_callback_id = -1;
    diag_log_flush();
}

static void moonlight_main_prepare_psplink_prompt_framebuffer(void)
{
    diag_log_write("MAIN", "process-exit framebuffer handoff begin\n");
    diag_log_flush();

    if (g_gu_active) {
        sceGuDisplay(GU_FALSE);
        sceGuTerm();
        g_gu_active = 0;
    }

    sceKernelDcacheWritebackInvalidateAll();
    sceDisplayWaitVblankStart();
    sceDisplaySetMode(0, 480, 272);
    pspDebugScreenInit();
    pspDebugScreenSetXY(0, 0);
    if (s_entry_display_valid && s_entry_display_snapshot_valid) {
        int ret;
        u8 *dst = (u8 *)moonlight_main_display_cpu_addr(s_entry_display_framebuf);
        u16 *snapshot = (u16 *)moonlight_main_entry_snapshot_addr();
        int y;

        for (y = 0; y < PSP_DISPLAY_HEIGHT_PIXELS; y++) {
            int x;
            if (s_entry_display_pixelformat == PSP_DISPLAY_PIXEL_FORMAT_8888) {
                u32 *dst_row = (u32 *)(dst + (size_t)y *
                                        (size_t)s_entry_display_bufferwidth * 4u);
                for (x = 0; x < PSP_ENTRY_SNAPSHOT_WIDTH; x++) {
                    dst_row[x] = moonlight_main_unpack_rgb565(
                        snapshot[y * PSP_ENTRY_SNAPSHOT_WIDTH + x]);
                }
            } else {
                u16 *dst_row = (u16 *)(dst + (size_t)y *
                                        (size_t)s_entry_display_bufferwidth * 2u);
                memcpy(dst_row,
                       snapshot + y * PSP_ENTRY_SNAPSHOT_WIDTH,
                       PSP_ENTRY_SNAPSHOT_WIDTH * sizeof(u16));
            }
        }
        sceKernelDcacheWritebackInvalidateAll();
        diag_log_write("MAIN", "process-exit restored entry framebuffer pixels bytes=%d storage=vram565\n",
                       s_entry_display_snapshot_bytes);
        ret = sceDisplaySetFrameBuf(s_entry_display_framebuf,
                                    s_entry_display_bufferwidth,
                                    s_entry_display_pixelformat,
                                    PSP_DISPLAY_SETBUF_IMMEDIATE);
        diag_log_write("MAIN", "process-exit restored entry framebuffer fb=0x%08X bw=%d fmt=%d ret=0x%08X\n",
                       (unsigned)s_entry_display_framebuf,
                       s_entry_display_bufferwidth,
                       s_entry_display_pixelformat,
                       (unsigned)ret);
    } else {
        diag_log_write("MAIN", "process-exit no entry framebuffer snapshot; leaving display buffer unchanged\n");
    }
    sceKernelDcacheWritebackInvalidateAll();
    sceDisplayWaitVblankStart();

    diag_log_write("MAIN", "process-exit framebuffer handoff complete\n");
    diag_log_flush();
}

#ifndef RETAIL_BUILD
/* PSPLink's helper unload bypasses newlib _exit(), which normally releases
 * the 12 MiB application heap. Only the successful own-exit handoff arms
 * release; ordinary CRT exit and external module-stop do not arm it. */
static int s_psplink_heap_release_pending;
extern void __psp_free_heap(void);
#endif

static int moonlight_main_exit_to_psplink(const char *reason)
{
    const char *why = reason ? reason : "unspecified";
    (void)why;

    diag_log_write("MAIN", "top-level process exit begin reason=%s\n", why);
    diag_log_flush();

    if (!moonlight_prepare_process_exit()) {
        diag_log_write("MAIN", "top-level process exit blocked reason=%s\n", why);
        diag_log_flush();
        return 0;
    }

#ifndef RETAIL_BUILD
    /*
     * Diagnostics builds are loaded as a PRX by PSPLink.  The MediaEngine
     * helper is a separate module, so ask it to stop/unload Moonlight after
     * this main thread exits.  Calling the module-manager self-stop APIs from
     * inside this PRX hangs in the PSPLink modload runtime, while the same
     * operation succeeds through the external modstun path.  Retail uses the
     * normal XMB exit path.
     */
    {
        SceUID module_id;
        int request_ret;

        moonlight_main_shutdown_exit_callback_thread();
        moonlight_main_prepare_psplink_prompt_framebuffer();
        module_id = sceKernelGetModuleId();
        diag_log_write("MAIN", "process exit: requesting helper stop/unload module=0x%08X reason=%s\n",
                       (unsigned)module_id, why);
        diag_log_flush();
        request_ret = RequestModuleUnload(module_id);
        if (request_ret >= 0) s_psplink_heap_release_pending = 1;
        diag_log_write("MAIN", "process exit: helper stop/unload request module=0x%08X ret=0x%08X reason=%s\n",
                       (unsigned)module_id,
                       (unsigned)request_ret,
                       why);
        diag_log_flush();
        if (request_ret < 0) {
            return 1;
        }
        sceKernelExitDeleteThread(0);
        return 1;
    }
#else
    {
        int helper_result = moonlight_startup_release_helper();
        if (helper_result < 0)
            diag_log_fatal("Release Media Engine helper", helper_result,
                           moonlight_startup_helper_path());
    }
    diag_log_write("MAIN", "top-level sceKernelExitGame handoff reason=%s\n", why);
    diag_log_flush();
    sceKernelDelayThread(50000);
    sceKernelExitGame();
    diag_log_write("MAIN", "top-level sceKernelExitGame returned unexpectedly reason=%s\n", why);
    diag_log_flush();
    return 1;
#endif
}

void moonlight_main_prepare_for_process_exit(void)
{
    diag_log_write("MAIN", "process-exit main prepare begin\n");
    diag_log_flush();
    g_running = 0;
    g_remote_buttons = 0;
    g_remote_analog_active = 0;
    g_remote_exit_request = 0;
    g_remote_app_exit_request = 0;
    diag_log_write("MAIN", "process-exit main prepare complete\n");
    diag_log_flush();
}

static void halt_with_error(const char *step_name, int error_code) {
    SceCtrlData pad;
    int released = 0;
    diag_log_fatal(step_name, error_code, moonlight_startup_helper_path());
    if (g_gu_active) sceGuTerm();
    g_gu_active = 0;
    pspDebugScreenInit();
    pspDebugScreenPrintf("Moonlight could not continue\n\nStep: %s\nCode: 0x%08X\n\n",
                         step_name, (unsigned int)error_code);
    if (strstr(step_name, "helper"))
        pspDebugScreenPrintf("Place moonlight_me_helper.prx beside EBOOT.PBP.\n\n");
    pspDebugScreenPrintf("Error file: PSP/SAVEDATA/Moonlight/error.log\n\nPress a button to exit.\n");
    LOG("\n=== FATAL ERROR ===\nStep : %s\nCode : 0x%08X (%d)\n", step_name, (unsigned int)error_code, error_code);
    diag_log_flush();
    sceCtrlSetSamplingCycle(0); sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);
    /* The XMB launch button may still be held when startup fails. Require
     * release and a new press so the error cannot vanish immediately. */
    while (1) {
        memset(&pad, 0, sizeof(pad));
        if (sceCtrlPeekBufferPositive(&pad, 1) >= 0) {
            if (!pad.Buttons) released = 1;
            else if (released) break;
        }
        sceKernelDelayThread(50 * 1000);
    }
    moonlight_main_exit_to_psplink("fatal error");
}

static void setup_callbacks(void);

#if PSP_HARDWARE_AVC
/* Use the requested mode for the early warmup, then verify it against the
 * first in-band SPS/PPS. A mismatch is handed to the UI thread and corrected
 * before any IDR is submitted to Sony. */
static int psp_avc_firmware_main_mode(void)
{
#if defined(PSP_AVC_UNIFIED_MAIN_MODE) && PSP_AVC_UNIFIED_MAIN_MODE
    return 1;
#else
    return g_psp_config.cabacTestMode ? 1 : 0;
#endif
}

static int psp_avc_prepare_before_rtsp(void)
{
    unsigned int width = (unsigned int)g_psp_config.width;
    unsigned int height = (unsigned int)g_psp_config.height;
    int caller_priority;
    int changed = 0;
    int r;

    if (!width || !height || width > 480 || height > 272 ||
        (width & 1u) || (height & 1u)) {
        diag_log_write("AVC", "pre-RTSP warmup invalid dimensions=%ux%u",
                       width, height);
        return -1;
    }

    /* Keep AVC setup on the sole UI thread before Wi-Fi and audio workers are
     * created. Session setup keeps module loading at the normal priority and
     * lowers only the firmware mode switch to the verified decoder priority. */
    caller_priority = sceKernelGetThreadCurrentPriority();
    /* Keep the entry thread at its normal priority for firmware module
     * import fixup.  The proven codec-stack bootstrap performs this work on
     * the default user-main thread; priority 33 can leave module-manager
     * fixup waiting on higher-priority startup work. */
    diag_log_write("AVC", "pre-RTSP warmup priority=%d changed=%d",
                   caller_priority, changed);
    diag_log_flush();
    psp_avc_player_log_state("pre-RTSP-before-open");
    r = psp_avc_player_open_profile(width, height, NULL,
                                    psp_avc_firmware_main_mode());
    if (changed) sceKernelChangeThreadPriority(0, caller_priority);

    if (r == -1 && psp_avc_player_is_open()) {
        /* Settings can change the negotiated dimensions between host-menu
         * visits.  Recycle only the old, idle session in that case. */
        diag_log_write("AVC", "pre-RTSP warmup replacing session dimensions=%ux%u",
                       width, height);
        r = psp_avc_player_close();
        if (r == 0) {
            caller_priority = sceKernelGetThreadCurrentPriority();
            changed = 0;
            if (caller_priority >= 0 && caller_priority < 33 &&
                sceKernelChangeThreadPriority(0, 33) >= 0) {
                changed = 1;
            }
            r = psp_avc_player_open_profile(width, height, NULL,
                                            psp_avc_firmware_main_mode());
            if (changed) sceKernelChangeThreadPriority(0, caller_priority);
        }
    }
    diag_log_write("AVC", "pre-RTSP warmup result=%d dimensions=%ux%u",
                   r, width, height);
    psp_avc_player_log_state("pre-RTSP-after-open");
    return r;
}

/* The only reliable ownership window for sceMeBootStart660(4) is the short
 * entry phase before settings/netconf creates utility and WLAN activity.  The
 * MPEG session itself is independent of the eventual stream dimensions; the
 * later prepare call only retargets the backend's SPS contract. */
static int psp_avc_prime_at_entry(void)
{
    int caller_priority;
    int r;

    caller_priority = sceKernelGetThreadCurrentPriority();
    /* Load/fix imports on the user-main thread, then boot the requested mode. */
    diag_log_write("AVC", "entry prime priority=%d firmware_main_mode=%d stream_cabac_request=%d; firmware boot uses 33",
                   caller_priority, psp_avc_firmware_main_mode(),
                   g_psp_config.cabacTestMode ? 1 : 0);
    diag_log_flush();
    r = psp_avc_player_open_profile(480, 272, NULL,
                                    psp_avc_firmware_main_mode());
    psp_avc_player_log_state("entry-prime");
    diag_log_write("AVC", "entry prime result=%d", r);
    diag_log_flush();
    return r;
}
#endif

int main(int argc, char *argv[]) {
    int ret; int skip_rescan = 0; char selected_host_ip[16] = {0}; HostPC *selected_host = NULL;
    setup_callbacks();
    diag_log_clear();
    moonlight_main_capture_entry_display();
#if PSP_HARDWARE_AVC
#ifdef RETAIL_BUILD
    /* PSPLink owns helper loading for diagnostics. XMB launches must load
     * their adjacent helper before calling any of its weak imports. */
    pspDebugScreenInit();
    pspDebugScreenPrintf("Starting Moonlight...\n");
    ret = moonlight_startup_load_helper(argc, argv);
    if (ret < 0) {
        halt_with_error(moonlight_startup_error_step(), ret);
        return ret;
    }
#endif
    ret = loadConfig(&g_psp_config);
    diag_log_write("AVC", "entry config read result=%d cabac=%d firmware_main=%d dimensions=%dx%d",
                   ret,g_psp_config.cabacTestMode,
                   psp_avc_firmware_main_mode(),g_psp_config.width,
                   g_psp_config.height);
    diag_log_flush();
    ret = psp_avc_prime_at_entry();
    if (ret < 0) {
        diag_log_write("MAIN", "entry AVC prime failed=%d\n", ret);
        diag_log_flush();
        halt_with_error("Initialize hardware AVC", ret);
        return ret;
    }
#endif
    if (!s_mbedtls_heap_ready) {
        mbedtls_memory_buffer_alloc_init(s_mbedtls_heap, sizeof(s_mbedtls_heap));
        s_mbedtls_heap_ready = 1;
    }
    pspDebugScreenInit();
    ret = scePowerSetClockFrequency(333, 333, 166);

    diag_log_write("MAIN", "[REMOTE] g_remote_buttons at 0x%08X\n", (unsigned int)&g_remote_buttons);
#ifndef RETAIL_BUILD
    diag_log_write("MAIN", "[REMOTE] g_remote_buttons_hold at 0x%08X\n", (unsigned int)&g_remote_buttons_hold);
#endif
    diag_log_write("MAIN", "[REMOTE] g_remote_analog_active at 0x%08X\n", (unsigned int)&g_remote_analog_active);
    diag_log_write("MAIN", "[REMOTE] g_remote_analog_lx at 0x%08X\n", (unsigned int)&g_remote_analog_lx);
    diag_log_write("MAIN", "[REMOTE] g_remote_analog_ly at 0x%08X\n", (unsigned int)&g_remote_analog_ly);
    diag_log_write("MAIN", "[REMOTE] g_decode_paused at 0x%08X\n", (unsigned int)&g_decode_paused);
    diag_log_write("MAIN", "[REMOTE] g_remote_exit_request at 0x%08X\n", (unsigned int)&g_remote_exit_request);
    diag_log_write("MAIN", "[REMOTE] g_remote_app_exit_request at 0x%08X\n", (unsigned int)&g_remote_app_exit_request);
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

#if defined(PSP_AVC_ENTRY_REPLAY) && PSP_AVC_ENTRY_REPLAY
    ret = psp_avc_entry_replay();
    diag_log_write("AVC","entry replay setup failed result=%08X\n",(unsigned)ret);
    diag_log_flush();
    /* Retain ownership on setup failure too; reset is the probe's exit. */
    if(ret<0) { for(;;) sceKernelDelayThread(100000); }
#endif

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

#if PSP_HARDWARE_AVC
    /* Retarget the already-owned entry session to the configured stream. */
    ret = psp_avc_prepare_before_rtsp();
    if (ret < 0) {
        diag_log_write("MAIN", "configured AVC session failed=%d\n", ret);
        diag_log_flush();
        halt_with_error("Configure hardware AVC", ret);
        return ret;
    }
#endif

    LOG("[STEP 2] Connecting Wi-Fi...\n");
    diag_log_write("UI", "TRANSITION wifi_start t=%u\n", sceKernelGetSystemTimeLow() / 1000);
    diag_log_flush();
    { /* Skip netconf dialog if WiFi is already connected (back-navigation) */
        int apctl_state = 0;
        ret = sceNetApctlGetState(&apctl_state);
#if PSP_HARDWARE_AVC && !defined(RETAIL_BUILD)
        {
            unsigned int stack_pointer;
            __asm__ volatile("move %0,$sp" : "=r"(stack_pointer));
            diag_log_write("AVC","pre-WiFi APCTL result=%08X state=%d stack_free=%d sp=%08X thread=%08X\n",
                (unsigned)ret,apctl_state,sceKernelCheckThreadStack(),
                stack_pointer,(unsigned)sceKernelGetThreadId());
            diag_log_flush();
        }
#endif
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
#if PSP_HARDWARE_AVC
    if (!psp_avc_player_is_ready()) {
        /* A previous stream closes the session before returning here.  Stop
         * the WiFi keepalive while recreating the firmware owner, then resume
         * it for discovery and the next RTSP connection. */
        diag_log_write("AVC", "reopening session before host discovery\n");
        diag_log_flush();
        wifi_keepalive_stop();
        ret = psp_avc_prepare_before_rtsp();
        wifi_keepalive_start();
        if (ret < 0) {
            diag_log_write("MAIN", "host-loop AVC warmup failed=%d\n", ret);
            diag_log_flush();
            halt_with_error("Reopen hardware AVC", ret);
            return ret;
        }
    }
#endif
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
        if (host_ret == -4) {
            diag_log_write("UI", "HOST requested process exit t=%u\n",
                           sceKernelGetSystemTimeLow() / 1000);
            moonlight_main_exit_to_psplink("host discovery");
            return 0;
        }
        if (host_ret >= 0) { selected_host = host_discovery_get_selected(); break; }
    }
    if (!selected_host) { halt_with_error("Host Selection", -1); return -1; }
    snprintf(selected_host_ip, sizeof(selected_host_ip), "%s", selected_host->ip);
    network_set_target_host(selected_host_ip);
    /* Trust the server's <PairStatus> from the HTTP probe rather than
     * relying solely on the single paired_host_ip in config.  This lets
     * the PSP work with multiple paired hosts without re-pairing. */
    /* Clear stale saved pairing too: a different PSP/client certificate may
     * select an unpaired host even when copied config says it was paired. */
    g_is_paired = selected_host->paired ? 1 : 0;
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
            if (ret != -3) break;  /* Only retry RTSP session launch errors (-3), not pairing (-1) or cancel (-2) */
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
        skip_rescan = 0;  /* Re-probe local network to rediscover host PC */
        goto host_select_loop;
    }

    s_stream_ram_start_free = sceKernelTotalFreeMemSize();
    s_stream_ram_start_largest = sceKernelMaxFreeMemSize();
    diag_log_write("MAIN", "RAM stream-start free=%uK largest=%uK\n",
                   (unsigned)(s_stream_ram_start_free / 1024),
                   (unsigned)(s_stream_ram_start_largest / 1024));

    extern int g_audio_rtsp_ok;
    if (g_audio_rtsp_ok && g_psp_config.audioEnabled) {
        diag_log_write("MAIN", "Audio receiver initialized before RTSP PLAY\n");
    } else if (g_audio_rtsp_ok) {
        diag_log_write("MAIN", "Audio disabled: keeping RTSP ping-only path; skipping RTP drain/decode/playback\n");
    } else {
        diag_log_write("MAIN", "Skipping audio init (no RTSP audio transport)\n");
    }

    diag_log_write("MAIN", "Initializing shared memory (~%uKB)...\n",
                   (unsigned)((sizeof(g_shared) + 1023) / 1024));
    memset(&g_shared, 0, sizeof(g_shared));

    diag_log_write("MAIN", "Initializing video decoder before RTP worker...\n");
    diag_log_flush();
    { g_cabac_detected = 0;
      g_cabac_dialog_active = 0;
    }
    ret = sw_decoder_thread_init(&g_shared.frame_ring);
    if (ret < 0) {
#if PSP_HARDWARE_AVC
        diag_log_write("AVC", "Sony AVC decoder init failed: %d\n", ret);
        halt_with_error("Sony AVC Init", ret); return ret;
#else
        diag_log_write("MAIN", "SW Decoder Init failed: %d\n", ret);
        halt_with_error("SW Decoder Init", ret); return ret;
#endif
    }
    me_running = 1;
    diag_log_write("MAIN", "Video decoder ready.\n");
    diag_log_flush();

    diag_log_write("MAIN", "Initializing network ME (D-UDP)...\n");
    diag_log_flush();
    network_me_init(&g_shared.packet_ring);
    diag_log_write("MAIN", "network_me_init done. Threads ready.\n");
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
    s_video_seen_this_session = 0;
    s_mode_b_soft_count = 0;
    s_force_restart_no_progress = 0;
    diag_log_flush();  /* Flush all handshake/setup logs to disk */
    safety_buffer_init();
    hud_init();
    telemetry_reset();
    {
        int signal_bitrate_kbps = signal_strength_get_launch_bitrate_kbps(g_psp_config.bitrate);
        int ri = g_psp_config.resolutionIndex;
        int stream_w = 0;
        int stream_h = 0;
        if (ri >= 0 && ri < RESOLUTION_COUNT) {
            stream_w = RESOLUTION_WIDTHS[ri];
            stream_h = RESOLUTION_HEIGHTS[ri];
        }
        if (g_psp_config.cabacTestMode &&
            g_psp_config.audioEnabled &&
            stream_w == 480 && stream_h == 272 &&
            g_psp_config.fps <= 10 &&
            signal_bitrate_kbps > 192) {
            diag_log_write("SIG", "CABAC quality signal cap: %d -> 192 kbps\n",
                           signal_bitrate_kbps);
            signal_bitrate_kbps = 192;
        }
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
        /* A lost host video sender can leave RTP silent while APCTL and the
         * control socket keep answering. network_me requests an urgent IDR
         * after 2.5 seconds without media; if that does not restore RTP,
         * tear down cleanly instead of holding a frozen image indefinitely. */
        if (me_running && g_stream_ready_flag) {
            u32 video_idle_ms = network_me_video_idle_ms();
            if (video_idle_ms >= 10000u) {
                diag_log_write("MAIN", "video RTP idle %ums after first display and IDR recovery; ending stalled stream\n",
                               (unsigned)video_idle_ms);
                diag_log_flush();
                g_stream_status = 1;
                me_running = 0;
            }
        }
        if (!me_running && g_stream_status == 1) {
            diag_log_write("MAIN", "host stream ended; completing teardown and returning to discovery\n");
            diag_log_flush();
            abort_stream_to_menu();
            memset(&g_shared, 0, sizeof(g_shared));
            skip_rescan = 1;
            goto host_select_loop;
        }
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
        u32 frame_decode_ts = 0;
        unsigned int remote_buttons_snapshot = g_remote_buttons;

        if (g_remote_app_exit_request) {
            diag_log_write("INP", "Remote app exit request detected before frame work\n");
            diag_log_flush();
            g_remote_app_exit_request = 0;
            g_remote_buttons = 0;
            moonlight_main_exit_to_psplink("remote app exit");
            return 0;
        }
        if (g_remote_exit_request) {
            diag_log_write("INP", "Remote exit request detected before frame work\n");
            diag_log_flush();
            g_remote_exit_request = 0;
            g_remote_buttons = 0;
            abort_stream_to_menu();
            memset(&g_shared, 0, sizeof(g_shared));
            skip_rescan = 1;
            goto host_select_loop;
        }
        if (!g_decode_paused &&
            (remote_buttons_snapshot & PSP_CTRL_START) &&
            (remote_buttons_snapshot & PSP_CTRL_SELECT)) {
            diag_log_write("INP", "Remote Start+Select exit request detected before frame work\n");
            diag_log_flush();
            g_remote_buttons = 0;
            abort_stream_to_menu();
            memset(&g_shared, 0, sizeof(g_shared));
            skip_rescan = 1;
            goto host_select_loop;
        }

#if PSP_HARDWARE_AVC
        if (psp_avc_player_profile_change_ready()) {
#ifndef RETAIL_BUILD
            int old_profile = psp_avc_player_current_profile();
            int new_profile = psp_avc_player_requested_profile();
#endif
            ret = psp_avc_player_apply_profile_change();
            if (ret < 0) {
#ifndef RETAIL_BUILD
                diag_log_write("AVC", "UI-thread profile switch failed=%08X old_main=%d requested_main=%d; exiting stream cleanly",
                               (unsigned)ret,old_profile,new_profile);
                diag_log_flush();
#endif
                g_remote_exit_request = 1;
            } else {
#ifndef RETAIL_BUILD
                diag_log_write("AVC", "UI-thread profile switch complete old_main=%d new_main=%d; requesting fresh IDR before decode",
                               old_profile,new_profile);
                diag_log_flush();
#endif
                control_stream_request_idr_force();
            }
        }
#endif

        /* Race-safe ring buffer drain: rely solely on head != tail.
         * The old 'frame_ready' flag had a TOCTOU race: consumer could
         * clear it to 0 right after producer set it to 1, losing a frame
         * and—if the producer was between IDRs—freezing the display. */
        {
#if PSP_HARDWARE_AVC
            unsigned int output_sequence;
            frame = psp_avc_player_take(&output_sequence, &frame_decode_ts);
#else
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
#endif
        }
        if (frame && frame_decode_ts == 0) {
            frame_decode_ts = g_last_frame_decode_us;
        }
        if (frame) s_video_seen_this_session = 1;

        static int video_started = 0;
        static int s_disp_count = 0;
        static int s_idle_count = 0;
        static u32 s_fps_last_us = 0;
        static int s_fps_frame_count = 0;
        static float s_display_fps = 0.0f;
        static int s_latency_avg_ms = 0;
        int cabac_detected = decoder_is_cabac_detected();
#if PSP_HARDWARE_AVC
        int stream_entropy_mode = g_avc_entropy_mode;
#endif

        /* CABAC detection is telemetry-only here. Release readiness must come
         * from making this path playable, not from rejecting it after startup. */
        {
            static int s_cabac_detect_logged = 0;
            static int s_entropy_mode_logged = -2;

#if PSP_HARDWARE_AVC
            if (stream_entropy_mode >= 0 &&
                stream_entropy_mode != s_entropy_mode_logged) {
                diag_log_write("AVC", "in-band PPS selected %s tuning row; Sony firmware mode=%s; preset=%d %dx%d@%d",
                               stream_entropy_mode ? "CABAC" : "CAVLC",
                               psp_avc_player_current_profile() ? "Main" : "Baseline",
                               g_psp_config.presetIndex,g_psp_config.width,
                               g_psp_config.height,g_psp_config.fps);
                s_entropy_mode_logged = stream_entropy_mode;
            }
#else
            (void)s_entropy_mode_logged;
#endif

            if (!cabac_detected) {
                s_cabac_detect_logged = 0;
            } else if (!s_cabac_detect_logged) {
                diag_log_write("CABAC", "CABAC detected while cabacTestMode=%d -- continuing\n",
                               g_psp_config.cabacTestMode ? 1 : 0);
                s_cabac_detect_logged = 1;
            }

            g_cabac_dialog_active = 0;
        }

        /* G-1: Poll input BEFORE display for minimum latency.
         * Input is sampled and sent at the earliest point in the frame. */
        if (video_started && !hud_is_visible()) input_poll_and_send();

        if (!PSP_HARDWARE_AVC)
            frame = cabac_pace_present_frame(frame, video_started);

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
                int cabac_clocked_present = cabac_present_pacing_enabled();
                if (decode_ts > 0) {
                    u32 age_us = sceKernelGetSystemTimeLow() - decode_ts;
                    if (!PSP_HARDWARE_AVC && !cabac_clocked_present &&
                        age_us < 4000) {
                        sceDisplayWaitVblankStart();
                        s_pace_count++;
                        if (s_pace_count <= 3 || (s_pace_count % 500) == 0) {
                            diag_log_write("PACE", "held frame: age=%uus total=%u\n",
                                           age_us, s_pace_count);
                        }
                    }
                }
            }

#if PSP_HARDWARE_AVC
            display_avc_frame(frame);
#else
            display_frame(frame);
#endif
            s_disp_count++;
            s_idle_count = 0;  /* Reset idle counter for new decoded content. */
            s_fps_frame_count++;
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
                ConnQualityState cq = control_stream_get_quality();
                int restore_threshold = 900;
                s_credit_counter++;
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
#if PSP_HARDWARE_AVC
                            /* Sony's decoder has no in-place flush operation;
                             * the worker restart below owns its recovery path. */
#else
                            oh264_pipeline_flush_buffers();
#endif
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
                    /* A live stream that has already displayed video but has
                     * exhausted a full set of IDR recoveries is no longer a
                     * useful session. This commonly follows a host display or
                     * encoder re-enumeration that leaves audio alive but stops
                     * video RTP. Return through the normal teardown path rather
                     * than holding the last frame indefinitely. */
                    if (s_video_seen_this_session &&
                        s_force_restart_no_progress >= 1) {
                        diag_log_write("MAIN", "WATCHDOG-B: video absent after repeated IDR recovery; requesting clean return to host menu");
                        diag_log_flush();
                        g_remote_exit_request = 1;
                    }
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
                u32 ctrl_heartbeat_us = g_ctrl_ping_heartbeat_us;
                if (ctrl_heartbeat_us != 0) {
                    u32 now_us = sceKernelGetSystemTimeLow();
                    u32 ctrl_elapsed = now_us - ctrl_heartbeat_us;
                    if (ctrl_elapsed > 5000000) { /* 5s without ping */
                        static u32 s_last_ctrl_stall_log_us = 0;
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
                update_hud_stats = video_started;
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
                        u32 d_attempts = 0;
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
                                d_attempts = vs.recovery_attempts - s_prev_vs.recovery_attempts;
                            }
                        }
                        s_prev_vs = vs;
                        s_prev_vs_valid = 1;
                        hs.fec_attempts = d_attempts;
                        hs.fec_recovered_packets = d_recovered;
                        hs.fec_failed_packets = d_failed;
                        hs.fec_metric_valid = (d_recovered + d_failed) > 0;

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

                        if (hs.fec_metric_valid) {
                            hs.fec_recovery_pct =
                                (float)((d_recovered * 100) /
                                        (d_recovered + d_failed));
                        }
                    }

                    hs.battery_pct = scePowerGetBatteryLifePercent();
                    hs.host_proc_us = (int)g_host_processing_us;
                    hs.host_proc_valid = g_host_processing_valid &&
                        (u32)(sceKernelGetSystemTimeLow()-g_host_processing_sample_us) < 2000000u;
                    hs.latency_valid = s_latency_avg_ms > 0;
                    hs.decode_us = (int)g_decode_time_us;
                    hs.decode_valid = g_decode_time_us > 0;
                    hs.gu_submit_sync_us = g_display_gu_submit_sync_us;
                    hs.gu_frame_valid = g_display_gu_submit_sync_us > 0;
#if PSP_HARDWARE_AVC
                    {
                        u32 gu_share_pct = 0;
                        telemetry_sample(elapsed, NULL, &gu_share_pct, NULL);
                        hs.gu_share_pct = (int)gu_share_pct;
                    }
#else
                    {
                        u32 cpu_pct = 0, gu_share_pct = 0, me_pct = 0;
                        telemetry_sample(elapsed,&cpu_pct,&gu_share_pct,&me_pct);
                        hs.cpu_pct = (int)cpu_pct;
                        hs.gu_share_pct = (int)gu_share_pct;
                        hs.me_pct = (int)me_pct;
                    }
#endif

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
                        hs.fec_active = bw.video_fec_kbps > 0;
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
                        hs.ram_free_delta_kb = (int)(baseline / 1024) -
                                               hs.ram_free_kb;
                    }

                    hs.audio_enabled = g_psp_config.audioEnabled != 0;
                    hs.audio_active = audio_thread_is_running();
                    if (hs.audio_enabled && hs.audio_active) {
                        AudioStats audio_stats;
                        RtpAudioStats rtp_audio_stats;
                        memset(&audio_stats,0,sizeof(audio_stats));
                        memset(&rtp_audio_stats,0,sizeof(rtp_audio_stats));
                        audio_thread_get_stats(&audio_stats);
                        rtp_get_audio_stats(&rtp_audio_stats);
                        hs.audio_frames_played = rtp_audio_stats.frames_played;
                        hs.audio_empty_holds = audio_stats.empty_holds;
                        hs.audio_underruns = rtp_audio_stats.underruns;
                        hs.audio_ring_drops = audio_stats.frames_dropped;
                        hs.audio_plc = rtp_audio_stats.plc_count;
                    }

                    hud_update_stats(&hs);
#ifndef RETAIL_BUILD
#if PSP_HARDWARE_AVC
                    diag_log_write("HUD",
                                   "stats fps=%.1f d2p_ms=%d d2p_valid=%d avc_us=%d avc_valid=%d gu_sync_us=%u gu_share_pct=%d loss=%.1f fec_active=%d fec_valid=%d fec_attempts=%u fec_recovered=%u fec_failed=%u host_us=%d host_valid=%d ram_free=%dK ram_largest=%dK ram_delta=%dK audio_enabled=%d audio_active=%d audio_played=%u audio_holds=%u audio_underruns=%u audio_plc=%u audio_drops=%u bw_rx=%dkbps bw_usable=%dkbps bw_audio=%dkbps bw_drop=%dkbps bw_use=%d%% pkts=%d/s v=%d vfec=%d a=%d afec=%d\n",
                                   hs.fps,hs.latency_ms,hs.latency_valid,
                                   hs.decode_us,hs.decode_valid,
                                   (unsigned)hs.gu_submit_sync_us,hs.gu_share_pct,
                                   hs.packet_loss_pct,hs.fec_active,
                                   hs.fec_metric_valid,(unsigned)hs.fec_attempts,
                                   (unsigned)hs.fec_recovered_packets,
                                   (unsigned)hs.fec_failed_packets,
                                   hs.host_proc_us,hs.host_proc_valid,
                                   hs.ram_free_kb,hs.ram_largest_kb,
                                   hs.ram_free_delta_kb,hs.audio_enabled,
                                   hs.audio_active,
                                   (unsigned)hs.audio_frames_played,
                                   (unsigned)hs.audio_empty_holds,
                                   (unsigned)hs.audio_underruns,
                                   (unsigned)hs.audio_plc,
                                   (unsigned)hs.audio_ring_drops,
                                   hs.bw_rx_kbps, hs.bw_usable_kbps,
                                   hs.bw_audio_kbps, hs.bw_drop_kbps,
                                   hs.bw_usable_pct, hs.bw_video_packets_s,
                                   hud_video_payload_kbps,
                                   hud_video_fec_kbps,
                                   hud_audio_payload_kbps,
                                   hud_audio_fec_kbps);
#else
                    diag_log_write("HUD",
                                   "stats fps=%.1f d2p_ms=%d avc_us=%d loss=%.1f fec_valid=%d fec_pct=%.1f cpu_decode_share=%d gu_share=%d me_share=%d host_us=%d host_valid=%d ram_free=%dK ram_largest=%dK ram_delta=%dK audio_enabled=%d audio_played=%u audio_holds=%u audio_underruns=%u audio_plc=%u audio_drops=%u bw_rx=%dkbps bw_usable=%dkbps bw_audio=%dkbps bw_drop=%dkbps bw_use=%d%% pkts=%d/s\n",
                                   hs.fps,hs.latency_ms,hs.decode_us,
                                   hs.packet_loss_pct,hs.fec_metric_valid,
                                   hs.fec_recovery_pct,hs.cpu_pct,
                                   hs.gu_share_pct,hs.me_pct,
                                   hs.host_proc_us,hs.host_proc_valid,
                                   hs.ram_free_kb,hs.ram_largest_kb,
                                   hs.ram_free_delta_kb,hs.audio_enabled,
                                   (unsigned)hs.audio_frames_played,
                                   (unsigned)hs.audio_empty_holds,
                                   (unsigned)hs.audio_underruns,
                                   (unsigned)hs.audio_plc,
                                   (unsigned)hs.audio_ring_drops,
                                   hs.bw_rx_kbps,hs.bw_usable_kbps,
                                   hs.bw_audio_kbps,hs.bw_drop_kbps,
                                   hs.bw_usable_pct,hs.bw_video_packets_s);
#endif
#endif
                }
            }
        }

        sceCtrlPeekBufferPositive(&pad, 1);
        remote_buttons_snapshot |= g_remote_buttons;
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
        consume_stream_remote_buttons(); /* after every stream input consumer */
        signal_strength_update();

        /* Keep maintenance tied to elapsed time as idle polling speeds up. */
        {
            static u32 s_last_log_flush_us = 0;
            u32 log_now_us = sceKernelGetSystemTimeLow();
            if (!s_last_log_flush_us) {
                s_last_log_flush_us = log_now_us;
            } else if ((u32)(log_now_us - s_last_log_flush_us) >= 5000000U) {
                diag_log_flush();
                s_last_log_flush_us = log_now_us;
            }
        }

        if (frame || !video_started) {
            hud_render(); display_frame_finish();
            /* Include the VBlank-synchronized swap in decode-to-present age. */
            if (frame && frame_decode_ts != 0) {
                u32 present_us = sceKernelGetSystemTimeLow();
                int frame_lat =
                    (int)((present_us - frame_decode_ts) / 1000);
                if (frame_lat >= 0 && frame_lat < 9999) {
                    static float s_lat_ema = 0.0f;
                    s_lat_ema = s_lat_ema * 0.9f +
                                (float)frame_lat * 0.1f;
                    s_latency_avg_ms = (int)(s_lat_ema + 0.5f);
#ifndef RETAIL_BUILD
                    if ((s_disp_count % 15) == 0) {
                        diag_log_write("LATENCY",
                                       "decode-to-present=%dms frame_ts=%u present_ts=%u",
                                       frame_lat,(unsigned)frame_decode_ts,
                                       (unsigned)present_us);
                    }
#endif
                }
            }
        } else if (hud_overlay_visible()) {
            /* No new frame but HUD is open — re-blit last video frame so
             * the HUD can composite on top without double-buffer flashing. */
            if (cabac_performance_video_only_mode()) {
                static int s_cabac_hud_idle_hold_logged = 0;
                if (!s_cabac_hud_idle_hold_logged) {
                    diag_log_write("GPU", "CABAC performance HUD idle hold path enabled");
                    s_cabac_hud_idle_hold_logged = 1;
                }
                sceDisplayWaitVblankStart();
            } else {
                display_frame_repeat(); hud_render(); display_frame_finish();
            }
        } else if (hud_closed_this_loop) {
            if (cabac_performance_video_only_mode()) {
                sceDisplayWaitVblankStart();
            } else {
                display_frame_repeat(); display_frame_finish();
            }
        } else {
#if PSP_HARDWARE_AVC
            /* A decoded picture can arrive while the presenter is idle.
             * Waiting a whole VBlank here, then another at the swap, adds
             * a refresh of age and turns arrival jitter into uneven swaps.
             * The bounded yield below lets audio/AVC run between polls. */
#ifndef RETAIL_BUILD
            static int s_hw_idle_poll_logged = 0;
            if (!s_hw_idle_poll_logged) {
                diag_log_write("GPU", "hardware idle polls every 1000us; VBlank retained for swaps and HUD");
                s_hw_idle_poll_logged = 1;
            }
#endif
#else
            sceDisplayWaitVblankStart();
#endif
        }
        /* Completed swaps remain synchronized to VBlank. Yield when no frame
         * is ready, including hardware polling, so decoder/audio can run. */
        if (!frame) {
            sceKernelDelayThread(1000);
        }
    }

    moonlight_main_exit_to_psplink("main loop fallthrough");
    return 0;
}

static int exit_callback(int arg1, int arg2, void *common) {
    (void)arg1;
    (void)arg2;
    (void)common;
    g_running = 0;
    g_remote_buttons = 0;
    g_remote_analog_active = 0;
    g_remote_exit_request = 0;
    g_remote_app_exit_request = 0;
    g_exit_callback_seen = 1;

    sceKernelExitGame();
    return 0;
}

int module_stop(SceSize args, void *argp)
{
    (void)args;
    (void)argp;
    g_running = 0;
    me_running = 0;
    g_stream_status = 0;
    g_remote_buttons = 0;
    g_remote_analog_active = 0;
    g_remote_exit_request = 0;
    g_remote_app_exit_request = 0;
    {
        int prepared;
        diag_log_write("MAIN", "module_stop requested; running bounded app cleanup\n");
        diag_log_flush();
        prepared = moonlight_prepare_process_exit();
        me_running = 0;
        g_stream_status = 0;
        moonlight_main_shutdown_exit_callback_thread();
        moonlight_main_prepare_psplink_prompt_framebuffer();
#ifdef RETAIL_BUILD
        if (prepared) (void)moonlight_startup_release_helper();
#endif
        diag_log_write("MAIN", "module_stop cleanup result=%d\n", prepared);
        diag_log_flush();
#ifndef RETAIL_BUILD
        if (prepared && s_psplink_heap_release_pending) {
            /* All workers and the UI main thread have exited. This is the
             * last operation that can use newlib; do not log or allocate
             * after releasing its partition block. Clear our flag first
             * because the SDK hook does not clear its saved block UID. */
            s_psplink_heap_release_pending = 0;
            __psp_free_heap();
        }
#endif
        return prepared ? 0 : 1;
    }
}
static int callback_thread(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    g_exit_callback_id = sceKernelCreateCallback("Exit Callback", exit_callback, NULL);
    if (g_exit_callback_id >= 0) sceKernelRegisterExitCallback(g_exit_callback_id);
    while (!g_exit_callback_thread_stop) sceKernelSleepThreadCB();
    if (g_exit_callback_id >= 0) {
        sceKernelDeleteCallback(g_exit_callback_id);
        g_exit_callback_id = -1;
    }
    g_callback_thread_id = -1;
    sceKernelExitDeleteThread(0);
    return 0;
}
static void setup_callbacks(void) {
    g_exit_callback_thread_stop = 0;
    g_callback_thread_id = sceKernelCreateThread("update_thread", callback_thread, 0x20, 0xFA0, 0, NULL);
    if (g_callback_thread_id >= 0) sceKernelStartThread(g_callback_thread_id, 0, NULL);
}
