<div align="center">

# PSP Moonlight

**v1.0.0 - Dual-Core Software H.264 Streaming Client for Sony PlayStation Portable**

[![Build](https://img.shields.io/badge/build-passing-brightgreen)](#building)
[![PSP FW](https://img.shields.io/badge/PSP%20FW-6.60%2F6.61-blue)](#requirements)
[![License](https://img.shields.io/badge/license-GPLv3-blue)](#license)
[![Status](https://img.shields.io/badge/status-stable-brightgreen)](#known-limitations)
[![Release](https://img.shields.io/badge/release-v1.0.0-blue)](#version-history)

</div>

---

PSP Moonlight is a Moonlight-compatible game-streaming client for Sony PSP systems, designed around a custom PSP-native networking, decode, audio, and rendering stack.

The project streams H.264 video from a host PC running Sunshine, performs software decode on the main CPU, and uses the Media Engine for accelerated YUV-to-RGBA conversion.

## What Changed Since v0.2.3-beta

Using the current repository history at https://github.com/k4idyn/Moonlight-PSP (`v0.2.3-beta..main`), this v1.0.0 tree includes:

- A PSP-native end-to-end pipeline rewrite (custom RTSP/RTP/FEC/decode path; no `moonlight-common-c` dependency).
- OpenH264 software decode + ME VFPU YUV420P->RGBA conversion pipeline with watchdog-driven recovery.
- Host discovery and UX upgrades: mDNS discovery, subnet scan, multi-host pairing, and persistent icon cache flow.
- Binary-safe HTTPS icon asset downloads with static PNG decode buffers and raw RGB565 cache format.
- Adaptive streaming controls: quality controller, loss-aware recovery behavior, and robust CAVLC-focused host compatibility.
- UPnP IGD support for hotspot/remote RTP/RTCP mapping assistance.

## Highlights

| Feature | Status | Notes |
|---|---|---|
| Host discovery | Ready | mDNS + optional subnet scan |
| Pairing + TLS transport auth | Ready | Runtime identity + pin-based trust |
| Game library + icons | Ready | Normal Sunshine box-art download, static PNG decode, raw RGB565 cache |
| RTSP / RTP / FEC pipeline | Ready | Real-hardware validated, CAVLC host profile required for stable normal playback |
| OpenH264 decode + ME conversion | Ready | PSP-optimized software decode path |
| Opus stereo audio | Ready | PLC + adaptive handling |
| UPnP hotspot/remote assist | Ready | Automatic IGD UDP port mapping for RTP/RTCP session ports |
| Multi-host support | Ready | Up to 8 paired hosts |

## Hotspot and Remote Sessions (UPnP)

PSP Moonlight includes UPnP IGD integration to improve session setup when streaming over:

- Mobile hotspot networks
- Public IP / WAN remote sessions
- NAT paths that require inbound UDP mapping for RTP/RTCP

During session preparation, the client requests temporary UDP mappings for active stream ports and cleans them up when the session ends.

### Requirements for UPnP assist

- Your gateway/hotspot must support UPnP IGD
- UPnP must be enabled on the gateway/hotspot
- Host PC must be reachable by the selected session route

If UPnP is unavailable, streaming may still work on LAN or on NAT setups that do not require explicit mapping.

## How It Works

```
Main CPU (Allegrex)                    Media Engine
--------------------                   ---------------------------
Wi-Fi receive + RTP/FEC processing     YUV420P -> RGBA8888 via VFPU
OpenH264 software decode               Concurrent conversion work
Control/input channel                  Output frame handoff
Opus audio decode + recovery
```

## Requirements

### PSP

- PSP-1000 / PSP-2000 / PSP-3000
- Custom firmware (ARK-4 recommended)
- 2.4 GHz Wi-Fi

### Host PC

- Sunshine (current stable release recommended)
- H.264 output configured for PSP-compatible streaming

Recommended baseline host profile:

- Codec: H.264
- Encoder profile: Baseline
- Entropy: CAVLC (required for PSP v1.0 validation and normal playback)
- Resolution/FPS: Start with 480x272 @ 15 fps
- Bitrate: Start around 384 kbps and tune as needed

## Building

See [docs/BUILDING.md](docs/BUILDING.md) for full environment setup.

```bash
# 1) Build Media Engine helper PRX
cd moonlight_me_helper && make

# 2) Build application
cd .. && make
```

Build output includes:

- EBOOT.PBP
- moonlight_me_helper.prx

## Install

Quick install path:

1. Create folder:

```
ms0:/PSP/GAME/Moonlight/
```

2. Copy:

- EBOOT.PBP
- moonlight_me_helper.prx

3. Launch from XMB -> Game -> Memory Stick.

For full setup, pairing flow, and troubleshooting, see [INSTALL.md](INSTALL.md).

## Known Limitations

- No tearless double-buffer output yet
- PSP Wi-Fi quality can still be a limiting factor on weak signals
- Very high streaming resolutions are outside practical PSP decode limits

Detailed notes: [docs/KNOWN_ISSUES.md](docs/KNOWN_ISSUES.md)

## Documentation Index

- [docs/BUILDING.md](docs/BUILDING.md)
- [docs/RELEASE_READINESS.md](docs/RELEASE_READINESS.md)
- [docs/KNOWN_ISSUES.md](docs/KNOWN_ISSUES.md)
- [docs/GAME_LIST_PARSER_README.md](docs/GAME_LIST_PARSER_README.md)
- [docs/PAIRING_README.md](docs/PAIRING_README.md)
- [docs/DECODER_PIPELINE.md](docs/DECODER_PIPELINE.md)
- [INSTALL.md](INSTALL.md)
- [CHANGELOG.md](CHANGELOG.md)

## Version History

- v1.0.0: Public release of the PSP-native rewrite with documented host compatibility guidance and release validation.
- v0.2.x: Public beta cycle.

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md).

## License

GPLv3 — see [LICENSE](LICENSE).
