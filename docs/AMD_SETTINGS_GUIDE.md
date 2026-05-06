# AMD AMF Encoder Settings Guide

## Overview

AMD AMF (Advanced Media Framework) is the hardware video encoder in AMD Radeon GPUs (RX 400 series and newer). It uses the Video Core Engine (VCE) for H.264/HEVC encoding with minimal CPU overhead. For PSP streaming, AMF works well when configured correctly — our A/B testing showed some default settings cause failures while others deliver excellent performance.

> **Sunshine vs Apollo:** All AMD encoder settings are identical between Sunshine and Apollo (ClassicOldSong fork). The encoding pipeline is shared code.

## Settings

### 1. `encoder` — Which Encoder to Use

**Web UI:** Configuration → Video → Encoder
**Default:** auto-detect
**Config key:** `encoder`

Forces the server to use AMD's VCE hardware encoder. Set this explicitly if your PC has both an AMD and NVIDIA GPU. Otherwise leave on auto.

---

### 2. `amd_usage` — Usage Profile

**Web UI:** Configuration → Video → AMD AMF → Usage
**Default:** `ultralowlatency`
**Config key:** `amd_usage`
**Valid values:** `ultralowlatency`, `lowlatency`, `lowlatency_high_quality`, `webcam`, `transcoding`

This is the most impactful setting. It sets a base "template" of hidden internal encoder parameters.

| Value | Speed | Quality | Latency | Notes |
|-------|-------|---------|---------|-------|
| `ultralowlatency` | Fastest | Lowest | Lowest | Best for twitch gaming |
| `lowlatency` | Fast | Low-Medium | Low | Good balance |
| `lowlatency_high_quality` | Medium | High | Medium | Best visual quality while still streaming |
| `webcam` | Slow | Medium | High | Not useful for game streaming |
| `transcoding` | Slowest | Highest | Highest | Not useful for real-time streaming |

**PSP recommendation:** `ultralowlatency` or `lowlatency`. Our A/B testing showed `lowlatency_high_quality` actually delivered the highest FPS (38 fps peak) — likely because it produces cleaner frames that decode faster on the PSP.

---

### 3. `amd_rc` — Rate Control Mode

**Web UI:** Configuration → Video → AMD AMF → Rate Control
**Default:** `vbr_latency`
**Config key:** `amd_rc`
**Valid values:** `vbr_latency`, `cbr`, `vbr_peak`, `cqp`

Controls how the encoder manages bitrate (how many bits each frame gets).

| Value | What It Does | Good For |
|-------|-------------|----------|
| `vbr_latency` | Targets your bitrate but can flex up/down for complex scenes | General use — **recommended** |
| `cbr` | Strict constant bitrate — every second uses exactly the same bits | Bandwidth-limited networks |
| `vbr_peak` | Variable bitrate with a hard ceiling — never exceeds max | Preventing spikes |
| `cqp` | Ignores bitrate entirely — uses fixed quality level (QP) | **Avoid for PSP** — can exceed bandwidth |

**PSP recommendation:** `vbr_latency`. Our testing showed `cqp` mode caused complete streaming failure because it doesn't respect the 384 kbps bitrate limit.

---

### 4. `amd_quality` — Quality Preset

**Web UI:** Configuration → Video → AMD AMF → Quality Preset
**Default:** `balanced`
**Config key:** `amd_quality`
**Valid values:** `speed`, `balanced`, `quality`

| Value | Effect |
|-------|--------|
| `speed` | Encode as fast as possible, accept lower quality |
| `balanced` | Middle ground |
| `quality` | Spend more time encoding for better-looking frames |

**PSP recommendation:** `balanced` or `speed`. At 368×208 resolution, the quality difference is hard to see on the PSP's small screen.

---

### 5. `amd_coder` — Entropy Coding (H.264 only)

**Web UI:** Configuration → Video → AMD AMF → Entropy Coding
**Default:** `auto`
**Config key:** `amd_coder`
**Valid values:** `auto`, `cavlc`, `cabac`

This only affects H.264 streams (not HEVC or AV1). It controls how the compressed video data is packed.

| Value | Compression | Decode Speed | Notes |
|-------|------------|-------------|-------|
| `auto` | Let encoder choose | Varies | Safest option |
| `cavlc` | Less efficient | Faster to decode | Better for weak decoders |
| `cabac` | More efficient (~10-15% smaller) | Slower to decode | Better quality per bit |

**PSP recommendation:** `cavlc`. The PSP's CPU is very slow — CAVLC is simpler to decode. CABAC is treated as unsupported for normal PSP playback, and the client will return to the menu if the host still delivers CABAC.

---

### 6. `amd_enforce_hrd` — HRD Enforcement

**Web UI:** Configuration → Video → AMD AMF → Enforce HRD
**Default:** `disabled`
**Config key:** `amd_enforce_hrd`
**Valid values:** `enabled`, `disabled`

HRD (Hypothetical Reference Decoder) forces the encoder to produce a bitstream that any standard-compliant decoder can handle without buffer overflow. In practice, it makes bitrate control stricter.

**PSP recommendation:** `disabled`. Enabling HRD can cause visual artifacts on some AMD cards. Our testing showed it works but adds no benefit at PSP bitrates.

---

### 7. `amd_preanalysis` — Pre-Analysis Pass

