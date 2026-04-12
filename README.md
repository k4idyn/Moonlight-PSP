<div align="center">

# PSP Moonlight

**v0.2.0-beta · Asymmetric Dual-Core Software H.264 Streaming Client for Sony PlayStation Portable**

[![Build](https://img.shields.io/badge/build-passing-brightgreen)](#building)
[![PSP FW](https://img.shields.io/badge/PSP%20FW-6.60%2F6.61-blue)](#requirements)
[![License](https://img.shields.io/badge/license-GPLv3-blue)](#license)
[![Status](https://img.shields.io/badge/status-beta-yellow)](#known-issues)
[![Release](https://img.shields.io/badge/release-v0.2.0--beta-orange)](#changelog)

</div>

---

PSP Moonlight is a [Moonlight](https://moonlight-stream.org/) game-streaming client for the **Sony PlayStation Portable**. It streams H.264 video from a host PC running [Sunshine](https://github.com/LizardByte/Sunshine) over Wi-Fi using a fully custom software decode pipeline built from scratch for PSP hardware.

This is a **complete architectural rewrite** of the original [moonlight-psp-core](https://github.com/k4idyn/Moonlight-PSP) project (v0.1.0.1–v0.1.0.3-alpha), which used `moonlight-common-c` and the PSP hardware MPEG decoder (`sceMpegAvcDecode`). That approach stalled — the `sceMpeg` ringbuffer model is fundamentally incompatible with the Moonlight RTP stream model, and `moonlight-common-c` required POSIX threading that can't be cleanly ported to sceKernel. Nothing from that codebase carries over here.

This version is a ground-up custom RTSP/RTP/FEC/H.264/VFPU pipeline with zero dependency on `moonlight-common-c`. It was developed internally and never published in alpha form. **v0.2.0-beta is the first public release of the working decoder.**

> **Status (2026-04-11):** Beta. Connects, pairs, fetches game library, receives video, decodes H.264, and outputs RGBA via the VFPU Media Engine. Full end-to-end streaming confirmed on real PSP-1000 hardware at 15–18 fps. Audio, input forwarding, FEC recovery, and watchdog auto-restart are all live.

---

## How It Works

PSP Moonlight uses an **asymmetric dual-core decode pipeline** that splits H.264 decode across the PSP's two processors:

```
Main CPU (Allegrex @ 333 MHz)        Media Engine (ME @ 222 MHz)
─────────────────────────────         ──────────────────────────
UDP receive / RTP reassembly          YUV420P → RGBA8888 via VFPU
Reed-Solomon FEC repair               (runs concurrently with decode)
FFmpeg libavcodec H.264 decode
Control stream + input forward
Opus stereo audio decode
```

The ME executes a fully vectorised YUV→RGBA conversion in VFPU. While the main CPU is decoding the next frame in FFmpeg, the ME is converting the previous frame's YUV output — throughput equals `max(decode_time, convert_time)` rather than their sum. The ME helper kernel module (`moonlight_me_helper.prx`) bootstraps the ME core and provides `InitME`/`KillME` exports.

---

## Feature Status

| Feature | Status | Notes |
|---|---|---|
| Wi-Fi connect + host discovery | ✅ Working | mDNS-compatible probe |
| TLS 1.2 pairing (mbedTLS) | ✅ Working | RSA + ECDH + AES-GCM |
| Game list + icon cache | ✅ Working | Server-side PNG icons, SD cache |
| RTSP session / AES-GCM setup | ✅ Working | Sunshine Gen7 protocol |
| UDP video receive + FEC | ✅ Working | Reed-Solomon, up to 66% parity |
| H.264 decode (FFmpeg) | ✅ Working | Baseline Profile, CAVLC |
| VFPU YUV→RGBA (Media Engine) | ✅ Working | >99.9% ME success rate |
| Opus stereo audio (48 kHz) | ✅ Working | Fixed-point Silk+CELT |
| Controller input forward | ✅ Working | All PSP buttons mapped |
| Dual-mode watchdog + auto-restart | ✅ Working | Recovers from ME hang and RTP stall |
| Sustained multi-frame streaming | ✅ Working | 15–18 fps @ 480×272, 180s clean |  
| Deblocking filter | ❌ Not yet | ME bandwidth limited |
| Double-buffered tearless output | ❌ Not yet | Planned post-beta |

---

## Hardware Test Results

All testing was on a real **PSP-1000** (not PPSSPP). Sunshine host, 2.4 GHz 802.11b/g, LAN.

| Metric | Result |
|---|---|
| FPS sustained | **15–18 fps** at 480×272, 500 kbps |
| ME YUV→RGBA | ~31 µs/frame (>99.9% on ME, <0.1% CPU fallback) |
| FFmpeg decode time | 20–50 ms/frame depending on macroblock complexity |
| Full 3-minute run | ✅ No freeze, no stall, no kernel panic |
| Button input confirmed | ✅ All PSP buttons registered on host |
| FEC recoveries per session | 2000+ at 500 kbps — expected, not a problem |
| Zero-artifact screenshots | **25/25 clean** — zero mosaic, zero corruption |

---

## Requirements

### Hardware
- Sony PSP-1000 / PSP-2000 / PSP-3000 (PSP Go untested)
- 802.11b/g Wi-Fi network
- Custom firmware: **ARK-4** (recommended), **6.60 PRO-C2**, or **6.61 ME/LME**

### Host PC
- [Sunshine](https://github.com/LizardByte/Sunshine) v0.20+ (or NVIDIA GameStream)
- Stream settings: **H.264 Baseline, CAVLC, 480×272, 15 fps, 500 kbps**

### Build Toolchain
- [pspdev/psptoolchain](https://github.com/pspdev/psptoolchain) (`psp-gcc 4.3.5`)
- GNU Make
- PSP-specific FFmpeg cross-compiled for MIPS/PSP

---

## Building

See [docs/BUILDING.md](docs/BUILDING.md) for full setup instructions.

```bash
# 1. Build the Media Engine helper kernel PRX
cd moonlight_me_helper && make

# 2. Build the main application
cd .. && make
```

Output: `EBOOT.PBP` + `moonlight_me_helper.prx`

---

## Installing on PSP

### Quick Install (ARK-4 CFW — Recommended)

ARK-4 is the most actively maintained PSP custom firmware. If you're using ARK-4:

1. Connect your PSP to your PC via USB or insert the Memory Stick into an adapter.
2. Navigate to `ms0:/PSP/GAME/` on the Memory Stick.
3. Create a new folder called `Moonlight`:
   ```
   ms0:/PSP/GAME/Moonlight/
   ```
4. Copy **both** files from the release into that folder:
   ```
   ms0:/PSP/GAME/Moonlight/EBOOT.PBP
   ms0:/PSP/GAME/Moonlight/moonlight_me_helper.prx
   ```
5. Eject USB / reinsert the Memory Stick.
6. On the PSP XMB, go to **Game → Memory Stick**.
7. Launch **PSP Moonlight**.

> **Important:** `moonlight_me_helper.prx` is a kernel-mode plugin that boots the Media Engine coprocessor. It **must** be in the same directory as `EBOOT.PBP` — the application loads it at startup. Without it, video decode will fail.

### Other CFW (PRO-C2, ME/LME, Infinity)

The same installation steps apply — `ms0:/PSP/GAME/Moonlight/` with both files. ARK-4 is recommended because it has the most stable kernel plugin loading for user PRXes.

### First Run

1. Make sure your PSP is connected to Wi-Fi (set up a network connection in **Settings → Network Settings** if you haven't already).
2. On your host PC, have [Sunshine](https://github.com/LizardByte/Sunshine) running.
3. Set Sunshine to: **H.264 Baseline, CAVLC, 480×272, 15 fps, 500 kbps**.
4. Launch Moonlight on the PSP — it will scan for hosts on your LAN.
5. Select your host and follow the on-screen pairing PIN prompt.
6. Once paired, select a game from the library to start streaming.

See [INSTALL.md](INSTALL.md) for detailed setup, troubleshooting, and Sunshine configuration.

---

## Repository Structure

```text
moonlight-psp/
├── docs/                       # Component documentation
├── include/                    # Public headers
├── legacy/                     # Archived CAVLC+VFPU pipeline (not in build)
├── lib/                        # Pre-built PSP static libraries (intraFont, etc.)
├── moonlight_me_helper/        # Media Engine kernel PRX (VFPU recon worker)
├── src/
│   ├── ffmpeg_decode.c         # FFmpeg H.264 decode + ME YUV→RGBA dispatch
│   ├── sw_decoder_thread.c     # Decoder thread + dual-mode watchdog
│   ├── rtp_reassembly.c        # RTP frame assembly
│   ├── rtp_fec.c               # Reed-Solomon FEC repair
│   ├── control_stream.c        # Moonlight control protocol + input
│   ├── network_connect.c       # RTSP + TLS session establishment
│   └── ...                     # Network, UI, crypto, audio
├── third_party/
│   ├── mbedtls/                # mbedTLS 2.28 (TLS 1.2, AES-GCM, RSA, ECDH)
│   └── opus/                   # libopus 1.4 (fixed-point Silk+CELT)
├── Makefile
└── PARAM.SFO
```

---

## Dependencies

| Library | Version | License | Purpose |
|---|---|---|---|
| mbedTLS | 2.28.x | Apache 2.0 | TLS 1.2, AES-GCM, RSA, ECDH |
| libopus | 1.4 | BSD 3-Clause | Opus audio decode |
| FFmpeg (PSP) | custom | LGPL | H.264 libavcodec |
| intraFont-G | — | Attribution | PSP UI font |
| PSPSDK | community | BSD | PSP system APIs |

mbedTLS and libopus are vendored in `third_party/` for reproducible builds.

---

## Known Issues

- **No deblocking filter:** Blockiness visible at ≤300 kbps. Workaround: use ≥500 kbps. Requires ME bandwidth that's currently allocated to YUV→RGBA.
- **No double buffering:** Display buffer swap is single-buffered. Tearing is visible during fast lateral movement. Fix planned.
- **15–18 fps ceiling on PSP-1000:** The Allegrex+ME can decode faster in isolation; the practical ceiling is the RTP/FEC processing overhead. PSP-2000/3000 may do slightly better.
- **802.11b packet loss:** The PSP's Wi-Fi is 802.11b-era. FEC handles routine loss; sustained >30% packet loss will cause visible stutter until IDR recovery fires.
- **ME crash recovery (rare):** Occasional ME timeout on first stream connect. Auto-recovery re-initializes ME within ~100ms; no manual restart required.  

See [docs/KNOWN_ISSUES.md](docs/KNOWN_ISSUES.md) for full details.

---

## Version History

**v0.2.0-beta (this release)** — First public beta. Complete ground-up rewrite. Custom pipeline, no `moonlight-common-c`, no `sceMpeg`. Full end-to-end streaming confirmed on real hardware.

**[moonlight-psp-core](https://github.com/k4idyn/Moonlight-PSP) v0.1.0.1–v0.1.0.3-alpha** — The original public project. Used `moonlight-common-c`, ENet, and `sceMpegAvcDecode`. Got as far as RTSP handshake stabilization and library cleanup. Never decoded a frame — the `sceMpeg` ringbuffer model requires MPEG-PS framing which is incompatible with Moonlight's RTP stream model, and `moonlight-common-c` assumes POSIX threading that can't be mapped cleanly to `sceKernel`. Archived. Nothing from that codebase carries into this one.

---

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md). PSP scene contributions welcome — hardware testing especially valuable.

---

## License

GPLv3 — see [LICENSE](LICENSE).
