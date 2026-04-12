# FFmpeg + VFPU Dual-Core Decode Pipeline

_PSP Moonlight v0.2.0-beta_

This document describes the active H.264 decode pipeline used in PSP Moonlight. The prior
`sceMpeg`-based approach (see `docs/ME_DECODER_THREAD_README.md` for historical context) was
abandoned because the `sceMpeg` ringbuffer model is incompatible with the Moonlight RTP stream
model. Everything in this document describes the current software pipeline.

---

## Overview

```
Network              Main CPU (333 MHz)                Media Engine (222 MHz)
────────         ─────────────────────────────         ─────────────────────
UDP socket
    │
    ▼
rtp_reassembly   ← RTP header strip + frame boundary
    │
    ▼
rtp_fec          ← Reed-Solomon FEC repair (up to 66% parity)
    │
    ▼
sw_decoder_thread ← frame queued to decode
    │
    ▼
ffmpeg_pipeline_decode_frame()
    │
    ├──[avcodec_send_packet()]──────────────────────┐
    │                                               │
    ├──[avcodec_receive_frame()]                    │ FFmpeg H.264
    │       AVFrame{ data[0]=Y, [1]=Cb, [2]=Cr }   │ Baseline, CAVLC
    │                                               │
    ▼                                               │
sw_me_worker_dispatch_yuv()                        ─┘
    │
    └──[BeginME(yuv420_to_rgba_entry)]──────────────▶ sw_vfpu_recon.c
                                                       VFPU YUV→RGBA
                                                       ~31 µs/frame
                                                       (async, concurrent
                                                        with next decode)
```

Throughput = `max(FFmpeg_decode_ms, ME_convert_ms)` — not their sum.

---

## Pipeline Lifecycle

### Init — `ffmpeg_pipeline_init()`

Located in `src/ffmpeg_decode.c:411`.

1. `avcodec_find_decoder(AV_CODEC_ID_H264)` — locate H.264 codec
2. `avcodec_alloc_context3()` — allocate codec context
3. Configure: `thread_count=1`, `flags |= AV_CODEC_FLAG_LOW_DELAY`, `skip_loop_filter=AVDISCARD_ALL`
4. `avcodec_open2()` — open decoder
5. `av_frame_alloc()` — allocate output AVFrame
6. `sw_me_worker_init()` — load `moonlight_me_helper.prx`, call `InitME(me_instance)`
7. Clear `g_refs_corrupted`, `g_watchdog_restart`, all counters
8. Return 0 on success, negative on failure

### Per-Frame Decode — `ffmpeg_pipeline_decode_frame()`

Located in `src/ffmpeg_decode.c:729`.

**Entry conditions:**
- `nal_data` — pointer to Annex-B access unit (starts with 0x00000001)
- `nal_len` — byte count
- Called from `sw_decoder_thread.c` decode loop

**Step-by-step:**

1. **Watchdog restart check** — if `g_watchdog_restart != 0`, reset stale static counters and clear the flag. This prevents misleading statistics after a pipeline reinit.

2. **Early-skip gate** (`g_refs_corrupted` path):
   - If `g_refs_corrupted == 1`, inspect the incoming NAL type.
   - If it is NOT an IDR slice (NAL type 5), **drop the frame immediately** and return `-6`. This avoids feeding P-frames with broken DPB references into FFmpeg — those would produce mosaic artifacts that persist across subsequent IDR boundaries. No CPU wasted on entropy decode.
   - If it IS an IDR, allow it through. A clean IDR decode will reset `g_refs_corrupted` to 0 on success.

3. **avcodec_send_packet()** — wrap `nal_data` in an `AVPacket`, submit to FFmpeg.
   - On `AVERROR(EAGAIN)`: drain with `avcodec_receive_frame()` first, then retry.
   - On other errors: set `g_refs_corrupted = 1`, return error code.

4. **avcodec_receive_frame()** — retrieve decoded `AVFrame` (YUV420P).
   - On `AVERROR(EAGAIN)`: buffering frame (normal for B-frame reorder, rare at Baseline). Return 0 to caller; no output this call.
   - On `AVERROR_EOF`: codec drained cleanly.
   - On other errors: set `g_refs_corrupted = 1`.
   - **On success**: validate `frame->format == AV_PIX_FMT_YUV420P`, validate `frame->width == 480`, `frame->height == 272`. Mismatches set `g_refs_corrupted`.

5. **ME dispatch** — `sw_me_worker_dispatch_yuv(frame)`:
   - Copy Y/Cb/Cr pointers and strides to ME job struct (cached → uncached alias for ME safety).
   - `BeginME(me_instance, yuv420_to_rgba_entry, ...)` — non-blocking. ME core starts VFPU conversion immediately.
   - Main CPU **does not** call `WaitME()` here — the wait is deferred to the start of the *next* `dispatch_yuv()` call (`WaitME()` at line 280 of `sw_me_worker.c`).

