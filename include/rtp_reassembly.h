/*
 * rtp_reassembly.h - Sunshine/Moonlight video packet reassembly
 *
 * This module handles RTP packets carrying Sunshine NV_VIDEO_PACKET headers
 * and reassembles per-frame H.264 payload for the ME decoder.
 *
 * Usage:
 * 1. Call rtp_reassembly_reset() when starting a new stream
 * 2. For each UDP packet received, call rtp_reassembly_process_packet()
 * 3. When a complete NAL unit is assembled, decode_nal() will be called
 *    automatically (you must implement this function in your decoder)
 *
 * The module uses a static assembly buffer and tracks RTP/stream packet
 * sequence numbers to detect packet loss/reordering.
 */

#ifndef RTP_REASSEMBLY_H
#define RTP_REASSEMBLY_H

#include <psptypes.h>

/*--------------------------------------------------------------------------
 * Public API Functions
 *--------------------------------------------------------------------------*/

/**
 * Process a single RTP packet from the network
 * 
 * This function parses RTP + NV_VIDEO_PACKET headers and reassembles frame
 * payload. When a frame is complete, it calls decode_nal() automatically.
 *
 * @param packet     Pointer to the UDP payload (starts with 12-byte RTP header)
 * @param packet_len Total length of the UDP payload in bytes
 */
void rtp_reassembly_process_packet(u8 *packet, int packet_len);

/**
 * Reset the reassembly state
 *
 * Call this function when:
 * - Starting a new video stream
 * - Recovering from errors
 * - Switching to a different video source
 *
 * This clears the assembly buffer and resets sequence number tracking.
 */
void rtp_reassembly_reset(void);

/* Replayed frames captured before decoder_ready and now ready. */
void rtp_reassembly_flush_pre_ready_frames(void);

/* Flush partially assembled frame (e.g. on START_A request) */
void rtp_reassembly_flush_partial_frame(void);

/*--------------------------------------------------------------------------
 * Required External Function
 *
 * You must implement this function in your decoder module
 * (e.g., decoder_me_thread.c).  It will be called automatically when
 * a complete NAL unit has been assembled.
 *
 * void decode_nal(u8 *nal_data, int nal_len, u32 pts, int corrupted);
 *   @param nal_data   Pointer to the complete NAL unit data
 *   @param nal_len    Length of the NAL unit in bytes
 *   @param pts        RTP timestamp for the frame
 *   @param corrupted  Non-zero if packet loss was detected
 *--------------------------------------------------------------------------*/

#endif /* RTP_REASSEMBLY_H */