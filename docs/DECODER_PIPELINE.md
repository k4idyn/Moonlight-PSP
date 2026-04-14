# OpenH264 + ME Dual-Core Decode Pipeline

_PSP Moonlight - OpenH264 low-latency decoder_

This document describes the active H.264 decode pipeline used in PSP Moonlight. The prior
`sceMpeg`-based approach (see `docs/ME_DECODER_THREAD_README.md` for historical context) was
abandoned because the `sceMpeg` ringbuffer model is incompatible with the Moonlight RTP stream
model. The legacy FFmpeg pipeline has been replaced by a custom OpenH264 PSP port optimised
for low-latency streaming. Everything in this document describes the current software pipeline.

---

## Overview

```
Network              Main CPU (333 MHz)                Media Engine (222 MHz)
--------         -----------------------------         ---------------------
UDP socket
    |
    v
rtp_reassembly   <- RTP header strip + frame boundary
    |
    v
rtp_fec          <- Reed-Solomon FEC repair (up to 66% parity)
    |
    v
sw_decoder_thread <- frame queued to decode
    |
    v
oh264_pipeline_decode_frame()
    |
    +--[DecodeFrameNoDelay()]-------------------+
    |       pData[0]=Y, [1]=Cb, [2]=Cr          | OpenH264 H.264
    |       SBufferInfo{ iBufferStatus, strides }| Baseline + CABAC
    |                                            | Single-threaded
    v                                            |
ME dispatch (BeginME)                           -+
    |
    +--[BeginME(me_yuv420_to_rgba_entry)]--------> VFPU YUV->RGBA
                                                    ~31 us/frame
                                                    (async, concurrent
                                                     with next decode)
```

Throughput = `max(OpenH264_decode_ms, ME_convert_ms)` - not their sum.

### Low-Latency Advantages Over Legacy FFmpeg Path

| Feature | FFmpeg (legacy) | OpenH264 (current) |
|---|---|---|
| Decode API | send_packet + receive_frame (2-call) | DecodeFrameNoDelay (single call, immediate) |
| Internal buffering | May buffer frames for reordering | No reorder buffering (streaming mode) |
| Thread overhead | Thread-safe mutexes (unused on PSP) | Compiled with DISABLE_DECODER_MT, zero sync |
| Error concealment | Full re-decode on error | SLICE_COPY - copies previous slice, no re-decode |
| CABAC support | Not available in PSP build | Native CABAC+CAVLC support |
| Trace logging | Always active | Disabled via DECODER_OPTION_TRACE_LEVEL=0 |
| Source control | Binary cross-compiled library | Full source - recompilable with PSP-specific flags |

---

## Pipeline Lifecycle

### Init - `oh264_pipeline_init()`

Located in `src/openh264_decode.cpp`.

1. `WelsCreateDecoder()` - create OpenH264 decoder instance
2. Configure `SDecodingParam`:
   - `eVideoBsType = VIDEO_BITSTREAM_AVC` - Annex-B NAL input
   - `eEcActiveIdc = ERROR_CON_SLICE_COPY` - fast error concealment
   - `bParseOnly = false` - full decode, immediate output
3. `ISVCDecoder::Initialize()` - initialise decoder
4. Post-init low-latency tuning:
   - `DECODER_OPTION_NUM_OF_THREADS = 0` - explicit single-thread (matches PSP hardware)
   - `DECODER_OPTION_TRACE_LEVEL = 0` - disable internal logging (~200us/frame saved)
5. Allocate RGBA double-buffer (64-byte aligned, from `g_stream_res`)
6. Load `moonlight_me_helper.prx`, call `InitME()` for ME dual-core dispatch
7. Clear `g_refs_corrupted`, all counters
8. Return 0 on success, negative on failure

### Per-Frame Decode - `oh264_pipeline_decode_frame()`

Located in `src/openh264_decode.cpp`.

**Entry conditions:**
- `nal_data` - pointer to Annex-B access unit (starts with 0x00000001)
- `nal_len` - byte count
- Called from `sw_decoder_thread.c` decode loop

**Step-by-step:**

1. **Collect previous ME frame** - if an ME job is pending from the previous call, wait for it (with timeout). This implements the double-buffer hand-off: the ME converted the *previous* frame's YUV while the main CPU was doing other work.

2. **DecodeFrameNoDelay()** - submit NAL data and receive decoded I420 planes in a single call.
   - Returns `DECODING_STATE` bitmask:
     - `dsErrorFree (0x00)` - clean decode
     - `dsDataErrorConcealed (0x20)` - error concealed via SLICE_COPY, output MAY be valid
     - `dsRefLost (0x02)` - reference frame missing
     - `dsBitstreamError (0x04)` - broken bitstream
   - **Key insight:** `iBufferStatus == 1` means usable output regardless of error bits. Concealed frames have minor visual artifacts but are far better than stale frames. Only discard when `iBufferStatus == 0` AND error bits are set.

