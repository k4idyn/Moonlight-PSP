# Intel Quick Sync (QSV) Encoder Settings Guide

## Overview

Intel Quick Sync Video (QSV) is the hardware video encoder built into Intel CPUs with integrated graphics (HD Graphics, Iris, Arc). It uses a dedicated media engine on the iGPU, so encoding happens with minimal CPU and GPU overhead.

QSV is available on:
- Intel Core 2nd gen (Sandy Bridge) and newer
- Intel Arc discrete GPUs
- Some Intel Xeon processors with integrated graphics

## Settings

### `qsv_preset` — Speed vs Quality

**Web UI:** Configuration → Video → Intel QuickSync Encoder → Preset
**Default:** `medium`
**Config key:** `qsv_preset`
**Valid values:** `veryfast`, `faster`, `fast`, `medium`, `slow`, `slower`, `veryslow`

| Preset | Speed | Quality |
|--------|-------|---------|
| `veryfast` | Fastest | Lowest |
| `faster` | Fast | Below average |
| `fast` | Above average | Average |
| `medium` | Average | Good |
| `slow` | Below average | Very good |
| `slower` | Slow | Excellent |
| `veryslow` | Slowest | Best |

**PSP recommendation:** `veryfast` or `faster`. At 368×208, quality differences between presets are negligible. Use the fastest preset for lowest encoding latency.

---

### `qsv_coder` — Entropy Coding (H.264 only)

**Web UI:** Configuration → Video → Intel QuickSync Encoder → Entropy Coder
**Default:** `auto`
**Config key:** `qsv_coder`

| Value | Compression | Decode Speed |
|-------|------------|-------------|
| `auto` | Let QSV decide | Varies |
| `cabac` | Better (~10-15% smaller) | Slower to decode |
| `cavlc` | Less efficient | Faster to decode |

**PSP recommendation:** `cavlc`. Same reasoning as AMD and NVIDIA — the PSP's CPU is slow, so CAVLC's simpler decoding is beneficial. CABAC is treated as unsupported for normal PSP playback, and the client will return to the menu if a host still delivers CABAC.

---

### `qsv_slow_hevc` — HEVC on Older Intel GPUs

**Web UI:** Configuration → Video → Intel QuickSync Encoder → Slow HEVC
**Default:** `disabled`
**Config key:** `qsv_slow_hevc`

Enables HEVC encoding on older Intel GPUs that support it but at reduced performance. Only relevant for 6th–9th gen Intel CPUs.

**PSP recommendation:** `disabled`. The PSP doesn't support HEVC — this setting is irrelevant.

---

## Recommended QSV Config for PSP

```ini
encoder = quicksync
qsv_preset = veryfast
qsv_coder = cavlc
qsv_slow_hevc = disabled
qp = 28
fec_percentage = 20
```

## QSV Quirks and Tips

### Integrated vs Discrete

- **Integrated graphics (HD/Iris):** QSV is available automatically. Your discrete GPU handles game rendering while the iGPU handles encoding — this is ideal because encoding doesn't compete with gaming.
- **Intel Arc (discrete):** QSV is available but competes with game rendering on the same GPU. Performance may vary.

### Intel Generation Differences

| Generation | QSV Quality | Notes |
|-----------|-------------|-------|
| 2nd–5th gen | Basic | Functional but lower quality |
| 6th–8th gen | Good | Improved rate control |
| 9th–11th gen | Very good | AV1 support on 11th gen+ |
| 12th gen+ | Excellent | Hardware AV1, improved quality |
| Arc | Best | Dedicated media engine |

For PSP streaming, even the oldest QSV implementation is fine because the resolution is so low.

### Common Issues

1. **"QSV not available" error:** Make sure integrated graphics is enabled in BIOS. Some motherboards disable iGPU when a discrete GPU is installed.
2. **Poor quality at low bitrates:** Try `qsv_preset = medium` instead of `veryfast`. QSV's fastest presets can produce visible blocking at 384 kbps.
3. **Linux QSV:** Requires `intel-media-va-driver` or `intel-media-va-driver-non-free` package. Use VA-API (`encoder = vaapi`) instead if QSV gives trouble.
