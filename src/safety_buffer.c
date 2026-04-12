/*
 * safety_buffer.c - Safety Buffer implementation for PSP Moonlight
 *
 * Circular buffer that caches the last 2 seconds of H.264 NAL units.
 * Handles packet loss gracefully by showing a rewind/pause icon instead
 * of crashing the RTSP connection.
 *
 * Async spill path:
 *   The decode thread (high-priority) calls safety_buffer_store_nal().
 *   When RAM is exhausted the NAL is placed in a write-queue ring buffer.
 *   A low-priority writer thread (0x20) drains the queue to the MS file
 *   via sceIoWrite, preventing multi-ms stalls in the decode path.
 */

#include <pspkernel.h>
#include <pspthreadman.h>
#include <pspiofilemgr.h>
#include <malloc.h>
#include <stdio.h>
#include <string.h>

#include "safety_buffer.h"
#include "shared.h"

/* GU UI owns the framebuffer during normal runtime; avoid direct debug-screen writes. */
#define pspDebugScreenPrintf(...) ((void)0)

/*============================================================================
 * Storage fault flag — set on first sceIoWrite error.
 * External modules (HUD, main) can read this to show a fatal error.
 *============================================================================*/
volatile int g_storage_fault = 0;

/*============================================================================
 * Write-Queue for async MemStick spill
 *============================================================================*/

#define WRITE_QUEUE_SLOTS   16      /* Max pending writes before dropping */
#define WRITE_QUEUE_BUF_SZ  (16 * 1024)  /* 16KB — covers >99% of NAL sizes (H-1) */

typedef struct {
    u8  buf[WRITE_QUEUE_BUF_SZ];
    u32 len;
} WriteQueueSlot;

/* Lock-free SPSC ring (producer = decode thread, consumer = writer thread) */
static WriteQueueSlot      s_wq_slots[WRITE_QUEUE_SLOTS];
static volatile u32        s_wq_head = 0;   /* producer writes */
static volatile u32        s_wq_tail = 0;   /* consumer reads  */

/* Semaphore: signalled by producer, waited on by consumer */
static SceUID              s_wq_sem = -1;

/*============================================================================
 * Internal State
 *============================================================================*/

/* Global safety buffer instance */
static SafetyBuffer g_safety_buffer;

/* Initialized flag */
static int g_initialized = 0;

/* Rewind icon visibility (for HUD integration) */
static volatile int g_show_rewind_icon = 0;
static u64 g_rewind_icon_timeout = 0;

/* Current PTS counter (shared with decoder) */
static u64 g_current_pts = 0;

/*============================================================================
 * Internal Helper Functions
 *============================================================================*/

/**
 * get_current_time_us - Get current time in microseconds
 */
static u64 get_current_time_us(void)
{
    return sceKernelGetSystemTimeWide();
}

/**
 * acquire_mutex - Lock the safety buffer mutex with timeout
 *
 * Returns: 0 on success, -1 on timeout/error
 */
static int acquire_mutex(void)
{
    if (g_safety_buffer.mutex_id >= 0) {
        SceUInt timeout = 100000; /* 100ms max — longer than any sane operation */
        int r = sceKernelWaitSema(g_safety_buffer.mutex_id, 1, &timeout);
        if (r < 0) return -1; /* timeout or error — skip operation */
    }
    return 0;
}

/**
 * release_mutex - Unlock the safety buffer mutex
 */
static void release_mutex(void)
{
    if (g_safety_buffer.mutex_id >= 0) {
        sceKernelSignalSema(g_safety_buffer.mutex_id, 1);
    }
}

/**
 * allocate_from_pool - Allocate memory from the RAM pool
 *
 * @size: Number of bytes to allocate
 *
 * Returns: Pointer to allocated memory, or NULL if pool is full
 */