**Web UI:** Configuration → Video → AMD AMF → Pre-Analysis
**Default:** `disabled`
**Config key:** `amd_preanalysis`
**Valid values:** `enabled`, `disabled`

When enabled, the encoder scans each frame before encoding it to make smarter compression decisions. This adds ~1 frame of latency.

**PSP recommendation:** `disabled`. The PSP is already at 15 fps — adding latency is not worth the marginal quality improvement.

---

### 8. `amd_vbaq` — Variance-Based Adaptive Quantization

**Web UI:** Configuration → Video → AMD AMF → VBAQ
**Default:** `enabled`
**Config key:** `amd_vbaq`
**Valid values:** `enabled`, `disabled`

VBAQ allocates more bits to smooth/flat areas (where blocking artifacts are most visible) and fewer bits to busy/textured areas (where artifacts are hidden by detail).

**PSP recommendation:** `enabled`. Particularly helpful at low bitrates like 384 kbps. No downside.

---

### 9. `qp` — Quantization Parameter

**Web UI:** Configuration → Video → QP
**Default:** `28`
**Config key:** `qp`
**Valid range:** 0–51

Only used when `amd_rc = cqp`. Higher values = more compression = lower quality but smaller bitstream. Lower values = less compression = better quality but bigger bitstream.

| Range | Quality Level |
|-------|--------------|
| 0–18 | Visually lossless (huge bitrate) |
| 18–28 | High quality |
| 28–35 | Medium quality |
| 35–51 | Low quality / high compression |

**PSP recommendation:** Not applicable — use `vbr_latency` instead of `cqp`. If you must use CQP, try 28–32.

---

### 10. `fec_percentage` — Forward Error Correction

**Web UI:** Configuration → Network → FEC Percentage
**Default:** `20`
**Config key:** `fec_percentage`
**Valid range:** 1–255

FEC adds redundant data to the stream so the PSP can recover from lost Wi-Fi packets without requesting a full new frame (IDR). Higher values = more resilience but more bandwidth used for redundancy.

| Value | Use Case |
|-------|----------|
| 10–15 | Excellent Wi-Fi, minimal packet loss |
| 20 | Default — good for most setups |
| 30–50 | Poor Wi-Fi, frequent packet loss |
| 50+ | Very unreliable network — but wastes bandwidth |

**PSP recommendation:** `20–30`. Our testing showed FEC 20% worked well. Going to 50% is conservative but safe. Below 20% caused poor performance in testing.

---

### 11. `hevc_mode` — HEVC/H.265 Advertisement

**Web UI:** Configuration → Video → HEVC Mode
**Default:** `0` (auto)
**Config key:** `hevc_mode`
**Valid values:** `0` (auto), `1` (never advertise), `2` (always 8-bit), `3` (always 10-bit/HDR)

Controls whether the server tells clients it supports HEVC.

**PSP recommendation:** `0` or `1`. The PSP does not support HEVC — this setting doesn't affect PSP streaming either way.

---

### 12. `av1_mode` — AV1 Advertisement

**Web UI:** Configuration → Video → AV1 Mode
**Default:** `0` (auto)
**Config key:** `av1_mode`
**Valid values:** `0` (auto), `1` (never), `2` (always 8-bit), `3` (always 10-bit)

Same as `hevc_mode` but for AV1 codec.

**PSP recommendation:** `0` or `1`. The PSP does not support AV1.

---

### 13. `headless_mode` — Virtual Display

**Web UI:** Configuration → Video → Headless Mode
**Default:** `disabled`
**Config key:** `headless_mode`
**Valid values:** `enabled`, `disabled`

Allows streaming without a physical monitor connected. Useful for dedicated streaming servers.

**PSP recommendation:** `disabled` for normal use. Enable only if your PC has no monitor attached. Note: first connection with headless mode may show incorrect codec capabilities until reconnection.

---

## A/B Test Results Summary

We tested 14 encoder combinations on PSP at 368×208@15 fps, 384 kbps for 60 seconds each:

| Rank | Configuration | Peak FPS | Decode Latency | Stability | Notes |
|------|--------------|----------|---------------|-----------|-------|
| 1 | LowLat HQ + Speed + VBR | 38.2 | 25.9 ms | Excellent | **Best overall** |
| 2 | Quality preset | 28.1 | 35.2 ms | Excellent | Best visual quality |
| 3 | QP 22 | 16.1 | 33.2 ms | Good | Strong fallback |
| 4 | VBAQ Off | 20.0 | 27.7 ms | Good | Alternative |
| 5 | HRD Enforced | 12.7 | 35.6 ms | Fair | Unnecessary constraint |
| — | Baseline (current) | — | — | **Failed** | Decode errors |
| — | CQP mode | — | — | **Failed** | Ignores bitrate limit |
| — | CBR+Balanced+HRD | — | — | **Failed** | Over-constrained |

### Recommended Apollo/Sunshine Config for PSP

```ini
encoder = amdvce
amd_usage = lowlatency_high_quality
amd_quality = speed
amd_rc = vbr_latency
amd_coder = cavlc
amd_enforce_hrd = disabled
amd_preanalysis = disabled
amd_vbaq = enabled
qp = 28
fec_percentage = 20
hevc_mode = 0
av1_mode = 0
```

---

