/*
 * shared.h - Shared data structures for PSP Moonlight streaming
 *
 * Shared between the network receive thread and the decoder thread.
 *
 * Network thread: receives UDP packets, writes into PacketRingBuffer.
 * Decoder thread: reads ring buffer, runs CAVLC+VFPU decode, outputs RGBA.
 * Main thread:    reads FrameRingBuffer, displays via GPU.
 *
 * The asymmetric dual-core pipeline (Main CPU + Media Engine) is internal
 * to the software decode pipeline (sw_decode_pipeline.h).
 */

#ifndef SHARED_H
#define SHARED_H

#include <psptypes.h>

/* FrameRingBuffer definition is below PacketRingBuffer in this file */

/*============================================================================
 * Lock-Free Ring Buffer
 *============================================================================
 *
 * - Network thread (producer) writes at 'head', advances head.
 * - Main thread (consumer) reads at 'tail', advances tail.
 * - FULL when (head + 1) % SLOTS == tail  (producer drops).
 * - EMPTY when head == tail.
 * - 'volatile' forces re-read from memory on every access.
 *============================================================================*/

#define RING_BUFFER_SLOTS       512     /* Reduced to reclaim 750KB RAM for network socket buffer */
#define MAX_PACKET_SIZE         1500    /* Max UDP payload (MTU safe) */

/* PSP Bitstream queue limits */
#define NAL_QUEUE_SLOTS         2       /* Minimum queue to save ~256KB RAM */
#define MAX_NAL_SIZE            (256 * 1024)
#define FRAME_WIDTH             480
#define FRAME_HEIGHT            272
#define FRAME_STRIDE            512     /* Hardware VRAM pitch in pixels */

/*--------------------------------------------------------------------------
 * PacketRingBuffer - Lock-free ring buffer for network packet passing
 *--------------------------------------------------------------------------*/
typedef struct {
    u8  slots[RING_BUFFER_SLOTS][MAX_PACKET_SIZE];
    u16 slot_length[RING_BUFFER_SLOTS];
    volatile u32 head;
    volatile u32 tail;
} PacketRingBuffer;

#define PIXEL_SIZE              4   /* RGBA8888 */

/*--------------------------------------------------------------------------
 * JPEG Pixel Buffers (ported from PSPdisp's compress.c double-buffer)
 *
 * PSPdisp uses two alternating 480×272×4 buffers to avoid tearing:
 * one is being displayed while the other is being decoded into.
 * Each buffer is 16-byte aligned for DMA/sceJpeg compatibility.
 *--------------------------------------------------------------------------*/
#define JPEG_PIXEL_BUF_SIZE     (FRAME_WIDTH * FRAME_HEIGHT * PIXEL_SIZE)

/*--------------------------------------------------------------------------
 * Frame Ring Buffer - Decoded frame handoff from ME thread to main thread
 *--------------------------------------------------------------------------*/
#define FRAME_RING_SLOTS    8   /* Pointer queue; main drains to newest, decoder keeps latest on full */

typedef struct FrameRingBuffer {
    void *frame_data[FRAME_RING_SLOTS];     /* Pointers to RGBA8888 (32-bit) frame data */
    volatile u32 head;                       /* Decoder thread writes here */
    volatile u32 tail;                       /* Main thread reads here */
    volatile u32 frame_ready;                /* Non-zero when frames available */
} FrameRingBuffer;

/*--------------------------------------------------------------------------
 * SharedState - All shared state between network, ME decoder, and main threads
 *--------------------------------------------------------------------------*/
typedef struct {
    PacketRingBuffer packet_ring;    /* Network → ME Decoder */
    FrameRingBuffer  frame_ring;     /* ME Decoder → Main */
} SharedState;

extern SharedState g_shared;

/* Stream session status: 0=Running, 1=Stopped, 2=Paused */
extern volatile int g_stream_status;

#endif /* SHARED_H */
