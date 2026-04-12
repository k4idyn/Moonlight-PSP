/*
 * decode_flags.h - Cross-module volatile decode state flags
 *
 * Centralizes extern declarations for volatile flags shared between
 * the decode, RTP, FEC, and control threads.  All definitions remain
 * in their original .c files; this header provides the single
 * authoritative declaration to prevent type-mismatch drift.
 *
 * MIPS Allegrex: volatile aligned word = sufficient memory barrier.
 */

#ifndef DECODE_FLAGS_H
#define DECODE_FLAGS_H

/* Reference corruption guard (defined in ffmpeg_decode.c) */
extern volatile int g_refs_corrupted;

/* Set when IDR is fully decoded (defined in ffmpeg_decode.c) */
extern volatile int g_idr_fully_decoded;

/* Per-frame corruption flag (defined in ffmpeg_decode.c) */
extern volatile int g_current_frame_is_corrupt;

/* CABAC detection flag (defined in sw_decoder_thread.c via check_nal_for_cabac) */
extern volatile int g_cabac_detected;

/* Frame counter for loss stats (defined in control_stream.c) */
extern volatile unsigned int g_last_good_frame;

/* FEC-requested IDR flag to avoid duplicate IDR requests (defined in rtp_fec.c) */
extern volatile int g_fec_requested_idr;

/* Watchdog restart signal for resetting function-level statics (defined in ffmpeg_decode.c) */
extern volatile int g_decode_counters_reset_pending;

/* Crypto fatal flag for consecutive decrypt failures (defined in stream_crypto.c) */
extern volatile int g_crypto_fatal;

/* FEC recovery clean flag (defined in rtp_fec.c)
 * Set to 1 by the FEC layer when RS recovery succeeds for the current frame.
 * Reassembly checks this to suppress false seq-gap corruption marking when
 * FEC already verified data integrity. Reset per-frame in submit. */
extern volatile int g_fec_recovery_clean;

#endif /* DECODE_FLAGS_H */
