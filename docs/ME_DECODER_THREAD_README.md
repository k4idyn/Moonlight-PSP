# Media Engine (ME) in PSP Moonlight

## What the ME Does

The PSP has two processors: the main Allegrex CPU at 333 MHz and the Media Engine (ME) at 222 MHz. In PSP Moonlight, they divide work as follows:

- **Main CPU**: FFmpeg H.264 decode, RTP reassembly, FEC repair, audio decode, input, UI, network receive
- **ME**: YUV420P → RGBA8888 color conversion using VFPU vector instructions

The ME does **not** run H.264 decode. The original goal was to use `sceMpeg` on the ME for hardware-assisted AVC decode, but that approach was abandoned — see the Historical Context section below.

---

## Why the ME for Color Conversion

After FFmpeg produces a decoded `AVFrame` (YUV420P planar), the PSP's GU/display system needs RGB or RGBA data. The conversion math — three multiply-accumulate operations per pixel at 480×272 = ~130,000 pixels per frame — is ideal for the VFPU's 128-bit vector lanes.

Critically, the ME runs **concurrently** with the main CPU. The pipeline is:

```
Main CPU                              ME (222 MHz)
─────────────────────────             ─────────────────────────
Frame N: avcodec_send_packet()
Frame N: avcodec_receive_frame()
         → YUV420P AVFrame
Frame N: BeginME(yuv→rgba entry) ──▶  Frame N: VFPU YUV→RGBA (~31 µs)
Frame N+1: avcodec_send_packet()
Frame N+1: avcodec_receive_frame()    [ME finishes Frame N]
```

Throughput = `max(FFmpeg_time, ME_convert_time)` rather than their sum.

---

## ME Worker API

**Source file:** `src/sw_me_worker.c`

Three functions manage the ME job lifecycle:

```c
// Dispatch a job to the ME (async — returns immediately)
void sw_me_worker_dispatch_yuv(YuvFrame *yuv_in, RgbaFrame *rgba_out);

// Block until the last dispatched job completes
void sw_me_worker_wait(void);

// Check if the ME is idle (non-blocking)
int sw_me_worker_check(void);  // 1 = idle, 0 = busy
```

Internally, `BeginME()` is called with the entry point `yuv420_to_rgba_entry` (defined in `moonlight_me_helper.prx`), which is a VFPU assembly routine loaded as a PRX module at init time.

### Cache Safety

Before `BeginME()`, the source YUV buffers must be flushed from D-cache:
```c
sceKernelDcacheWritebackInvalidateRange(yuv_in->y, y_size);
sceKernelDcacheWritebackInvalidateRange(yuv_in->cb, c_size);
sceKernelDcacheWritebackInvalidateRange(yuv_in->cr, c_size);
```

After `WaitME()`, the RGBA output buffer must be invalidated before the main CPU reads it:
```c
sceKernelDcacheInvalidateRange(rgba_out->data, rgba_size);
```

Skipping these is the most common cause of corrupted or stale frame output.

### Crash Recovery

If the ME hangs or `WaitME()` times out, `sw_me_worker.c` calls `KillME()`, re-initializes the ME context, and retries the job once. If the second attempt also fails, the frame is dropped and the pipeline continues. Persistent ME failures are logged to `moonlight_debug.log`.

---

## Initialization

Called from `ffmpeg_pipeline_init()` in `src/ffmpeg_decode.c`:

```c
sw_me_worker_init();
// Loads moonlight_me_helper.prx via sceKernelLoadModule
// Calls InitME(me_instance) to bind the VFPU entry point
// Verifies the PRX loaded cleanly — logs error and falls back if not
```

The PRX `moonlight_me_helper.prx` must exist at `ms0:/PSP/GAME/Moonlight/moonlight_me_helper.prx` at runtime. It is shipped alongside `EBOOT.PBP`.

---

## Teardown

Called from `ffmpeg_pipeline_shutdown()`:

```c
sw_me_worker_wait();     // drain any in-flight job
sw_me_worker_deinit();   // call FinishME, unload PRX
```

---

## Thread Priorities

| Thread | Priority | Role |
|--------|----------|------|
| Main / UI | 0x20 | FFmpeg decode, input, HUD |
| Network receive | 0x15 | UDP socket drain |
| Audio output | 0x12 | Opus decode + sceAudio |
| ME VFPU job | (ME hardware) | YUV→RGBA, runs on separate core |

The ME runs on a physically separate core — it does not compete with PSP thread priorities in the standard scheduler.

---

## Historical Context: Why Not sceMpeg

The original plan was to use `sceMpegAvcDecode` on the ME to decode H.264 directly in hardware. This was the approach used in the prior public release (`k4idyn/Moonlight-PSP` v0.1.0.1–v0.1.0.3-alpha).

`sceMpeg` was designed for MPEG-4/H.264 playback from managed ringbuffers. The hardware expects a continuous, properly-framed bitstream fed through `sceMpegRingbufferPut`. The Moonlight protocol delivers RTP-fragmented H.264 NAL units out of order with FEC repair packets interleaved. Mapping this stream model onto the `sceMpeg` ringbuffer contract cleanly is not feasible — the hardware stalls or returns error `0x80628002` on any gap or ordering issue in the input.

The software FFmpeg path handles RTP-fragmented input naturally, at the cost of running on the main CPU. The ME is then left free for the color conversion step, which it handles without the strict sequencing requirements that break `sceMpeg`.

For full decode pipeline documentation, see `docs/DECODER_PIPELINE.md`.
