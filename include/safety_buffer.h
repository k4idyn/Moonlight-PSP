/*
 * safety_buffer.h - Safety Buffer for PSP Moonlight streaming
 *
 * Provides a circular buffer that caches the last 2 seconds of H.264 NAL units
 * to handle PSP's slow 802.11b WiFi stutters. When packet loss is detected,
 * the player shows a "Rewind/Pause" icon instead of crashing the RTSP connection.
 *
 * Architecture:
 * - Circular buffer in RAM stores recent NAL units
 * - Falls back to ms0:/__temp_stream file if RAM is full
 * - Async writer thread handles file I/O without blocking decode
 * - Packet loss detection triggers rewind/pause UI instead of discarding
 */

#ifndef SAFETY_BUFFER_H
#define SAFETY_BUFFER_H

#include <psptypes.h>
#include <pspkernel.h>  /* For SceUID */

/*============================================================================
 * Configuration Constants
 *============================================================================*/

/* Target buffer duration in milliseconds (2 seconds) */
#define SAFETY_BUFFER_DURATION_MS   2000

/* Estimated bitrate for 2 seconds of H.264 at 720p/30fps */
/* Conservative estimate: ~2 Mbps = 250 KB/s, so 2s = 500 KB */
#define SAFETY_BUFFER_SIZE_BYTES    (500 * 1024)

/* Maximum NAL unit size to store */
#define SAFETY_BUFFER_MAX_NAL_SIZE  (256 * 1024)

/* Number of NAL unit slots in the circular buffer */
#define SAFETY_BUFFER_SLOTS         128

/* Fallback file path on Memory Stick */
#define SAFETY_BUFFER_FALLBACK_PATH "ms0:/__temp_stream"

/*============================================================================
 * Safety Buffer State
 *============================================================================*/

typedef enum {
    SAFETY_BUFFER_IDLE,         /* Normal operation */
    SAFETY_BUFFER_BUFFERING,    /* Building up buffer */
    SAFETY_BUFFER_REWIND,       /* Packet loss detected - showing rewind icon */
    SAFETY_BUFFER_PAUSED        /* Paused waiting for network */
} SafetyBufferState;

typedef struct {
    u8  *data;                  /* Pointer to NAL unit data */
    u32  length;                /* Length of NAL unit in bytes */
    u64  timestamp;             /* PTS timestamp for ordering */
    u8   is_keyframe;           /* 1 if this is an IDR/keyframe */
} SafetyBufferSlot;

typedef struct {
    /* Circular buffer slots */
    SafetyBufferSlot slots[SAFETY_BUFFER_SLOTS];
    
    /* RAM buffer pool for NAL data storage */
    u8  *ram_pool;
    u32  ram_pool_size;
    u32  ram_pool_used;
    
    /* Circular buffer indices */
    volatile u32 write_head;    /* Next slot to write */
    volatile u32 read_tail;     /* Next slot to read (for playback) */
    volatile u32 rewind_point;  /* Slot to rewind to on packet loss */
    
    /* State machine */
    volatile SafetyBufferState state;
    
    /* Fallback file descriptor (-1 if using RAM only) */
    int fallback_fd;
    u8   using_fallback;
    
    /* Statistics */
    u32  total_nals_buffered;
    u32  total_bytes_buffered;
    u32  rewind_count;
    u32  fallback_writes;
    u32  fallback_drops;
    
    /* Timestamp tracking for 2-second window */
    u64  oldest_timestamp;
    u64  newest_timestamp;
    
    /* Async writer thread */
    SceUID writer_thread_id;
    volatile int writer_running;
    
    /* Mutex for thread safety */
    SceUID mutex_id;
    
} SafetyBuffer;

/*============================================================================
 * Public API
 *============================================================================*/

/**
 * safety_buffer_init - Initialize the safety buffer system
 *
 * Allocates RAM pool and opens fallback file if needed.
 * Spawns async writer thread for file I/O.
 *
 * Returns: 0 on success, negative on error
 */
int safety_buffer_init(void);

/**
 * safety_buffer_store_nal - Store a complete NAL unit in the buffer
 *
 * Called by decode_nal() to cache the NAL before decoding.
 * Automatically manages the 2-second circular window.
 *
 * @nal_data: Pointer to complete NAL unit data
 * @nal_len:  Length of NAL unit in bytes
 * @pts:      Presentation timestamp for ordering
 * @is_keyframe: 1 if this NAL is an IDR frame
 */
void safety_buffer_store_nal(u8 *nal_data, u32 nal_len, u64 pts, u8 is_keyframe);

/**
 * safety_buffer_handle_packet_loss - Handle detected packet loss
 *
 * Called when rtp_reassembly detects a sequence gap.
 * Sets state to REWIND and displays the rewind/pause icon.
 */
void safety_buffer_handle_packet_loss(void);

/**
 * safety_buffer_get_state - Get current buffer state
 *
 * Returns: Current SafetyBufferState
 */
SafetyBufferState safety_buffer_get_state(void);

/**
 * safety_buffer_can_rewind - Check if buffer has data to rewind to
 *
 * Returns: 1 if rewind is possible, 0 if buffer is empty
 */
int safety_buffer_can_rewind(void);

/**
 * safety_buffer_rewind - Rewind to last keyframe in buffer
 *
 * Returns: Pointer to NAL data to replay, or NULL if no keyframe available
 *          Caller must NOT free the returned pointer
 */
u8* safety_buffer_rewind(u32 *out_len, u64 *out_pts);

/**
 * safety_buffer_get_stats - Get buffer statistics
 *
 * @out_total_nals: Output total NALs buffered
 * @out_total_bytes: Output total bytes buffered
 * @out_rewind_count: Output number of rewinds performed
 */
void safety_buffer_get_stats(u32 *out_total_nals, u32 *out_total_bytes, 
                             u32 *out_rewind_count);

/**
 * safety_buffer_clear - Clear the buffer and reset state
 *
 * Called when seeking or reconnecting.
 */
void safety_buffer_clear(void);

/**
 * safety_buffer_shutdown - Clean up all resources
 *
 * Stops writer thread, closes file, frees RAM pool.
 */
void safety_buffer_shutdown(void);

/*============================================================================
 * HUD Integration
 * These functions are declared in hud.h — include that header to use them.
 * safety_buffer.c calls hud_show_rewind_icon() and hud_hide_rewind_icon().
 *============================================================================*/

#endif /* SAFETY_BUFFER_H */