3. **Output validation** - read `src_w`, `src_h`, `y_stride`, `uv_stride` from `SBufferInfo`. Clamp to `g_stream_res` if dimensions are unexpected.

4. **ME dispatch** - write Y/Cb/Cr pointers and strides to `MeYuv2RgbaParams`, flush dcache, `BeginME()`. ME core starts VFPU conversion immediately.
   - Main CPU does **not** call `WaitME()` here - the wait is deferred to step 1 of the *next* decode call.

5. **Output** - set `*out_rgba` to the output from the *previous* ME job (now complete). This is the double-buffer hand-off that hides ME latency.

6. **Reference state update** - on `dsErrorFree`, clear `g_refs_corrupted`. On concealed frames, set `g_idr_fully_decoded` but leave `g_refs_corrupted` as-is.

### Flush - `oh264_pipeline_flush_buffers()`

Located in `src/openh264_decode.cpp`.

Called when a queue overrun is detected (ring buffer fell behind). Steps:
1. Feed `NULL` to `DecodeFrameNoDelay()` - drains any buffered frames from the decoder.
2. Caller (in `sw_decoder_thread.c`) separately sets `g_refs_corrupted = 1` and calls `control_stream_request_idr()`.

### Invalidate Refs - `oh264_pipeline_invalidate_refs()`

Located in `src/openh264_decode.cpp`.

Lightweight variant: sets `g_refs_corrupted = 1` and clears `g_saw_first_idr` without flushing decoder state. Used when FEC repair fails (RS erasure limit exceeded) - the frame is corrupt but the decoder can limp forward with error concealment.

### Abandon - `oh264_pipeline_abandon()`

Located in `src/openh264_decode.cpp`.

**Emergency path** - called after `sceKernelTerminateThread()` on the decoder thread. Because OpenH264 may hold internal state during a decode, calling `Uninitialize()`/`WelsDestroyDecoder()` on a killed thread risks accessing corrupted memory. `_abandon()` trades correctness for safety:

1. Call `KillME()` to stop the ME core.
2. Null all decoder and buffer pointers without calling destructors.
3. Set `g_refs_corrupted = 1`.
4. **Do not call WelsDestroyDecoder** - accept the one-time memory leak (~64 KB on PSP). This is intentional: better than a crash during a force-restart.

Caller must subsequently call `oh264_pipeline_init()` to get a fresh context.

---

## Key Design Decisions

### DecodeFrameNoDelay vs DecodeFrame2

OpenH264 offers two decode APIs:
- `DecodeFrame2()` - may buffer one frame internally for B-frame reordering
- `DecodeFrameNoDelay()` - immediate output, no internal buffering

We use `DecodeFrameNoDelay()` because Moonlight streams are strictly I/P-frame only (no B-frames), and any internal buffering adds latency to the glass-to-glass pipeline. This saves one full frame period (~66ms at 15fps) of decode-to-display latency.

### ERROR_CON_SLICE_COPY vs Other Concealment Modes

OpenH264 supports several error concealment strategies:
- `ERROR_CON_DISABLE` - no concealment, frame dropped on any error
- `ERROR_CON_FRAME_COPY` - copy entire previous frame on error
- `ERROR_CON_SLICE_COPY` - copy only affected slices from previous frame
- `ERROR_CON_SLICE_MV_COPY_CROSS_IDR` - motion-vector-based concealment

We use `ERROR_CON_SLICE_COPY` because it provides the best balance:
- Faster than MV-based concealment (no motion search)
- Better visual quality than FRAME_COPY (only affected areas replaced)
- Stable output (never produces garbage pixels)

### Single-Threaded Decode (DISABLE_DECODER_MT)

The PSP has one user-mode CPU core. OpenH264's multi-threaded decode would create threads that compete for the same core, adding context-switch overhead with zero parallel benefit. Our PSP port is compiled with `DISABLE_DECODER_MT` and we set `DECODER_OPTION_NUM_OF_THREADS = 0` at runtime to ensure no threading code activates.

### Trace Logging Disabled

OpenH264's internal trace system uses `snprintf` and buffer formatting on every frame boundary event. On the PSP's 333 MHz MIPS core, this formatting costs ~200us per frame - significant when the total decode budget is 20-50ms. Disabled via `DECODER_OPTION_TRACE_LEVEL = 0` (WELS_LOG_QUIET).