6. **Output** — set `*out_rgba = me_rgb_buffer` (pointer to the output from the *previous* ME job, which is now complete by the time this function returns, because `WaitME()` was called at the top of step 5). This is the double-buffer hand-off.

7. **Reference corruption check** — on a successful decode, if `g_refs_corrupted` was 1, clear it. The clean IDR decoded fully, DPB is rebuilt, stream is safe again.

### Flush — `ffmpeg_pipeline_flush_buffers()`

Located in `src/ffmpeg_decode.c:1499`.

Called when a queue overrun is detected (ring buffer fell behind by more than 3 frames). Steps:
1. `avcodec_flush_buffers()` — discard all FFmpeg internal state, DPB, pending packets.
2. Set `g_refs_corrupted = 1` — next non-IDR frames will be skipped until a fresh IDR arrives.
3. Call `control_stream_request_idr()` — send RFI to Sunshine host.

### Invalidate Refs — `ffmpeg_pipeline_invalidate_refs()`

Located in `src/ffmpeg_decode.c:1474`.

Lightweight variant: only sets `g_refs_corrupted = 1` without flushing FFmpeg state. Used when FEC repair fails (RS erasure limit exceeded) — the frame is corrupt but the codec may be able to limp forward.

### Abandon — `ffmpeg_pipeline_abandon()`

Located in `src/ffmpeg_decode.c:680`.

**Emergency path** — called after `sceKernelTerminateThread()` on the decoder thread. Because FFmpeg internally holds mutexes in some allocation paths, `avcodec_close()` / `avcodec_free_context()` may deadlock if called while the thread held a lock. `_abandon()` trades correctness for safety:

1. Zero the codec context pointer without calling `avcodec_close()`.
2. Call `av_frame_free()` only if the frame is not marked as a reference frame (safe).
3. Call `sw_me_worker_shutdown()` with a kill-then-ignore-error policy.
4. Set `g_refs_corrupted = 1`.
5. **Do not free AVCodecContext** — accept the one-time memory leak (~32 KB on PSP). This is intentional: better than a deadlock during a force-restart.

Caller must subsequently call `ffmpeg_pipeline_init()` to get a fresh context.

### Shutdown — `ffmpeg_pipeline_shutdown()`

Located in `src/ffmpeg_decode.c:634`.

Clean teardown path (cooperative exit):
1. `WaitME(me_instance)` — wait for any in-flight ME job to complete.
2. `KillME(me_instance)` — signal ME core to exit.
3. `avcodec_send_packet(NULL)` — drain flush.
4. `avcodec_flush_buffers()`.
5. `avcodec_close()` + `avcodec_free_context()`.
6. `av_frame_free()`.
7. Unload `moonlight_me_helper.prx`.

---

## Media Engine YUV→RGBA Path

### Entry Point

`yuv420_to_rgba_entry()` in `src/sw_vfpu_recon.c` — this is the function pointer passed to `BeginME()`.

### VFPU Algorithm

1. Load 8 Y samples into VFPU register `vf0` via `lv.q`.
2. Load 8 Cb (U) samples → `vf1`.
3. Load 8 Cr (V) samples → `vf2`.
4. Subtract 128 from Cb, Cr (`vsub`).
5. BT.601 matrix multiply via VFPU `vmmul`:
   ```
   R = Y + 1.402 * (Cr - 128)
   G = Y - 0.344 * (Cb - 128) - 0.714 * (Cr - 128)
   B = Y + 1.772 * (Cb - 128)
   ```
6. Clamp to [0, 255] via `vsat0` + `vsat1` (VFPU saturation pair).
7. Pack to RGBA8888 (`vf.A = 0xFF`) and store via `sv.q`.
8. Advance pointers: luma advances 8 pixels per iteration, chroma 4 (4:2:0).

### Memory Layout Requirements

- YUV planes must be in **uncached RAM** before `BeginME()` is called. The ME cannot access the main CPU's cache. `ffmpeg_decode.c` flushes cache lines for the output AVFrame buffers.
- RGBA output buffer is also in uncached RAM (allocated at startup in `main.c`).
- **Never** pass a cached pointer to `BeginME()` — this is the hardest PSP ME bug to diagnose and the root cause of 1500× performance regression that was fixed. See `psp-me-pipeline-fix.md` in repo memory.

### Timing

At 333/222 MHz clocks:
- `BeginME()` call overhead: ~2 µs
- YUV→RGBA for 480×272 (130,560 pixels): ~31 µs
- `WaitME()` timeout guard: 100 ms (after which ME is killed and reinited)

