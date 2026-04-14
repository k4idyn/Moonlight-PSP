/*!
 * moonlight_openh264.h — OpenH264 decoder integration API for Moonlight-PSP
 *
 * This header provides a minimal, C-linkage wrapper around the OpenH264
 * ISVCDecoder interface, tuned for Constrained Baseline Profile H.264 streams
 * as produced by Sunshine / Nvidia GameStream hosts.
 *
 * Design goals for PSP-1000 (32 MB RAM, single Allegrex core):
 *  • One decoder context per session (no multi-instance overhead).
 *  • Decoder ← Constrained Baseline, Level 3.1 maximum (720p30 / 480p60).
 *  • Output is always YUV 4:2:0 planar; convert to PSP swizzled GU format
 *    in the caller with the Media Engine or the GU CLUT path.
 *  • All functions are safe to call from a single PSP user-thread.
 *
 * Typical Moonlight-PSP call sequence
 * ------------------------------------
 *  MoonH264Decoder *dec = moonh264_create();
 *  moonh264_init(dec, width, height);
 *
 *  // Per-network-packet (RTP payload already stripped):
 *  MoonH264Frame frame = {0};
 *  int rc = moonh264_decode(dec, nal_buf, nal_len, &frame);
 *  if (rc == MOONH264_OK && frame.got_picture) {
 *      // frame.y / frame.u / frame.v → upload to GU texture
 *  }
 *
 *  moonh264_destroy(dec);
 *
 * Copyright (c) 2024, Moonlight-PSP contributors.
 * Based on OpenH264  Copyright (c) 2013-2024, Cisco Systems.
 * BSD 2-Clause licence — see openh264-master/LICENSE.
 */

#ifndef MOONLIGHT_OPENH264_PSP_H
#define MOONLIGHT_OPENH264_PSP_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* -------------------------------------------------------------------------
 * Return codes
 * ---------------------------------------------------------------------- */

#define MOONH264_OK            0   /* Frame decoded (check got_picture).   */
#define MOONH264_ERR_OOM      -1   /* Allocation failure.                   */
#define MOONH264_ERR_INIT     -2   /* Decoder initialisation failed.        */
#define MOONH264_ERR_DECODE   -3   /* ISVCDecoder returned an error.        */
#define MOONH264_ERR_PARAM    -4   /* Invalid argument.                     */

/* -------------------------------------------------------------------------
 * YUV 4:2:0 output frame
 *
 * Pointers reference internal decoder memory and are valid only until the
 * next call to moonh264_decode().  Copy the planes before that if needed.
 * ---------------------------------------------------------------------- */

typedef struct {
  uint8_t *y;          /* Luma plane   (width × height bytes)              */
  uint8_t *u;          /* Cb plane     ((width/2) × (height/2) bytes)      */
  uint8_t *v;          /* Cr plane     ((width/2) × (height/2) bytes)      */
  int      stride_y;   /* Luma row stride   (may be > width for alignment) */
  int      stride_uv;  /* Chroma row stride (may be > width/2)             */
  int      width;      /* Decoded frame width  in pixels                   */
  int      height;     /* Decoded frame height in pixels                   */
  int      got_picture; /* Non-zero when y/u/v are valid                   */
} MoonH264Frame;

/* -------------------------------------------------------------------------
 * Opaque decoder handle
 * ---------------------------------------------------------------------- */

typedef struct MoonH264Decoder MoonH264Decoder;

/* -------------------------------------------------------------------------
 * API
 * ---------------------------------------------------------------------- */

/**
 * moonh264_create — allocate a decoder handle.
 *
 * Returns NULL on allocation failure.  The returned handle must be freed
 * with moonh264_destroy() even if moonh264_init() subsequently fails.
 */
MoonH264Decoder *moonh264_create(void);

/**
 * moonh264_init — initialise the ISVCDecoder for a streaming session.
 *
 * @param dec      Decoder handle from moonh264_create().
 * @param width    Expected stream width  in pixels (e.g. 480).
 * @param height   Expected stream height in pixels (e.g. 272 for PSP native).
 *
 * Configures Constrained Baseline Profile, single-thread operation, and
 * sets the target decode buffer to the full-picture path (no slice-MT).
 *
 * Returns MOONH264_OK on success.
 */
int moonh264_init(MoonH264Decoder *dec, int width, int height);

/**
 * moonh264_decode — decode one NAL unit or a sequence of NAL units.
 *
 * @param dec       Decoder handle (must have been successfully initialised).
 * @param buf       Pointer to H.264 bitstream data.  The buffer MUST start
 *                  with a 4-byte start code (0x00 0x00 0x00 0x01) or a 3-byte
 *                  start code (0x00 0x00 0x01).  Moonlight delivers
 *                  Annex-B formatted NALUs, so no conversion is needed.
 * @param buf_len   Number of bytes in buf.
 * @param frame_out Output frame descriptor populated on success.
 *
 * Returns MOONH264_OK even when got_picture == 0 (B-frames, parameter sets).
 * Returns MOONH264_ERR_DECODE on fatal decoder errors.
 */
int moonh264_decode(MoonH264Decoder *dec,
                    const uint8_t   *buf,
                    int              buf_len,
                    MoonH264Frame   *frame_out);

/**
 * moonh264_flush — flush any frames buffered inside the decoder.
 *
 * Call this when the stream is interrupted (e.g. packet loss recovery,
 * Moonlight video re-sync event) to discard stale reference frames and
 * force the next IDR to be decoded cleanly.
 */
void moonh264_flush(MoonH264Decoder *dec);

/**
 * moonh264_destroy — release all resources associated with a decoder handle.
 *
 * Safe to call with dec == NULL.
 */
void moonh264_destroy(MoonH264Decoder *dec);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* MOONLIGHT_OPENH264_PSP_H */
