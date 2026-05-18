/*!
 * moonlight_openh264.cpp — Moonlight-PSP OpenH264 decoder wrapper
 *
 * Implements the C API declared in moonlight_openh264.h using the
 * ISVCDecoder C++ interface provided by OpenH264.
 *
 * Build this file together with libopenh264_dec_psp.a, e.g.:
 *
 *   psp-g++ -O2 -march=allegrex -mabi=eabi -G0          \
 *           -I../codec/api/wels -I../codec/common/inc    \
 *           -I../codec/decoder/core/inc                  \
 *           -c moonlight_openh264.cpp -o moonlight_openh264.o
 *
 *   psp-ar cr libmoonlight_openh264.a moonlight_openh264.o
 *   # Then link: -lmoonlight_openh264 -lopenh264_dec_psp -lstdc++ -lc
 */

#include "moonlight_openh264.h"

/* OpenH264 public C++ API */
#include "codec_api.h"    /* ISVCDecoder, WelsCreateDecoder, etc. */
#include "codec_def.h"    /* SDecodingParam, DECODING_STATE, etc. */

#include <cstdlib>
#include <cstring>

/* -------------------------------------------------------------------------
 * Internal decoder context
 * ---------------------------------------------------------------------- */

struct MoonH264Decoder {
  ISVCDecoder *decoder;
  int          width;
  int          height;
};

/* -------------------------------------------------------------------------
 * moonh264_create
 * ---------------------------------------------------------------------- */

MoonH264Decoder *moonh264_create(void) {
  MoonH264Decoder *ctx =
      static_cast<MoonH264Decoder *>(malloc(sizeof(MoonH264Decoder)));
  if (!ctx)
    return NULL;
  ctx->decoder = NULL;
  ctx->width   = 0;
  ctx->height  = 0;
  return ctx;
}

/* -------------------------------------------------------------------------
 * moonh264_init
 * ---------------------------------------------------------------------- */

int moonh264_init(MoonH264Decoder *dec, int width, int height) {
  if (!dec || width <= 0 || height <= 0)
    return MOONH264_ERR_PARAM;

  /* Tear down any previous session cleanly. */
  if (dec->decoder) {
    dec->decoder->Uninitialize();
    WelsDestroyDecoder(dec->decoder);
    dec->decoder = NULL;
  }

  /* Create a fresh ISVCDecoder instance. */
  if (WelsCreateDecoder(&dec->decoder) != 0 || dec->decoder == NULL)
    return MOONH264_ERR_OOM;

  /* ---- Decoding parameters ------------------------------------------- */

  SDecodingParam param;
  memset(&param, 0, sizeof(param));

  /*
   * VIDEO_CODING_LAYER: decode an ordinary H.264 Annex-B stream.
   * Sunshine / GameStream sends Constrained Baseline Profile which is a
   * strict subset of Baseline, so no special profile hint is required.
   */
  param.sVideoProperty.size         = sizeof(SVideoProperty);
  param.sVideoProperty.eVideoBsType = VIDEO_BITSTREAM_AVC;

  /*
   * eEcActiveIdc = ERROR_CON_DISABLE
   * Match the live Moonlight-PSP path: avoid concealed blocky output and let
   * the caller hold the last clean frame while transport recovery catches up.
   */
  param.eEcActiveIdc = ERROR_CON_DISABLE;

  /*
   * bParseOnly = false: we want fully decoded YUV output, not bitstream
   * parsing only.
   */
  param.bParseOnly = false;

  long rc = dec->decoder->Initialize(&param);
  if (rc != 0) {
    WelsDestroyDecoder(dec->decoder);
    dec->decoder = NULL;
    return MOONH264_ERR_INIT;
  }

  /*
   * Hint the decoder about expected output dimensions.  OpenH264 re-allocates
   * buffers if the actual SPS dimensions differ, but providing a hint avoids
   * a reallocation on the very first IDR frame, which matters on PSP where
   * heap fragmentation is expensive.
   *
   * Option key 0: number of decode threads (1 = no MT, safest on PSP-1000).
   */
  int thread_count = 1;
  dec->decoder->SetOption(DECODER_OPTION_NUM_OF_THREADS, &thread_count);

  dec->width  = width;
  dec->height = height;
  return MOONH264_OK;
}

/* -------------------------------------------------------------------------
 * moonh264_decode
 * ---------------------------------------------------------------------- */

int moonh264_decode(MoonH264Decoder *dec,
                    const uint8_t   *buf,
                    int              buf_len,
                    MoonH264Frame   *frame_out) {
  if (!dec || !dec->decoder || !buf || buf_len <= 0 || !frame_out)
    return MOONH264_ERR_PARAM;

  memset(frame_out, 0, sizeof(*frame_out));

  /*
   * OpenH264 DecodeFrameNoDelay() bypasses the internal DPB delay introduced
   * by B-frame reordering.  Sunshine / GameStream streams are Constrained
   * Baseline (no B-frames), so this is always safe and gives minimum latency —
   * critical for interactive game streaming.
   */
  uint8_t *dst[3] = {NULL, NULL, NULL};
  SBufferInfo buf_info;
  memset(&buf_info, 0, sizeof(buf_info));

  DECODING_STATE state =
      dec->decoder->DecodeFrameNoDelay(buf,
                                       static_cast<int>(buf_len),
                                       dst,
                                       &buf_info);

  if (state != dsErrorFree && state != dsDataErrorConcealed) {
    if (state & dsNoParamSets || state & dsRefLost)
      return MOONH264_OK;  /* Waiting for IDR — not fatal. */
    return MOONH264_ERR_DECODE;
  }

  if (buf_info.iBufferStatus == 1) {
    /* A complete picture is available. */
    frame_out->y         = dst[0];
    frame_out->u         = dst[1];
    frame_out->v         = dst[2];
    frame_out->stride_y  = buf_info.UsrData.sSystemBuffer.iStride[0];
    frame_out->stride_uv = buf_info.UsrData.sSystemBuffer.iStride[1];
    frame_out->width     = buf_info.UsrData.sSystemBuffer.iWidth;
    frame_out->height    = buf_info.UsrData.sSystemBuffer.iHeight;
    frame_out->got_picture = 1;
  }

  return MOONH264_OK;
}

/* -------------------------------------------------------------------------
 * moonh264_flush
 * ---------------------------------------------------------------------- */

void moonh264_flush(MoonH264Decoder *dec) {
  if (!dec || !dec->decoder)
    return;

  /*
   * DECODER_OPTION_IDR_PIC_ID can be used to request a clean-random-access
   * point, but the most reliable flush on OpenH264 is to drain pending frames
   * via DecodeFrameNoDelay with a zero-byte buffer, then reset the decoder
   * state so the next IDR is accepted unconditionally.
   */
  uint8_t *dst[3] = {NULL, NULL, NULL};
  SBufferInfo buf_info;
  memset(&buf_info, 0, sizeof(buf_info));

  /* Flush any frames that are still in the DPB. */
  dec->decoder->FlushFrame(dst, &buf_info);
}

/* -------------------------------------------------------------------------
 * moonh264_destroy
 * ---------------------------------------------------------------------- */

void moonh264_destroy(MoonH264Decoder *dec) {
  if (!dec)
    return;
  if (dec->decoder) {
    dec->decoder->Uninitialize();
    WelsDestroyDecoder(dec->decoder);
    dec->decoder = NULL;
  }
  free(dec);
}