---

## Dual-Mode Watchdog

The watchdog is split across `sw_decoder_thread.c` (timestamps) and `main.c` (monitor loop).

### Mode A — FFmpeg Hang

**Trigger:** `g_decode_active_us` was set (decode in progress) but no update for > `DECODE_HANG_TIMEOUT_US` (500 ms).

**Cause:** FFmpeg entered an infinite loop on a corrupt NAL, or a MIPS exception inside libavcodec was swallowed.

**Recovery:**
1. `sceKernelTerminateThread(g_decoder_thread_id)`
2. `ffmpeg_pipeline_abandon()` — emergency cleanup
3. `sceKernelDeleteThread()`
4. `ffmpeg_pipeline_init()` — fresh context
5. `sceKernelCreateThread()` + `sceKernelStartThread()` — new decoder thread
6. `control_stream_request_idr()` — ask host for IDR

### Mode B — RTP Stall

**Trigger:** `g_last_frame_received_us` hasn't advanced for > `RTP_STALL_TIMEOUT_US` (3 s), AND `g_decode_active_us == 0` (decoder not stuck — it's idle because no frames arrive).

**Cause:** FEC repair is dropping entire frames, or the network receive thread stalled.

**Recovery:**
1. `control_stream_request_idr()` only (no thread restart).
2. If stall persists >10 s with repeated IDR requests: full stream reconnect.

### Distinguishing Mode A from Mode B

The key differentiator is `g_decode_active_us`:
- Mode A: `g_decode_active_us != 0` (decode started, never finished)
- Mode B: `g_decode_active_us == 0` (decoder idle due to starvation)

The zero-artifact early-skip policy was initially causing false Mode B triggers (the decoder was skipping frames during `g_refs_corrupted` and looked idle). The fix: `sw_decoder_thread.c` updates `g_last_frame_received_us` every time a frame arrives *at the thread*, regardless of whether it is accepted by the early-skip gate.

---

## Key Global State

| Symbol | Location | Meaning |
|---|---|---|
| `g_refs_corrupted` | `ffmpeg_decode.c:72` | DPB is invalidated; skip non-IDR frames |
| `g_watchdog_restart` | `ffmpeg_decode.c` | Pipeline was just reinited; reset stale counters |
| `g_decode_active_us` | `sw_decoder_thread.c:85` | Timestamp when current decode started (0 = idle) |
| `g_last_frame_received_us` | `sw_decoder_thread.c` | Timestamp of last frame received at thread |
| `g_idr_fully_decoded` | `ffmpeg_decode.c` | First IDR complete; safe to display frames |
| `g_saw_first_idr` | `ffmpeg_decode.c` | At least one IDR has been seen in this session |
| `me_instance` | `sw_me_worker.c` | ME kernel module instance handle |

---

## IDR / RFI Policy

See `src/rtp_fec.c` for FEC-layer IDR triggering.

- **RFI (Request for Intra)** — preferred. Sunshine sends a partial IDR (intra-coded macroblocks at the affected region only). Faster encoder response, less bandwidth.
- **Full IDR** — requested when `g_refs_corrupted == 1` after a clean flush. Resets the entire DPB. Used after Mode A watchdog recovery and `ffmpeg_pipeline_flush_buffers()`.

Cadence limit: IDR requests are rate-limited to once per 30 consecutive failures (`control_stream.c`) to avoid flooding the host encoder with back-to-back IDR demands.

---

## Source Files Quick Reference

| File | Role |
|---|---|
| `src/ffmpeg_decode.c` | Pipeline init/decode/flush/abandon, early-skip, ME dispatch |
| `src/sw_decoder_thread.c` | Decoder thread entry, watchdog timestamps, frame queuing |
| `src/sw_me_worker.c` | `BeginME`/`WaitME`/`KillME`/`InitME` wrappers, ME hang recovery |
| `src/sw_vfpu_recon.c` | VFPU IDCT, MotComp, YUV→RGBA conversion (runs on ME) |
| `src/sw_cavlc.c` | Hand-rolled CAVLC entropy decoder (legacy path, superseded by FFmpeg) |
| `src/sw_decode_orchestrator.c` | CPU↔ME handoff state machine |
| `src/rtp_fec.c` | Reed-Solomon FEC, IDR/RFI request policy |
| `src/rtp_reassembly.c` | RTP depacketisation, frame boundary detection |
| `include/decode_flags.h` | Shared decode state declarations (`g_refs_corrupted`, etc.) |
| `include/sw_decode_pipeline.h` | Pipeline API (`ffmpeg_pipeline_*` prototypes) |
| `moonlight_me_helper/` | Kernel PRX bootstrapping ME core; exports `InitME`/`KillME` |
