# NVIDIA NVENC Encoder Settings Guide

## Overview

NVENC is NVIDIA's hardware video encoder built into GeForce GTX 600+ and Quadro/Tesla GPUs. It encodes video with near-zero CPU overhead. For PSP streaming, NVENC is excellent because it's fast and produces clean H.264 output.

## Settings

### `nvenc_preset` — Performance Preset

**Web UI:** Configuration → Video → NVIDIA NVENC Encoder → Preset
**Default:** `1` (P1 — fastest)
**Config key:** `nvenc_preset`
**Range:** 1–7

| Preset | Speed | Quality | Notes |
|--------|-------|---------|-------|
| P1 | Fastest | Lowest | Best for low-latency streaming |
| P2 | Very fast | Low | Slight quality bump |
| P3 | Fast | Below average | Good balance |
| P4 | Medium | Average | Middle ground |
| P5 | Below average | Good | Quality-focused |
| P6 | Slow | Very good | Near-maximum quality |
| P7 | Slowest | Best | Maximum quality, highest latency |

**PSP recommendation:** `1` (P1). At 368×208, the quality differences between presets are invisible. P1 gives the lowest encoding latency.

---

### `nvenc_twopass` — Two-Pass Encoding

**Web UI:** Configuration → Video → NVIDIA NVENC Encoder → Two-Pass Mode
**Default:** `quarter_res`
**Config key:** `nvenc_twopass`

| Value | What It Does |
|-------|-------------|
| `disabled` | Single pass — fastest, least accurate bitrate |
| `quarter_res` | Quick pre-pass at 1/4 resolution — good balance |
| `full_res` | Full pre-pass at full resolution — best bitrate accuracy, slower |

**PSP recommendation:** `quarter_res` or `disabled`. The two-pass mode helps the encoder distribute bits more evenly across the frame, but at 368×208 the difference is minimal.

---

### `nvenc_spatial_aq` — Spatial Adaptive Quantisation

**Web UI:** Configuration → Video → NVIDIA NVENC Encoder → Spatial AQ
**Default:** `disabled`
**Config key:** `nvenc_spatial_aq`

Similar to AMD's VBAQ — allocates more bits to flat/smooth areas where blocking artifacts are most visible. Costs some GPU performance.

**PSP recommendation:** `disabled`. At 384 kbps there aren't enough bits for AQ to redistribute meaningfully.

---

### `nvenc_vbv_increase` — VBV Buffer Increase

**Web UI:** Configuration → Video → NVIDIA NVENC Encoder → VBV Increase
**Default:** `0`
**Config key:** `nvenc_vbv_increase`
**Range:** 0–400

Increases the Video Buffering Verifier (VBV) buffer size as a percentage. Higher values allow single frames to use more than their "fair share" of bitrate, improving quality during complex scenes but risking packet loss.

| Value | Effect |
|-------|--------|
| 0 | Strict — each frame gets exactly its share |
| 100 | Allow 2× bitrate for complex frames |
| 200 | Allow 3× bitrate |
| 400 | Allow 5× bitrate (risky for PSP) |

**PSP recommendation:** `0`. The PSP's Wi-Fi can't handle sudden bitrate spikes. Keep VBV strict.

---

### `nvenc_realtime_hags` — Realtime HAGS Priority

**Web UI:** Configuration → Video → NVIDIA NVENC Encoder → Realtime HAGS
**Default:** `enabled`
**Config key:** `nvenc_realtime_hags`
**Windows only**

Uses Windows Hardware Accelerated GPU Scheduling to give the encoder realtime priority. Helps reduce encoding latency jitter.

**PSP recommendation:** `enabled`. No reason to disable.

---

### `nvenc_latency_over_power` — Low Latency Power Mode

**Web UI:** Configuration → Video → NVIDIA NVENC Encoder → Latency Over Power
**Default:** `enabled`
**Config key:** `nvenc_latency_over_power`
**Windows only**

Requests the NVIDIA driver to keep the GPU in a high-power state for faster encoding response. Disabling this can increase encoding latency significantly.

**PSP recommendation:** `enabled`. Do not disable — the latency increase is not worth the power savings.

---

### `nvenc_h264_cavlc` — Force CAVLC for H.264

**Web UI:** Configuration → Video → NVIDIA NVENC Encoder → CAVLC
**Default:** `disabled` (uses CABAC)
**Config key:** `nvenc_h264_cavlc`

Forces CAVLC entropy coding instead of CABAC. CAVLC is ~10% less efficient (bigger frames) but faster to decode.

**PSP recommendation:** `enabled`. The PSP's CPU is very slow — CAVLC reduces decode load. The 10% efficiency loss is acceptable at PSP resolution. CABAC is treated as unsupported for normal PSP playback, and the client will return to the menu if the host still delivers CABAC.

---

### `nvenc_opengl_vulkan_on_dxgi` — OpenGL/Vulkan on DXGI

**Web UI:** Configuration → Video → NVIDIA NVENC Encoder → OpenGL/Vulkan on DXGI
**Default:** `enabled`
**Config key:** `nvenc_opengl_vulkan_on_dxgi`
**Windows only**

Presents OpenGL and Vulkan frames through the DXGI capture pipeline. Improves compatibility but may add a small amount of latency for non-DirectX games.

**PSP recommendation:** `enabled`. Leave default for best compatibility.

---

## Recommended NVENC Config for PSP

```ini
encoder = nvenc
nvenc_preset = 1
nvenc_twopass = quarter_res
nvenc_spatial_aq = disabled
nvenc_vbv_increase = 0
nvenc_realtime_hags = enabled
nvenc_latency_over_power = enabled
nvenc_h264_cavlc = enabled
nvenc_opengl_vulkan_on_dxgi = enabled
qp = 28
fec_percentage = 20
```

## Notes

- NVENC is generally the best encoder for game streaming due to very low latency
- GTX 1650+ and RTX cards have improved NVENC quality
- The PSP requests H.264 only — HEVC/AV1 settings don't matter
- NVENC can handle multiple simultaneous streams without performance impact
