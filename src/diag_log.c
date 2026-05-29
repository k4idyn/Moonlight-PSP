/*
 * diag_log.c — Diagnostic logger for PSP Moonlight
 *
 * Writes every log line to savedata; host0: is kept only as a legacy cleanup
 * target because live host0 writes can block under PSPLink.
 * Also mirrors every line to Kprintf (UsbKprintf → pspsh console).
 *
 * Thread safety: PSP kernel semaphore protects the shared buffer.
 */

#include <pspkernel.h>
#include <pspiofilemgr.h>
#include <psptypes.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#define DIAG_LOG_IMPLEMENTATION
#include "diag_log.h"
#include "storage_paths.h"

/* ---------- debug/retail toggle ---------- */
#ifdef RETAIL_BUILD
int g_debug_logging = 0;  /* retail: logs off by default */
#else
int g_debug_logging = 1;  /* debug: logs on by default */
#endif

void diag_log_set_debug(int enable)
{
#ifdef RETAIL_BUILD
    (void)enable;
    g_debug_logging = 0;
#else
    g_debug_logging = enable ? 1 : 0;
#endif
}

#ifndef RETAIL_BUILD

/* ---------- paths ---------- */
#define LOG_PATH_MS    MOONLIGHT_SAVE_LOG_PATH
#define LOG_PATH_HOST  "host0:/moonlight.log"

/* ---------- buffer ---------- */
#define LOG_BUF_SIZE  4096
#define LOG_LINE_MAX  512
#define FLUSH_THRESH  (LOG_BUF_SIZE - LOG_LINE_MAX - 64)

static char  s_buf[LOG_BUF_SIZE];
static int   s_buf_pos = 0;
#ifndef RETAIL_BUILD
static int   s_first_write = 1;  /* truncate log on first write each load */
#endif

/* ---------- time-guarded flush ---------- */
#define FLUSH_TIME_GUARD_US  2000000  /* flush if >2 seconds since last */
static u32   s_last_flush_us = 0;

/* ---------- thread safety ---------- */
static SceUID s_sem = -1;

static void ensure_init(void)
{
    if (s_sem < 0) {
        s_sem = sceKernelCreateSema("diag_log", 0, 1, 1, NULL);
    }
}

/* ---------- write buffer to a single path ---------- */
#ifndef RETAIL_BUILD
static void write_to_path(const char *path)
{
    SceUID fd;
    int flags = PSP_O_WRONLY | PSP_O_CREAT;
    if (s_first_write) {
        flags |= PSP_O_TRUNC;
        s_first_write = 0;
    } else {
        flags |= PSP_O_APPEND;
    }
    if (strcmp(path, LOG_PATH_MS) == 0) {
        moonlight_storage_ensure_data_dir();
    }
    fd = sceIoOpen(path, flags, 0777);
    if (fd >= 0) {
        sceIoWrite(fd, s_buf, (SceSize)s_buf_pos);
        sceIoClose(fd);
    }
}
#endif

/* ---------- internal flush (caller holds semaphore) ---------- */
static void flush_locked(void)
{
    if (s_buf_pos == 0) return;

    /* Write to ms0: only.  host0: writes go through psplink USB and
     * can block if pspsh is simultaneously issuing commands (scrshot,
     * cp, etc.), causing a deadlock: the diag_log semaphore is held
     * while host0: blocks, so every other thread that tries to log
     * also freezes.  ms0: writes are local and non-contended. */
#ifndef RETAIL_BUILD
    write_to_path(LOG_PATH_MS);
#endif

    s_buf_pos = 0;
    s_last_flush_us = sceKernelGetSystemTimeLow();
}

#endif /* !RETAIL_BUILD */

/* ---------- public API ---------- */

void diag_log_write(const char *tag, const char *fmt, ...)
{
#ifdef RETAIL_BUILD
    (void)tag;
    (void)fmt;
    return;
#else
    va_list ap;
    char   line[LOG_LINE_MAX];
    int    len;
    u32    raw_us, sec, ms;

    /* Retail mode: suppress all logs except FATAL tag */
    if (!g_debug_logging && tag[0] != 'F') return;  /* 'F' = FATAL */

    ensure_init();

    /* timestamp — cheap, no heap */
    raw_us = sceKernelGetSystemTimeLow();
    sec = (raw_us / 1000000) % 10000;
    ms  = (raw_us / 1000) % 1000;

    /* build "[SSSS.mmm] [TAG] message" */
    len = snprintf(line, sizeof(line), "[%04d.%03d] [%s] ",
                   (int)sec, (int)ms, tag);
    va_start(ap, fmt);
    len += vsnprintf(line + len, sizeof(line) - (size_t)len, fmt, ap);
    va_end(ap);
    if (len >= (int)sizeof(line)) len = (int)sizeof(line) - 1;

    /* Mirror to Kprintf (shows in pspsh console via UsbKprintf).
     * DISABLED: printf/Kprintf goes through USB and can block indefinitely
     * if pspsh console buffer is full, hanging the calling thread.
     * savedata log is sufficient for diagnostics. */
    /* printf("%s", line); */

    if (sceKernelWaitSema(s_sem, 1, NULL) < 0) return;

    /* auto-flush if this line would overflow */
    if (s_buf_pos + len >= LOG_BUF_SIZE) {
        flush_locked();
    }

    memcpy(s_buf + s_buf_pos, line, (size_t)len);
    s_buf_pos += len;

    /* Threshold flush: keep logs flowing without per-write Memory Stick I/O.
     * Per-write flushing caused catastrophic WiFi/ms0: DMA contention
     * on PSP — TCP SYN-ACK packets were missed, adding 5+ second
     * delays to RTSP connects and causing PLAY timeouts.
     *
     * Time-guard: flush if >2s since last flush even if below threshold,
     * so diagnostic data is never more than 2s stale. */
    {
        u32 now_us = sceKernelGetSystemTimeLow();
        if (s_buf_pos >= FLUSH_THRESH ||
            (s_buf_pos > 0 && (now_us - s_last_flush_us) > FLUSH_TIME_GUARD_US))
        {
            flush_locked();
            s_last_flush_us = now_us;
        }
    }

    sceKernelSignalSema(s_sem, 1);
#endif
}

void diag_log_flush(void)
{
#ifdef RETAIL_BUILD
    return;
#else
    ensure_init();
    if (sceKernelWaitSema(s_sem, 1, NULL) < 0) return;
    flush_locked();
    sceKernelSignalSema(s_sem, 1);
#endif
}

void diag_log_clear(void)
{
#ifdef RETAIL_BUILD
    return;
#else
    ensure_init();
    if (sceKernelWaitSema(s_sem, 1, NULL) < 0) return;

    s_buf_pos = 0;

#ifndef RETAIL_BUILD
    sceIoRemove(LOG_PATH_MS);
    sceIoRemove(LOG_PATH_HOST);
    /* legacy cleanup */
    sceIoRemove("host0:/moonlight_live.log");
    sceIoRemove(MOONLIGHT_SAVE_DIR "/diag.log");
    sceIoRemove(MOONLIGHT_SAVE_DIR "/net.log");
    sceIoRemove(MOONLIGHT_SAVE_DEBUG_LOG_PATH);
    sceIoRemove(MOONLIGHT_SAVE_DIR "/hello_test.txt");
    sceIoRemove(MOONLIGHT_SAVE_APPLIST_DUMP_PATH);
#endif

    sceKernelSignalSema(s_sem, 1);
#endif
}
