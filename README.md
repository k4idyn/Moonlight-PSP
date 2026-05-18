<div align="center">

# PSP Moonlight

**v1.2.0 - PSP-native H.264 Game Streaming Client**

[![Build](https://img.shields.io/badge/build-passing-brightgreen)](#building)
[![PSP FW](https://img.shields.io/badge/PSP%20FW-6.60%2F6.61-blue)](#requirements)
[![License](https://img.shields.io/badge/license-GPLv3-blue)](#license)
[![Status](https://img.shields.io/badge/status-release%20candidate-yellow)](#known-limitations)
[![Release](https://img.shields.io/badge/release-v1.2.0-blue)](#version-history)

</div>

---

PSP Moonlight is a Moonlight-compatible game-streaming client for Sony PSP systems, designed around a custom PSP-native networking, decode, audio, input, and rendering stack.

The project streams H.264 video from a host PC running Sunshine, decodes video on the PSP main CPU, and uses the Media Engine for accelerated YUV-to-RGBA conversion.

## New in v1.2

- Three PSP-focused presets are now built into the settings menu:
  - **Performance:** 300x170 at 30 fps, 384 kbps, 1056-byte packets, audio disabled by default.
  - **Balanced:** 360x204 at 20 fps, 480 kbps, 1200-byte packets, audio enabled.
  - **Quality:** 480x272 at 10 fps, 576 kbps, 1200-byte packets, native PSP resolution, audio enabled.
- The preset selector is separate from the Resolution row, so manual resolution changes no longer re-snap FPS, bitrate, packet size, or audio state.
- Packet size is now a first-class setting under bitrate.
- Stream sizes use the PSP LCD aspect ratio so built-in presets fill the 480x272 screen without fixed black bars.
- Pairing now completes the authenticated HTTPS confirmation step, cleans up failed pair attempts, and recovers from stale paired-host entries more reliably.
- The default controller map is tuned for Xbox-style input on PSP:
  - L + Triangle/Cross/Square/Circle = right stick up/down/left/right.
  - L + D-pad Left/Right = LT/RT.
  - L + D-pad Down/Up = L3/R3.
  - The mapper can also switch right-stick control to L + analog nub.
- Browser mode mouse movement has stronger acceleration, and app-owned combos such as HUD toggle and stream exit are kept local instead of leaking host input.
- The game library and stream-start paths prime display buffers to reduce stale menu/frame flicker during screen transitions.

## Highlights

| Feature | Status | Notes |
|---|---|---|
| Host discovery | Implemented | mDNS, known-host probes, optional subnet scan |
| Pairing + TLS transport auth | Implemented | Runtime identity, PIN flow, authenticated confirm |
| Game library + icons | Implemented | Sunshine box-art download, static PNG decode, raw RGB565 cache |
| RTSP / RTP / FEC pipeline | Implemented | PSP-native transport with CAVLC host profile required |
| OpenH264 decode + ME conversion | Implemented | PSP-optimized software decode path |
| Audio | Implemented | Opus playback when enabled; Performance preset disables local audio work |
| Input | Implemented | Xbox and Browser modes, customizable PSP combo mapper |
| UPnP hotspot/remote assist | Implemented | Temporary IGD UDP port mapping for stream ports |
| Multi-host support | Implemented | Up to 8 paired hosts |

## Recommended Host Profile

- Codec: H.264
- Encoder profile: Baseline
- Entropy: CAVLC
- Rate control: low latency / bandwidth-limited mode
- FEC: start at 35 percent for PSP Wi-Fi, then tune only if your network is clean

This guidance applies to NVIDIA NVENC, AMD AMF, Intel QSV, and software x264 hosts. See the encoder guides in `docs/` for backend-specific keys.

## Preset Guide

| Preset | Use When | Stream | Audio |
|---|---|---|---|
| Performance | You want the most responsive PSP-1000 profile | 300x170, 30 fps, 384 kbps, 1056-byte packets | Disabled |
| Balanced | You want a middle point between detail and smoothness | 360x204, 20 fps, 480 kbps, 1200-byte packets | Enabled |
| Quality | You want native PSP resolution | 480x272, 10 fps, 576 kbps, 1200-byte packets | Enabled |

Audio Disabled is a client-side low-work mode. It skips local Opus decode, SRC playback, and audio output work on the PSP. It does not require changing Sunshine host settings and can be changed from the PSP settings menu.

## How It Works

```text
Main CPU (Allegrex)                    Media Engine
--------------------                   ---------------------------
Wi-Fi receive + RTP/FEC processing     YUV420P -> RGBA8888 via VFPU
OpenH264 software decode               Concurrent conversion work
Control/input channel                  Output frame handoff
Opus audio decode when enabled
```

## Requirements

### PSP

- PSP-1000 / PSP-2000 / PSP-3000
- Custom firmware: ARK-4 on 6.60/6.61 is the supported target
- 2.4 GHz Wi-Fi

### Host PC

- Sunshine current stable release recommended
- H.264 Baseline + CAVLC configured for PSP-compatible streaming

## Building

See [docs/BUILDING.md](docs/BUILDING.md) for full environment setup.

```bash
# 1) Build Media Engine helper PRX
cd moonlight_me_helper && make

# 2) Build application. Retail mode is the default.
cd .. && make

# Optional verbose diagnostics build
make RETAIL_BUILD=0
```

Build output includes:

- `EBOOT.PBP`
- `moonlight_me_helper.prx`

## Install

Quick install path:

1. Create this folder on the Memory Stick:

```text
ms0:/PSP/GAME/Moonlight/
```

2. Copy both files into that folder:

- `EBOOT.PBP`
- `moonlight_me_helper.prx`

3. Launch from XMB -> Game -> Memory Stick.

For full setup, pairing flow, and troubleshooting, see [INSTALL.md](INSTALL.md).

## Known Limitations

- The PSP Wi-Fi radio is 2.4 GHz only and remains the main practical bottleneck.
- H.264 CAVLC is required for normal playback; CABAC is not recommended for PSP use.
- High resolutions above the native PSP display are outside the intended operating range.
- Some fast-motion content can still expose the PSP display and network limits.

Detailed notes: [docs/KNOWN_ISSUES.md](docs/KNOWN_ISSUES.md)

## Documentation Index

- [docs/BUILDING.md](docs/BUILDING.md)
- [docs/KNOWN_ISSUES.md](docs/KNOWN_ISSUES.md)
- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)
- [docs/NVENC_SETTINGS_GUIDE.md](docs/NVENC_SETTINGS_GUIDE.md)
- [docs/AMD_SETTINGS_GUIDE.md](docs/AMD_SETTINGS_GUIDE.md)
- [docs/QSV_SETTINGS_GUIDE.md](docs/QSV_SETTINGS_GUIDE.md)
- [docs/SOFTWARE_ENCODING_GUIDE.md](docs/SOFTWARE_ENCODING_GUIDE.md)
- [docs/GAME_LIST_PARSER.md](docs/GAME_LIST_PARSER.md)
- [docs/RTP_REASSEMBLY.md](docs/RTP_REASSEMBLY.md)
- [docs/SAFETY_BUFFER.md](docs/SAFETY_BUFFER.md)
- [docs/ME_DECODER_THREAD.md](docs/ME_DECODER_THREAD.md)
- [docs/PAIRING.md](docs/PAIRING.md)
- [docs/UI_FLOW.md](docs/UI_FLOW.md)
- [docs/DECODER_PIPELINE.md](docs/DECODER_PIPELINE.md)
- [INSTALL.md](INSTALL.md)
- [CHANGELOG.md](CHANGELOG.md)

## Version History

- v1.2.0: PSP preset ladder, packet-size setting, pairing confirm, controller-map overhaul, Browser input cleanup, and transition-buffer fixes.
- v1.1.0: Network controller refinements and retail artifact publication.
- v1.0.0: Public release of the PSP-native rewrite.
- v0.2.x: Public beta cycle.

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md).

## License

GPLv3 - see [LICENSE](LICENSE).
