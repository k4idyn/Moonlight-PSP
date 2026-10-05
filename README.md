<div align="center">

# Moonlight PSP

**v1.5.0 - PSP-native H.264 Game Streaming Client**

[![PSP FW](https://img.shields.io/badge/PSP%20FW-6.60%2F6.61-blue)](#requirements)
[![License](https://img.shields.io/badge/license-GPLv3-blue)](#license)
[![Release](https://img.shields.io/badge/release-v1.5.0-blue)](#version-history)


</div>

---

Moonlight PSP is a Moonlight-compatible game-streaming client for Sony PSP systems, designed around a custom PSP-native networking, decode, audio, input, and rendering stack.

v1.5 adds Sony hardware H.264 decoding for CAVLC and CABAC streams. Balanced is the recommended default for visual detail, smoothness and audio.

## New in v1.5

- **Sony hardware H.264 decoding**: Decode CAVLC and CABAC streams through the PSP's AVC hardware, with the decoder mode selected from the stream.
- **Video presentation improvements**: Reduced receive copies, owned frame buffers, corrected color range and quicker presentation of ready frames.
- **Audio playback improvements**: Corrected playback clock accounting across silence, held audio and queue trims.
- **Reliable controls**: Corrected keyboard, mouse wheel and controller packets, with acknowledged delivery for buttons, keys and gamepad state.
- **Cleaner stream exit**: Release decoder, application memory and network resources when leaving the stream or application.
- **Mapping interface fixes**: Visible analog direction badges and local HUD controls that do not send unwanted keys to the host.

## Highlights

| Feature | Status | Notes |
|---|---|---|
| Host discovery | Implemented | mDNS, known-host probes, optional subnet scan |
| Pairing + TLS transport auth | Implemented | Runtime identity, PIN flow, authenticated confirm |
| Game library + icons | Implemented | Sunshine box-art download, static PNG decode, raw RGB565 cache |
| RTSP / RTP / FEC pipeline | Implemented | Packet assembly and recovery for Sony hardware AVC |
| Sony hardware AVC | Implemented | Hardware decoding with actual CAVLC/CABAC PPS selection |
| Audio | Implemented | Opus playback is enabled in Quality and Balanced; Performance disables local audio work |
| Input | Implemented | Xbox and Browser modes, customizable PSP combo mapper |
| UPnP hotspot/remote assist | Implemented | Temporary IGD UDP port mapping for stream ports |
| Multi-host support | Implemented | Up to 8 paired hosts |

## Recommended Host Profile

- Codec: H.264
- H.264 profile and entropy: Baseline with CAVLC, or Main with CABAC
- The hardware decoder selects its mode from the emitted PPS
- Rate control: low latency / bandwidth-limited mode
- FEC: start at 35 percent for PSP Wi-Fi, then tune only if your network is clean

This guidance applies to NVIDIA NVENC, AMD AMF, Intel QSV, and software x264 hosts. See the encoder guides in `docs/` for backend-specific keys.

## Preset Guide

| Preset | Use When | Stream | Audio |
|---|---|---|---|
| Performance | You want the most responsive PSP profile | 300x170, 30 fps, 384 kbps, 1056-byte packets | Disabled |
| Balanced (default) | Recommended for visual detail, smoothness and audio | 360x204, 20 fps, 480 kbps, 1200-byte packets | Enabled |
| Quality | You want native PSP resolution | 480x272, 15 fps, 576 kbps, 1200-byte packets | Enabled |

New installations start with Balanced. Existing saved settings are retained when upgrading.

Audio Disabled is a client-side low-work mode. It skips local Opus decode, SRC playback, and audio output work on the PSP. It does not require changing Sunshine host settings and can be changed from the PSP settings menu.

## How It Works

The client uses Sony's sceMpeg AVC framework. The Media Engine helper loads firmware providers and prepares the AVC mode. The client inspects the stream's PPS before submitting the first IDR and presents decoded video through the PSP graphics engine.

## Requirements

### PSP

- PSP-1000 / PSP-2000 / PSP-3000
- Custom firmware: ARK-4 on 6.60/6.61 is the supported target
- 2.4 GHz Wi-Fi

### Host PC

- Sunshine current stable release recommended
- H.264 Baseline/CAVLC or Main/CABAC, with low-latency rate control

## Building

See [docs/BUILDING.md](docs/BUILDING.md) for full environment setup.

```bash
# From the repository root; make all builds the helper and application
make clean
make -j2 RETAIL_BUILD=1 PSP_HARDWARE_AVC=1 PSP_AVC_UNIFIED_MAIN_MODE=1 PSP_VIDEO_FEC_PERCENT=35 PSP_VIDEO_FEC_MIN_REQUIRED=1 PSP_AUDIO_PACKET_DURATION_MS=40
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

Runtime data is written to `ms0:/PSP/SAVEDATA/Moonlight/`, not the install folder.

When updating from an older build, follow [INSTALL.md](INSTALL.md).

For full setup, pairing flow, and troubleshooting, see [INSTALL.md](INSTALL.md).

## Known Limitations

- The PSP Wi-Fi radio is 2.4 GHz only and remains the main practical bottleneck.
- CAVLC and CABAC differ in compression efficiency and decode work. Use the built-in presets for a balance of detail and responsiveness.

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

- v1.5.0: Sony hardware AVC decoding, improved video presentation and audio timing, reliable controls, mapping hints and stream cleanup.
- v1.4.0: H.264 CABAC stream decoding support, 3-slot decoder ring buffer, safe self-module exit teardown, and Net Resolver DNS hostname support.
- v1.3.0: Savedata runtime layout, remote public-IP launch fix, pairing persistence, XMB artwork, and safer HOME/XMB exit cleanup.
- v1.2.0: PSP preset ladder, packet-size setting, pairing confirm, controller-map overhaul, Browser input cleanup, and transition-buffer fixes.
- v1.1.0: Network controller refinements and retail artifact publication.
- v1.0.0: Public release of the PSP-native rewrite.
- v0.2.x: Public beta cycle.

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md).

## License

GPLv3 - see [LICENSE](LICENSE).