static u8* allocate_from_pool(u32 size)
{
    u8 *ptr;
    
    /* Align to 64 bytes for ME DMA */
    size = (size + 63) & ~63;
    
    if (g_safety_buffer.ram_pool_used + size > g_safety_buffer.ram_pool_size) {
        return NULL; /* Pool exhausted */
    }
    
    ptr = g_safety_buffer.ram_pool + g_safety_buffer.ram_pool_used;
    g_safety_buffer.ram_pool_used += size;
    
    return ptr;
}

/**
 * write_to_fallback - Enqueue a NAL unit for async write to the MS file.
 *
 * Called from the decode thread (high-priority).  Does NOT block on I/O.
 * Returns 0 if successfully enqueued, -1 if queue is full (NAL dropped).
 */
static int write_to_fallback(u8 *nal_data, u32 nal_len)
{
    u32 next_head;
    WriteQueueSlot *slot;

    if (g_safety_buffer.fallback_fd < 0)
        return -1;

    /* Silently drop NALs that exceed per-slot capacity */
    if (nal_len > WRITE_QUEUE_BUF_SZ)
        return -1;

    next_head = (s_wq_head + 1) % WRITE_QUEUE_SLOTS;
    if (next_head == s_wq_tail)
        return -1; /* Queue full — drop rather than block */

    slot = &s_wq_slots[s_wq_head];
    memcpy(slot->buf, nal_data, nal_len);
    slot->len = nal_len;

    /* Publish: advance head AFTER data is written (memory barrier via volatile) */
    s_wq_head = next_head;

    /* Wake writer thread */
    if (s_wq_sem >= 0)
        sceKernelSignalSema(s_wq_sem, 1);

    g_safety_buffer.fallback_writes++;
    return 0;
}

/**
 * evict_old_entries - Remove entries older than 2 seconds
 */
static void evict_old_entries(void)
{
    u64 current_time = g_safety_buffer.newest_timestamp;
    u64 cutoff_time;
    u32 idx;
    
    if (g_safety_buffer.newest_timestamp == 0) {
        return;
    }
    
    /* Calculate cutoff: anything older than 2 seconds */
    cutoff_time = current_time - (SAFETY_BUFFER_DURATION_MS * 1000); /* Convert ms to us */
    
    /* Evict entries older than cutoff */
    while (g_safety_buffer.read_tail != g_safety_buffer.write_head) {
        idx = g_safety_buffer.read_tail;
        
        if (g_safety_buffer.slots[idx].timestamp >= cutoff_time) {
            break; /* This entry is within the 2-second window */
        }
        
        /* Free the RAM allocation if using RAM pool */
        /* Note: We don't actually free individual allocations from the pool
         * because they're contiguous. Instead, we track the oldest entry. */
        
        /* Move to next slot */
        g_safety_buffer.read_tail = (g_safety_buffer.read_tail + 1) % SAFETY_BUFFER_SLOTS;
        
        /* Update oldest timestamp */
        g_safety_buffer.oldest_timestamp = g_safety_buffer.slots[idx].timestamp;
    }

    /* H-3: If ring is empty after eviction, reclaim entire pool */
    if (g_safety_buffer.read_tail == g_safety_buffer.write_head) {
        g_safety_buffer.ram_pool_used = 0;
    }
}

/**
 * find_last_keyframe - Find the most recent keyframe in the buffer
 *
 * Returns: Slot index of last keyframe, or -1 if none found
 */
static int find_last_keyframe(void)
{
    u32 idx;
    int last_keyframe = -1;
    
    /* Search backwards from write_head to read_tail */
    idx = (g_safety_buffer.write_head + SAFETY_BUFFER_SLOTS - 1) % SAFETY_BUFFER_SLOTS;
    
    while (idx != g_safety_buffer.read_tail) {
        if (g_safety_buffer.slots[idx].data != NULL && 
            g_safety_buffer.slots[idx].is_keyframe) {
            last_keyframe = idx;
            break;
        }
        idx = (idx + SAFETY_BUFFER_SLOTS - 1) % SAFETY_BUFFER_SLOTS;
    }
    
    return last_keyframe;
}

/*============================================================================
 * Async Writer Thread (drains write-queue to fallback file)
 *============================================================================*/

