# Software Encoding Settings Guide

## When You Need Software Encoding

Software encoding uses your PC's CPU (via x264/libx264) instead of GPU hardware. You'd use this when:

- Your GPU doesn't have a hardware encoder
- Your GPU encoder is already busy (e.g., recording + streaming)
- You want maximum compatibility
- You're running on a headless server with no GPU

Software encoding is **significantly slower** than hardware encoding. For PSP streaming at 368×208@15 fps, it's usually fine because the resolution is so low.

## Settings

### `sw_preset` — Speed vs Quality

**Web UI:** Configuration → Video → Software Encoder → Preset
**Default:** `superfast`
**Config key:** `sw_preset`
**Valid values:** `ultrafast`, `superfast`, `veryfast`, `faster`, `fast`, `medium`, `slow`, `slower`, `veryslow`

| Preset | Speed | Quality | CPU Usage |
|--------|-------|---------|-----------|
| `ultrafast` | Fastest | Lowest | ~5% |
| `superfast` | Very fast | Low | ~8% |
| `veryfast` | Fast | Below average | ~12% |
| `faster` | Above average | Average | ~18% |
| `fast` | Average | Above average | ~25% |
| `medium` | Below average | Good | ~35% |
| `slow` | Slow | Very good | ~50% |
| `slower` | Very slow | Excellent | ~70% |
| `veryslow` | Slowest | Best | ~90% |

**PSP recommendation:** `superfast` or `veryfast`. At 368×208, even `superfast` looks decent. Don't go slower than `fast` — the added quality is invisible on the PSP screen and the CPU cost isn't worth it.

---

### `sw_tune` — Content Type Optimisation

**Web UI:** Configuration → Video → Software Encoder → Tune
**Default:** `zerolatency`
**Config key:** `sw_tune`
**Valid values:** `zerolatency`, `fastdecode`, `film`, `animation`, `grain`, `stillimage`

| Tune | Best For | What It Does |
|------|----------|-------------|
| `zerolatency` | **Game streaming** | Disables frame buffering — frames are sent immediately. Essential for interactive use. |
| `fastdecode` | Weak decoders | Makes the stream easier to decode. Good for PSP. |
| `film` | Movie content | Optimises for high-quality video with natural grain |
| `animation` | Cartoons/anime | Optimises for flat colours and sharp edges |
| `grain` | Grainy content | Preserves film grain instead of smoothing it |
| `stillimage` | Static content | Optimises for images that don't change much |

**PSP recommendation:** `zerolatency`. This is critical for game streaming — without it, the encoder buffers frames and adds unacceptable latency. If you're streaming pre-recorded video (not games), `fastdecode` is a good alternative since it reduces PSP CPU load.

---

### `qp` — Quality Parameter

**Default:** `28`
**Range:** 0–51

Same as hardware encoders. Only used in CQP rate control mode. Lower = better quality but higher bitrate. For software encoding with `vbr_latency` rate control, this is ignored.

---

## Recommended Software Config for PSP

```ini
encoder = sw
sw_preset = superfast
sw_tune = zerolatency
qp = 28
fec_percentage = 20
```

## Performance Notes

At 368×208@15 fps, software encoding uses very little CPU because the resolution is tiny. Even a 10-year-old laptop CPU can handle this. The bottleneck will be your network, not the encoder.

If you see high CPU usage, check if another program is also using the CPU. Software encoding at PSP resolution should use less than 5% of a modern CPU.
