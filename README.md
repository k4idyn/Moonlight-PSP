<div align="center">

# PSP Moonlight

**v0.2.3-beta · Asymmetric Dual-Core Software H.264 Streaming Client for Sony PlayStation Portable**

[![Build](https://img.shields.io/badge/build-passing-brightgreen)](#building)
[![PSP FW](https://img.shields.io/badge/PSP%20FW-6.60%2F6.61-blue)](#requirements)
[![License](https://img.shields.io/badge/license-GPLv3-blue)](#license)
[![Status](https://img.shields.io/badge/status-beta-yellow)](#known-issues)
[![Release](https://img.shields.io/badge/release-v0.2.3--beta-orange)](#changelog)

</div>

---

PSP Moonlight is a [Moonlight](https://moonlight-stream.org/) game-streaming client for the **Sony PlayStation Portable**. It streams H.264 video from a host PC running [Sunshine](https://github.com/LizardByte/Sunshine) over Wi-Fi using a fully custom software decode pipeline built from scratch for PSP hardware.

This is a **complete architectural rewrite** of the original [moonlight-psp-core](https://github.com/k4idyn/Moonlight-PSP) project (v0.1.0.1–v0.1.0.3-alpha), which used `moonlight-common-c` and the PSP hardware MPEG decoder (`sceMpegAvcDecode`). That approach stalled — the `sceMpeg` ringbuffer model is fundamentally incompatible with the Moonlight RTP stream model, and `moonlight-common-c` required POSIX threading that can't be cleanly ported to sceKernel. Nothing from that codebase carries over here.

This version is a ground-up custom RTSP/RTP/FEC/H.264/VFPU pipeline with zero dependency on `moonlight-common-c`. It was developed internally and never published in alpha form. **v0.2.0-beta is the first public release of the working decoder.**

> **Status (2026-04-15):** Beta. Connects, pairs, fetches game library, receives video, decodes H.264, and outputs RGBA via the VFPU Media Engine. Full end-to-end streaming confirmed on real PSP-1000 hardware at 10–18 fps across 8 resolution/FPS configurations (256×144 through 480×272). New in v0.2.3: mDNS host auto-discovery, multi-host pairing (8 hosts), instant quit+relaunch with auto-resume, smooth-scroll UI with 3-layer shadows, deblocking re-enabled, and aggressive stream recovery tuning.

---

## How It Works

PSP Moonlight uses an **asymmetric dual-core decode pipeline** that splits H.264 decode across the PSP's two processors:

```
Main CPU (Allegrex @ 333 MHz)        Media Engine (ME @ 222 MHz)
─────────────────────────────         ──────────────────────────
UDP receive / RTP reassembly          YUV420P → RGBA8888 via VFPU
Reed-Solomon FEC repair               (runs concurrently with decode)
OpenH264 H.264 decode (low-latency)   Zero-delay mode at sub-native res
Control stream + input forward
Opus stereo audio with PLC recovery
```

The ME executes a fully vectorised YUV→RGBA conversion in VFPU. While the main CPU is decoding the next frame in OpenH264, the ME is converting the previous frame's YUV output — throughput equals `max(decode_time, convert_time)` rather than their sum. The ME helper kernel module (`moonlight_me_helper.prx`) bootstraps the ME core and provides `InitME`/`KillME` exports.

---

## Feature Status

| Feature | Status | Notes |
|---|---|---|
| Wi-Fi connect + host discovery | ✅ Working | mDNS multicast + subnet scan + HTTP probe |
| TLS 1.2 pairing (mbedTLS) | ✅ Working | RSA + ECDH + AES-GCM |
| Game list + icon cache | ✅ Working | Server-side PNG icons, SD cache |
| RTSP session / AES-GCM setup | ✅ Working | Sunshine Gen7 protocol |
| UDP video receive + FEC | ✅ Working | Reed-Solomon, up to 66% parity |
| H.264 decode (OpenH264) | ✅ Working | Baseline profile, CAVLC strongly recommended (CABAC supported but unstable — see INSTALL.md) |
| VFPU YUV→RGBA (Media Engine) | ✅ Working | >99.9% ME success rate |
| Opus stereo audio (48 kHz) | ✅ Working | Fixed-point Silk+CELT, PLC + volume ducking |
| Audio enable/disable toggle | ✅ Working | Settings menu option |
| Controller input forward | ✅ Working | All PSP buttons mapped |
| Custom button mapping | ✅ Working | L2/R2, right stick, L3/R3 remappable |
| Dual-mode watchdog + auto-restart | ✅ Working | 5-credit soft/hard recovery model |
| WiFi keepalive (active streaming) | ✅ Working | Prevents server-side stream stall |
| Keyboard / scroll events | ✅ Working | Text input + mouse scroll via combos |
| Controller battery reporting | ✅ Working | PSP battery % sent to host |
| PID adaptive bitrate | ✅ Working | Composite quality: RSSI+CQ+FEC |
| IDR exponential backoff | ✅ Working | 500ms→4000ms backoff, reduces flood |
| FEC predictive loss detection | ✅ Working | Pre-requests IDR on WiFi bursts |
| Dynamic resolution scaling | ✅ Working | 4-step ladder: 256×144 → 480×272 |
| Dynamic SO_RCVBUF | ✅ Working | 128KB–384KB based on quality |
| Quality hysteresis | ✅ Working | 3-reading minimum before transitions |
| Frame pacing | ✅ Working | Extra VBlank wait prevents tearing |
| Sustained multi-frame streaming | ✅ Working | 10–18 fps @ 480×272, 120s+ clean |
| Deblocking filter | ✅ Re-enabled | ~2–3ms/frame at sub-native res |
| Double-buffered tearless output | ❌ Not yet | Planned post-beta |

---

## Hardware Test Results

All testing was on a real **PSP-1000** (not PPSSPP). Sunshine host, 2.4 GHz 802.11b/g, LAN.

| Metric | Result |
|---|---|
| FPS sustained (480×272@15fps) | **13.8 fps** at 480×272, 500 kbps |
| FPS sustained (256×144@30fps) | **9.5 fps** at 256×144, 500 kbps |
| FPS sustained (368×208@20fps) | **12.3 fps** at 368×208, 500 kbps |
| ME YUV→RGBA | ~31 µs/frame (>99.9% on ME, <0.1% CPU fallback) |
| OpenH264 decode time | 36–80 ms/frame at 480×272 |
| Audio underrun rate | **0.0–0.5%** (with adaptive PLC + volume ducking) |
| Full 8-stage test (60s each) | ✅ 0 crashes across all 8 resolution/FPS configs |
| WiFi stability | ✅ 100% signal, 0 disconnects (keepalive active) |
| Features verified (31/39) | ✅ 31 ACTIVE, 0 INACTIVE, 8 N/A (not triggered) |
| Latency (network RTT) | **19–24 ms** (WiFi), host processing 1 ms |
| Packet loss | ~13–15% (802.11b typical) — FEC + PLC compensate |
| IDR requests/min | **5–15** (down from ~40–100 pre-backoff) |

### Resolution Compatibility

| Resolution | Target FPS | Result | Notes |
|---|---|---|---|
| 480×272 @15 | 15 | ✅ **13.8 fps** | Native PSP, best quality |
| 256×144 @30 | 30 | ✅ **9.5 fps** | Low-res, higher target |
| 368×208 @20 | 20 | ✅ **12.3 fps** | Sweet spot (custom) |
| 368×208 @15 | 15 | ✅ **17.3 fps** | Quality mode (custom) |
| 640×360 @15 | 15 | ⚠️ Splash only | Too demanding for decode |
| 854×480 @15 | 15 | ❌ No video | Exceeds PSP decode ceiling |
| 1280×720 @15 | 15 | ❌ No video | Far exceeds PSP capability |
| 480×272 @15 (browser) | 15 | ✅ **6.8 fps** | Browser mode verified |

---

## Requirements

### Hardware
- Sony PSP-1000 / PSP-2000 / PSP-3000 (PSP Go untested)
- 802.11b/g Wi-Fi network
- Custom firmware: **ARK-4** (recommended), **6.60 PRO-C2**, or **6.61 ME/LME**

### Host PC
- [Sunshine](https://github.com/LizardByte/Sunshine) v0.20+ (or NVIDIA GameStream)
- Stream settings: **H.264 Baseline, CAVLC, 480×272, 15 fps, 384 kbps**

### Build Toolchain
- [pspdev/psptoolchain](https://github.com/pspdev/psptoolchain) (`psp-gcc 4.3.5`)
- GNU Make
- Custom OpenH264 PSP port (decoder-only, single-threaded, low-latency)

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
3. Set Sunshine to: **H.264 Baseline, CAVLC, 480×272, 15 fps, 384 kbps**.
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
│   ├── openh264_decode.cpp     # OpenH264 H.264 decode + ME YUV→RGBA dispatch
│   ├── sw_decoder_thread.c     # Decoder thread + dual-mode watchdog
│   ├── rtp_reassembly.c        # RTP frame assembly
│   ├── rtp_fec.c               # Reed-Solomon FEC repair
│   ├── control_stream.c        # Moonlight control protocol + input
│   ├── network_connect.c       # RTSP + TLS session establishment
│   └── ...                     # Network, UI, crypto, audio
├── third_party/
│   ├── mbedtls/                # mbedTLS 2.28 (TLS 1.2, AES-GCM, RSA, ECDH)
│   ├── opus/                   # libopus 1.4 (fixed-point Silk+CELT)
│   └── openh264/               # OpenH264 (decoder-only, PSP port)
├── Makefile
└── PARAM.SFO
```

---

## Dependencies

| Library | Version | License | Purpose |
|---|---|---|---|
| mbedTLS | 2.28.x | Apache 2.0 | TLS 1.2, AES-GCM, RSA, ECDH |
| libopus | 1.4 | BSD 3-Clause | Opus audio decode |
| OpenH264 (PSP port) | custom | BSD 2-Clause | H.264 decode (low-latency) |
| intraFont-G | — | Attribution | PSP UI font |
| PSPSDK | community | BSD | PSP system APIs |

mbedTLS, libopus, and OpenH264 are vendored in `third_party/` for reproducible builds.

---

## Known Issues

- **No double buffering:** Display buffer swap is single-buffered. Tearing is visible during fast lateral movement. Fix planned.
- **10–18 fps at 480×272:** Decode time averages ~50ms at full resolution; frame budget is 66ms at 15fps. Spikes can cause occasional frame drops.
- **802.11b packet loss:** The PSP's Wi-Fi is 802.11b-era. ~37% audio packet loss typical. FEC and PLC handle routine loss; sustained loss causes audible static despite volume ducking.
- **Audio PLC static:** At high packet loss rates, consecutive PLC frames produce audible artifacts. Volume ducking reduces but doesn't eliminate this.
- **ME crash recovery (rare):** Occasional ME timeout on first stream connect. Auto-recovery re-initializes ME within ~100ms; no manual restart required.
- **Resolutions above 480×272:** The PSP's decode pipeline cannot sustain video at 640×360 or above. 368×208 is the practical sweet spot for custom resolutions.
- **8 protocol features untriggered in testing:** Graceful Disconnect, IDR Suppression, P-Frame Skip, Frame Pacing, RTP Stats API, Predictive Frame Drop, Audio Crypto Sep, and Session Resume require specific edge-case scenarios not exercised in standard streaming.

See [docs/KNOWN_ISSUES.md](docs/KNOWN_ISSUES.md) for full details.

---

## Version History

**v0.2.3-beta (this release)** — mDNS host discovery, multi-host pairing (8 hosts), instant quit+relaunch with auto-resume, smooth-scroll host list with 3-layer shadows, deblocking re-enabled, codec-aligned bitrate presets, FEC/IDR recovery tuning, and polished card UI across all screens.

**v0.2.2-beta** — PID-based adaptive bitrate, FEC prediction, IDR exponential backoff, dynamic resolution scaling (4-step ladder), keyboard/scroll/battery events, quality-adaptive audio PLC, frame pacing, P-frame skip-ahead, enhanced watchdog, and HUD improvements. 31/39 protocol features verified across 8-stage automated test.

**v0.2.1-beta** — OpenH264 decoder (replaced FFmpeg), first-time audio streaming with Opus decode, custom button mapping UI, WiFi keepalive during streaming, PLC volume ducking, and revamped settings menu.

**v0.2.0-beta** — First public beta. Complete ground-up rewrite. Custom pipeline, no `moonlight-common-c`, no `sceMpeg`. Full end-to-end streaming confirmed on real hardware.

**[moonlight-psp-core](https://github.com/k4idyn/Moonlight-PSP) v0.1.0.1–v0.1.0.3-alpha** — The original public project. Used `moonlight-common-c`, ENet, and `sceMpegAvcDecode`. Got as far as RTSP handshake stabilization and library cleanup. Never decoded a frame — the `sceMpeg` ringbuffer model requires MPEG-PS framing which is incompatible with Moonlight's RTP stream model, and `moonlight-common-c` assumes POSIX threading that can't be mapped cleanly to `sceKernel`. Archived. Nothing from that codebase carries into this one.

---

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md). PSP scene contributions welcome — hardware testing especially valuable.

---

## License

GPLv3 — see [LICENSE](LICENSE).
