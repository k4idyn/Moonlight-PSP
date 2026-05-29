/*
 * power_handler.h - PSP Power Switch (Suspend/Resume) Handler for Moonlight
 *
 * Handles the PSP's physical power switch toggle to sleep mode:
 * - On Suspend: Sends PAUSE command to Moonlight stream and caches session token
 * - On Resume: Performs Quick Reconnect using cached token without re-pairing
 *
 * Uses scePowerRegisterCallback to detect suspend/resume events.
 * Quick Reconnect uses sceNetInetConnect with 5-second timeout.
 */

#ifndef POWER_HANDLER_H
#define POWER_HANDLER_H

#include <psptypes.h>
#include "storage_paths.h"

#ifdef __cplusplus
extern "C" {
#endif

/*--------------------------------------------------------------------------
 * Configuration Constants
 *--------------------------------------------------------------------------*/

/* Session token cache file path on Memory Stick */
#define SESSION_TOKEN_CACHE_PATH    MOONLIGHT_SAVE_DIR "/session_token.bin"

/* Session info cache file path */
#define SESSION_INFO_CACHE_PATH     MOONLIGHT_SAVE_DIR "/session_info.bin"

/* Quick reconnect timeout in seconds */
#define QUICK_RECONNECT_TIMEOUT_SEC 5

/* RTSP port for reconnection */
#define RTSP_RECONNECT_PORT         48010

/*--------------------------------------------------------------------------
 * Session Token Structure
 *
 * Contains all data needed for quick reconnection without re-pairing.
 *--------------------------------------------------------------------------*/
typedef struct {
    /* Authentication token (from pairing) */
    unsigned char auth_token[16];
    int auth_token_len;
    
    /* Server certificate hash */
    unsigned char cert_hash[32];
    int cert_hash_len;
  
    /* Server address and ports */
    char server_address[64];
    unsigned short rtsp_port;
    unsigned short http_port;
    
    /* Session ID from RTSP */
    char session_id[64];
    
    /* Stream configuration */
    int width;
    int height;
    int fps;
    int bitrate;
    
    /* Client unique ID */
    char unique_id[32];
    
    /* Timestamp of cache creation */
    u32 cache_timestamp;
    
    /* Validity flag */
    int is_valid;
} SessionToken;

/*--------------------------------------------------------------------------
 * Power State Enum
 *--------------------------------------------------------------------------*/
typedef enum {
    POWER_STATE_AWAKE = 0,
    POWER_STATE_SUSPENDING,
    POWER_STATE_SUSPENDED,
    POWER_STATE_RESUMING,
    POWER_STATE_RESUME_FAILED
} PowerState;

/*--------------------------------------------------------------------------
 * Power Handler Statistics
 *--------------------------------------------------------------------------*/
typedef struct {
    u32 suspend_count;
    u32 resume_count;
    u32 quick_reconnect_success;
    u32 quick_reconnect_failed;
    u32 last_suspend_time;
    u32 last_resume_time;
    u32 total_suspend_duration_ms;
} PowerHandlerStats;

/*--------------------------------------------------------------------------
 * Public API
 *--------------------------------------------------------------------------*/

/**
 * power_handler_init - Initialize the power switch handler
 *
 * Registers a callback with scePowerRegisterCallback to detect
 * suspend/resume events. Initializes session token cache.
 *
 * Returns: 0 on success, negative on error
 */
int power_handler_init(void);

/**
 * power_handler_shutdown - Shutdown the power handler
 *
 * Unregisters the power callback and cleans up resources.
 */
void power_handler_shutdown(void);

/**
 * power_handler_get_state - Get current power state
 *
 * Returns: Current PowerState
 */
PowerState power_handler_get_state(void);

/**
 * power_handler_is_suspended - Check if system is currently suspended
 *
 * Returns: 1 if suspended, 0 if awake
 */
int power_handler_is_suspended(void);

/**
 * power_handler_cache_session_token - Cache current session for quick reconnect
 *
 * Saves authentication token, server info, and stream configuration
 * to ms0:/PSP/SAVEDATA/Moonlight/session_token.bin for use after resume.
 *
 * Returns: 0 on success, negative on error
 */
int power_handler_cache_session_token(void);

/**
 * power_handler_quick_reconnect - Attempt quick reconnection after resume
 *
 * Uses cached session token to reconnect without re-pairing:
 * 1. Loads cached session token from savedata
 * 2. Creates TCP socket and connects to RTSP port with 5-second timeout
 * 3. Sends RTSP PLAY command to resume stream
 * 4. Restarts network receive thread
 *
 * Returns: 0 on success, negative on error
 */
int power_handler_quick_reconnect(void);

/**
 * power_handler_invalidate_cache - Invalidate cached session token
 *
 * Call this when the session ends normally or when re-pairing is needed.
 */
void power_handler_invalidate_cache(void);

/**
 * power_handler_is_cache_valid - Check if cached session token exists and is valid
 *
 * Returns: 1 if valid cache exists, 0 otherwise
 */
int power_handler_is_cache_valid(void);

/**
 * power_handler_get_stats - Get power handler statistics
 *
 * @stats: Pointer to PowerHandlerStats structure to populate
 */
void power_handler_get_stats(PowerHandlerStats *stats);

/**
 * power_handler_reset_stats - Reset power handler statistics
 */
void power_handler_reset_stats(void);

#ifdef __cplusplus
}
#endif

#endif /* POWER_HANDLER_H */