/**
 * writer_thread - Low-priority thread that flushes the write-queue to MS.
 *
 * Blocks on s_wq_sem so it never burns CPU when idle.
 * Writes: [4-byte length header][NAL data] per entry.
 */
static int writer_thread(SceSize args, void *argp)
{
    (void)args;
    (void)argp;

    while (g_safety_buffer.writer_running) {
        SceUInt timeout = 500000; /* 500 ms max block */
        sceKernelWaitSema(s_wq_sem, 1, &timeout);

        /* Drain all pending queue entries */
        while (s_wq_tail != s_wq_head) {
            WriteQueueSlot *slot = &s_wq_slots[s_wq_tail];

            if (g_safety_buffer.fallback_fd >= 0 && !g_storage_fault) {
                int wr;

                /* Write 4-byte length header */
                wr = sceIoWrite(g_safety_buffer.fallback_fd, &slot->len, 4);
                if (wr < 0) {
                    /* Storage error (MS removed, full, or corrupt) */
                    pspDebugScreenPrintf("safety_buf: write header failed (%d)\n", wr);
                    g_storage_fault = 1;
                    sceIoClose(g_safety_buffer.fallback_fd);
                    g_safety_buffer.fallback_fd = -1;
                    goto drain_skip;
                }

                /* Write NAL payload */
                wr = sceIoWrite(g_safety_buffer.fallback_fd, slot->buf, slot->len);
                if (wr < 0 || (u32)wr != slot->len) {
                    pspDebugScreenPrintf("safety_buf: write payload failed (%d)\n", wr);
                    g_storage_fault = 1;
                    sceIoClose(g_safety_buffer.fallback_fd);
                    g_safety_buffer.fallback_fd = -1;
                    goto drain_skip;
                }
            }

drain_skip:
            /* Consume: advance tail (volatile) */
            s_wq_tail = (s_wq_tail + 1) % WRITE_QUEUE_SLOTS;
        }
    }

    sceKernelExitDeleteThread(0);
    return 0;
}

/*============================================================================
 * Public API Implementation
 *============================================================================*/

int safety_buffer_init(void)
{
    if (g_initialized) {
        return 0;
    }
    
    /* Clear the buffer structure */
    memset(&g_safety_buffer, 0, sizeof(SafetyBuffer));
    
    /* Initialize file descriptor */
    g_safety_buffer.fallback_fd = -1;
    
    /* Create mutex for thread safety */
    g_safety_buffer.mutex_id = sceKernelCreateSema("safety_buf_mutex", 0, 1, 1, NULL);
    if (g_safety_buffer.mutex_id < 0) {
        pspDebugScreenPrintf("safety_buf: mutex creation failed\n");
        return -1;
    }
    
    /* Initialize write-queue indices */
    s_wq_head = 0;
    s_wq_tail = 0;

    /* Create write-queue semaphore (initial count 0; maxval = WRITE_QUEUE_SLOTS) */
    s_wq_sem = sceKernelCreateSema("safety_wq_sem", 0, 0, WRITE_QUEUE_SLOTS, NULL);
    if (s_wq_sem < 0) {
        pspDebugScreenPrintf("safety_buf: write-queue semaphore creation failed\n");
        sceKernelDeleteSema(g_safety_buffer.mutex_id);
        return -1;
    }

    /* Allocate RAM pool (500 KB for 2 seconds of video) */
    g_safety_buffer.ram_pool_size = SAFETY_BUFFER_SIZE_BYTES;
    g_safety_buffer.ram_pool = (u8*)memalign(64, g_safety_buffer.ram_pool_size);
    
    if (!g_safety_buffer.ram_pool) {
        pspDebugScreenPrintf("safety_buf: RAM allocation failed, using fallback\n");
        
        /* Open fallback file on Memory Stick */
        g_safety_buffer.fallback_fd = sceIoOpen(
            SAFETY_BUFFER_FALLBACK_PATH,
            PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC,
            0777
        );
        
        if (g_safety_buffer.fallback_fd < 0) {
            pspDebugScreenPrintf("safety_buf: fallback file open failed\n");
            sceKernelDeleteSema(g_safety_buffer.mutex_id);
            return -2;
        }
        
        g_safety_buffer.using_fallback = 1;
        pspDebugScreenPrintf("safety_buf: using MS fallback file\n");
    } else {
        pspDebugScreenPrintf("safety_buf: allocated %d KB RAM pool\n", 
                             g_safety_buffer.ram_pool_size / 1024);
    }
    
    /* Initialize slot pointers to NULL */
    for (int i = 0; i < SAFETY_BUFFER_SLOTS; i++) {
        g_safety_buffer.slots[i].data = NULL;
        g_safety_buffer.slots[i].length = 0;
    }
    
    /* Set initial state */
    g_safety_buffer.state = SAFETY_BUFFER_BUFFERING;
    g_safety_buffer.write_head = 0;
    g_safety_buffer.read_tail = 0;
    g_safety_buffer.rewind_point = 0;
    
    /* Spawn async writer thread */
    g_safety_buffer.writer_running = 1;
    g_safety_buffer.writer_thread_id = sceKernelCreateThread(
        "safety_buf_writer",
        writer_thread,
        0x20,           /* Low priority */
        0x4000,         /* 16 KB stack */
        PSP_THREAD_ATTR_USER,
        NULL
    );
    
    if (g_safety_buffer.writer_thread_id >= 0) {
        int ret = sceKernelStartThread(g_safety_buffer.writer_thread_id, 0, NULL);
        if (ret < 0) {
            pspDebugScreenPrintf("safety_buf: thread start failed (0x%2X)\n", ret);
        }
    }
    
    g_initialized = 1;
    pspDebugScreenPrintf("safety_buf: initialized\n");
    
    return 0;
}

