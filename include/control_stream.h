/*
 * control_stream.h - Minimal ENet control channel for Sunshine
 *
 * Implements the bare-minimum ENet handshake (CONNECT → VERIFY_CONNECT → ACK)
 * and sends START_A + START_B messages required before Sunshine will stream.
 * Also sends periodic control pings to keep the session alive.
 */

#ifndef CONTROL_STREAM_H
#define CONTROL_STREAM_H

/*
 * control_stream_start - Connect to Sunshine control port via ENet and
 *                        send START_A + START_B.
 *
 * Must be called AFTER RTSP PLAY succeeds and BEFORE video/audio threads
 * expect to receive data.  Blocks until the handshake completes or times out.
 *
 * Returns: 0 on success, negative on error
 */
int control_stream_start(void);

/*
 * control_stream_stop - Shut down the control channel.
 */
void control_stream_stop(void);

/*
 * control_stream_request_idr - Request an IDR frame from the host.
 *
 * Sends a START_A (0x0302) control message to the Sunshine server
 * which triggers it to generate a fresh IDR frame.
 *
 * Returns: 0 on success, negative on error
 */
int control_stream_request_idr(void);

/*
 * control_stream_request_rfi - Request Reference Frame Invalidation.
 *
 * Tells Sunshine frames [start_frame..end_frame] were lost so
 * the encoder avoids referencing them and sends a recovery P-frame.
 *
 * Returns: 0 on success, negative on error
 */
int control_stream_request_rfi(unsigned int start_frame, unsigned int end_frame);

/*
 * control_stream_send_input - Send an input packet through the encrypted
 *                             control channel using unsequenced ENet delivery.
 *
 * Unsequenced delivery is critical on the PSP's lightweight ENet implementation
 * which lacks retransmission.  With reliable sends, a single lost WiFi packet
 * causes the server to buffer ALL subsequent input until the retransmission
 * that never comes, permanently killing input.
 *
 * @payload:     Pointer to the input packet (e.g. 34-byte NV_MULTI_CONTROLLER_PACKET).
 * @payload_len: Length of the payload.
 * @channel:     ENet channel (0x10=gamepad0, 0x03=mouse, 0x02=keyboard).
 *
 * Returns: number of bytes sent on success, negative on error.
 */
int control_stream_send_input(const unsigned char *payload, int payload_len,
                              unsigned char channel);

/*
 * control_stream_send_fec_status - Send per-frame FEC status (0x5502) to Sunshine.
 * Called after each frame is submitted to reassembly.
 */
int control_stream_send_fec_status(unsigned int frame_index,
                                   unsigned short highest_seq,
                                   unsigned short next_contig_seq,
                                   unsigned short missing_before_highest,
                                   unsigned short total_data,
                                   unsigned short total_parity,
                                   unsigned short received_data,
                                   unsigned short received_parity,
                                   unsigned char  fec_pct,
                                   unsigned char  multi_fec_idx,
                                   unsigned char  multi_fec_cnt);

#endif /* CONTROL_STREAM_H */
