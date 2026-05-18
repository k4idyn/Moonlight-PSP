/* diag_log.h - Diagnostic logging to ms0: and host0: with ring-buffer and kernel semaphore */
#ifndef DIAG_LOG_H
#define DIAG_LOG_H

#include <psptypes.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Debug/retail toggle.
 * Set to 0 at runtime to suppress all non-critical log writes (retail mode).
 * Set to 1 for full diagnostic logging (debug mode, default).
 * Controlled by config.ini "debugLog" field in diagnostics builds.
 * RETAIL_BUILD forces logging off.
 */
extern int g_debug_logging;

/**
 * diag_log_set_debug - Enable or disable verbose diagnostic logging.
 * @param enable  1 = debug (full logs), 0 = retail (critical only)
 */
void diag_log_set_debug(int enable);

/**
 * diag_log_write - Buffered logging to ms0:/moonlight.log with [TAG] prefix.
 * Thread-safe. Accumulates in a 4KB ring buffer; auto-flushes when full.
 * Suppressed completely in retail mode.
 *
 * @param tag  Module name (e.g., "NET", "DEC", "UI", "CTRL")
 * @param fmt  printf-style format string
 */
void diag_log_write(const char *tag, const char *fmt, ...);

/**
 * diag_log_flush - Write buffered log data to disk immediately.
 * Called periodically from main loop and at key transition points.
 */
void diag_log_flush(void);

/**
 * diag_log_clear - Remove old log files from memory stick.
 */
void diag_log_clear(void);

#if defined(RETAIL_BUILD) && !defined(DIAG_LOG_IMPLEMENTATION)
#undef diag_log_set_debug
#undef diag_log_write
#undef diag_log_flush
#undef diag_log_clear
#define diag_log_set_debug(...) ((void)0)
#define diag_log_write(...)     ((void)0)
#define diag_log_flush()        ((void)0)
#define diag_log_clear()        ((void)0)
#endif

#ifdef __cplusplus
}
#endif

#endif /* DIAG_LOG_H */