void safety_buffer_store_nal(u8 *nal_data, u32 nal_len, u64 pts, u8 is_keyframe)
{
    SafetyBufferSlot *slot;
    u8 *data_copy;
    u32 next_head;
    
    if (!g_initialized || !nal_data || nal_len == 0) {
        return;
    }
    
    /* Skip if NAL is too large */
    if (nal_len > SAFETY_BUFFER_MAX_NAL_SIZE) {
        return;
    }
    
    if (acquire_mutex() < 0) return;
    
    /* Check if ring is full */
    next_head = (g_safety_buffer.write_head + 1) % SAFETY_BUFFER_SLOTS;
    if (next_head == g_safety_buffer.read_tail) {
        /* Ring full - evict oldest entry */
        g_safety_buffer.read_tail = (g_safety_buffer.read_tail + 1) % SAFETY_BUFFER_SLOTS;
    }
    
    /* Get slot to write to */
    slot = &g_safety_buffer.slots[g_safety_buffer.write_head];
    
    /* Free old data if present */
    /* Note: Data from RAM pool isn't individually freed, 
     * just overwritten as the circular buffer wraps */
    
    /* Allocate memory for NAL data */
    if (!g_safety_buffer.using_fallback) {
        data_copy = allocate_from_pool(nal_len);
        if (!data_copy) {
            /* RAM pool exhausted - switch to fallback */
            pspDebugScreenPrintf("safety_buf: RAM full, switching to fallback\n");
            
            g_safety_buffer.fallback_fd = sceIoOpen(
                SAFETY_BUFFER_FALLBACK_PATH,
                PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC,
                0777
            );
            
            if (g_safety_buffer.fallback_fd >= 0) {
                g_safety_buffer.using_fallback = 1;
                
                /* Write current NAL to fallback */
                write_to_fallback(nal_data, nal_len);
            }
            
            release_mutex();
            return;
        }
        
        /* Copy NAL data to pool */
        memcpy(data_copy, nal_data, nal_len);
    } else {
        /* Using fallback file - write asynchronously */
        write_to_fallback(nal_data, nal_len);
        data_copy = NULL; /* Data is on disk, not in RAM */
    }
    
    /* Update slot */
    slot->data = data_copy;
    slot->length = nal_len;
    slot->timestamp = pts;
    slot->is_keyframe = is_keyframe;
    
    /* Update timestamps */
    if (g_safety_buffer.oldest_timestamp == 0) {
        g_safety_buffer.oldest_timestamp = pts;
    }
    g_safety_buffer.newest_timestamp = pts;
    
    /* Update statistics */
    g_safety_buffer.total_nals_buffered++;
    g_safety_buffer.total_bytes_buffered += nal_len;
    
    /* Advance write head */
    g_safety_buffer.write_head = next_head;
    
    /* Update rewind point if this is a keyframe */
    if (is_keyframe) {
        g_safety_buffer.rewind_point = g_safety_buffer.write_head;
    }
    
    /* Evict old entries (older than 2 seconds) */
    evict_old_entries();
    
    /* Transition from BUFFERING to IDLE once we have enough data */
    if (g_safety_buffer.state == SAFETY_BUFFER_BUFFERING) {
        /* Buffer for ~0.5 seconds before allowing playback */
        u64 buffer_duration = g_safety_buffer.newest_timestamp - g_safety_buffer.oldest_timestamp;
        if (buffer_duration >= 500000) { /* 500ms in microseconds */
            g_safety_buffer.state = SAFETY_BUFFER_IDLE;
        }
    }
    
    release_mutex();
}

