/*
 * rtp_fec.h - Reed-Solomon FEC recovery for RTP video packets
 *
 * Implements per-frame FEC recovery matching moonlight-common-c's
 * RtpVideoQueue approach. Buffers data+parity packets for each frame
 * and uses RS to recover lost data packets before passing to reassembly.
 */
#ifndef RTP_FEC_H
#define RTP_FEC_H

#include <psptypes.h>

/* Maximum packets per FEC block (data + parity).
 * At 500kbps/480x272/15fps, IDRs are ~4KB = ~3 data + 1 parity = 4 total.
 * 48 covers up to ~40 data packets at 20% FEC.  Beyond this, FEC is bypassed
 * and the reassembly layer handles the frame directly (gap-tolerant).
 * Memory: 128 * 1500 * 2 = ~384KB static BSS (safe for PSP-1000). */
#define FEC_MAX_PACKETS   128

/* Initialize FEC subsystem (call once at startup) */
void rtp_fec_init(void);

/* Process a raw RTP packet. Returns:
 *  1 = packet consumed by FEC (do NOT process in reassembly)
 *  0 = packet should be processed normally by reassembly
 * When a frame is fully recovered, rtp_fec calls decode_nal() directly. */
int rtp_fec_add_packet(const u8 *packet, int packet_len);

/* Reset FEC state (call on stream restart) */
void rtp_fec_reset(void);

/* Last FEC status values for CTRL PING piggyback.
 * Updated by rtp_fec.c on every frame submit so the keepalive
 * can echo real values instead of confusing the server with zeros. */
extern volatile u16 g_fec_last_highest_seq;
extern volatile u16 g_fec_last_next_contig_seq;
extern volatile u16 g_fec_last_data_pkts;
extern volatile u16 g_fec_last_parity_pkts;
extern volatile u16 g_fec_last_recv_data;
extern volatile u16 g_fec_last_recv_parity;
extern volatile u8  g_fec_last_fec_pct;

#endif /* RTP_FEC_H */