void safety_buffer_handle_packet_loss(void)
{
    if (!g_initialized) {
        return;
    }
    
    if (acquire_mutex() < 0) return;
    
    /* Set state to REWIND */
    g_safety_buffer.state = SAFETY_BUFFER_REWIND;
    g_safety_buffer.rewind_count++;
    
    /* Show rewind icon on HUD */
    g_show_rewind_icon = 1;
    g_rewind_icon_timeout = get_current_time_us() + 2000000; /* 2 seconds */
    
    release_mutex();
    
    pspDebugScreenPrintf("safety_buf: packet loss detected (rewind #%d)\n", 
                         g_safety_buffer.rewind_count);
}

SafetyBufferState safety_buffer_get_state(void)
{
    return (SafetyBufferState)g_safety_buffer.state;
}

int safety_buffer_can_rewind(void)
{
    int can_rewind = 0;
    
    if (!g_initialized) {
        return 0;
    }
    
    if (acquire_mutex() < 0) return 0;
    
    /* Check if we have any buffered data */
    if (g_safety_buffer.write_head != g_safety_buffer.read_tail) {
        /* Check if we have a keyframe to rewind to */
        if (find_last_keyframe() >= 0) {
            can_rewind = 1;
        }
    }
    
    release_mutex();
    
    return can_rewind;
}

u8* safety_buffer_rewind(u32 *out_len, u64 *out_pts)
{
    int keyframe_idx;
    SafetyBufferSlot *slot;
    
    if (!g_initialized || !out_len || !out_pts) {
        return NULL;
    }
    
    if (acquire_mutex() < 0) return NULL;
    
    /* Find last keyframe */
    keyframe_idx = find_last_keyframe();
    if (keyframe_idx < 0) {
        release_mutex();
        return NULL;
    }
    
    slot = &g_safety_buffer.slots[keyframe_idx];
    
    /* Return keyframe data */
    *out_len = slot->length;
    *out_pts = slot->timestamp;
    
    /* Move write head to after the keyframe (discard everything after) */
    g_safety_buffer.write_head = (keyframe_idx + 1) % SAFETY_BUFFER_SLOTS;
    
    /* Reset rewind point */
    g_safety_buffer.rewind_point = g_safety_buffer.write_head;
    
    /* Transition back to BUFFERING state */
    g_safety_buffer.state = SAFETY_BUFFER_BUFFERING;
    
    release_mutex();
    
    pspDebugScreenPrintf("safety_buf: rewound to keyframe at slot %d\n", keyframe_idx);
    
    return slot->data;
}

void safety_buffer_get_stats(u32 *out_total_nals, u32 *out_total_bytes, 
                             u32 *out_rewind_count)
{
    if (!g_initialized) {
        if (out_total_nals) *out_total_nals = 0;
        if (out_total_bytes) *out_total_bytes = 0;
        if (out_rewind_count) *out_rewind_count = 0;
        return;
    }
    
    if (acquire_mutex() < 0) {
        if (out_total_nals) *out_total_nals = 0;
        if (out_total_bytes) *out_total_bytes = 0;
        if (out_rewind_count) *out_rewind_count = 0;
        return;
    }
    
    if (out_total_nals) *out_total_nals = g_safety_buffer.total_nals_buffered;
    if (out_total_bytes) *out_total_bytes = g_safety_buffer.total_bytes_buffered;
    if (out_rewind_count) *out_rewind_count = g_safety_buffer.rewind_count;
    
    release_mutex();
}

void safety_buffer_clear(void)
{
    if (!g_initialized) {
        return;
    }
    
    if (acquire_mutex() < 0) return;
    
    /* Reset indices */
    g_safety_buffer.write_head = 0;
    g_safety_buffer.read_tail = 0;
    g_safety_buffer.rewind_point = 0;
    
    /* Reset RAM pool */
    g_safety_buffer.ram_pool_used = 0;
    
    /* Clear slot data pointers */
    for (int i = 0; i < SAFETY_BUFFER_SLOTS; i++) {
        g_safety_buffer.slots[i].data = NULL;
        g_safety_buffer.slots[i].length = 0;
    }
    
    /* Reset state */
    g_safety_buffer.state = SAFETY_BUFFER_BUFFERING;
    
    /* Reset statistics */
    g_safety_buffer.total_nals_buffered = 0;
    g_safety_buffer.total_bytes_buffered = 0;
    
    /* Clear fallback file if using it */
    if (g_safety_buffer.using_fallback && g_safety_buffer.fallback_fd >= 0) {
        sceIoClose(g_safety_buffer.fallback_fd);
        g_safety_buffer.fallback_fd = sceIoOpen(
            SAFETY_BUFFER_FALLBACK_PATH,
            PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC,
            0777
        );
    }
    
    release_mutex();
}

void safety_buffer_shutdown(void)
{
    if (!g_initialized) {
        return;
    }

    /* Signal writer thread to drain remaining queue and exit */
    g_safety_buffer.writer_running = 0;
    if (s_wq_sem >= 0)
        sceKernelSignalSema(s_wq_sem, 1); /* unblock the thread if sleeping */

    if (g_safety_buffer.writer_thread_id >= 0) {
        sceKernelWaitThreadEnd(g_safety_buffer.writer_thread_id, NULL);
        sceKernelDeleteThread(g_safety_buffer.writer_thread_id);
        g_safety_buffer.writer_thread_id = -1;
    }

    /* Delete write-queue semaphore */
    if (s_wq_sem >= 0) {
        sceKernelDeleteSema(s_wq_sem);
        s_wq_sem = -1;
    }

    /* Close fallback file */
    if (g_safety_buffer.fallback_fd >= 0) {
        sceIoClose(g_safety_buffer.fallback_fd);
        g_safety_buffer.fallback_fd = -1;

        /* Delete the temp file */
        sceIoRemove(SAFETY_BUFFER_FALLBACK_PATH);
    }

    /* Free RAM pool */
    if (g_safety_buffer.ram_pool) {
        free(g_safety_buffer.ram_pool);
        g_safety_buffer.ram_pool = NULL;
    }

    /* Delete mutex */
    if (g_safety_buffer.mutex_id >= 0) {
        sceKernelDeleteSema(g_safety_buffer.mutex_id);
        g_safety_buffer.mutex_id = -1;
    }

    g_initialized = 0;
    pspDebugScreenPrintf("safety_buf: shutdown complete\n");
}

/*============================================================================
 * PTS Update Function (called by decoder)
 *============================================================================*/

void safety_buffer_update_pts(u64 pts)
{
    g_current_pts = pts;
}