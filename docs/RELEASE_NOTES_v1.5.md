# Moonlight PSP v1.5.0

## What's new

- Sony hardware H.264 decoding for CAVLC and CABAC streams.
- Improved video receive, frame buffering, color conversion and presentation.
- Corrected audio playback clock accounting.
- Reliable keyboard, mouse wheel, button and controller delivery.
- Visible analog mapping hints and improved HUD button handling.
- Improved decoder reopening and application cleanup.

## Streaming presets

Balanced is the recommended default for visual detail, smoothness and audio. New installations select it automatically; upgrades retain saved settings.

| Preset | Video | Bitrate | Audio |
|---|---|---|---|
| Quality | 480x272 at 15 fps | 576 kbps | On |
| Balanced (default) | 360x204 at 20 fps | 480 kbps | On |
| Performance | 300x170 at 30 fps | 384 kbps | Off |

## Installation

Copy `EBOOT.PBP` and its matching `moonlight_me_helper.prx` into `ms0:/PSP/GAME/Moonlight/`, then launch Moonlight from XMB. Keep the two files from the same package together.

Use a Sunshine-compatible host configured for H.264 and connect the PSP to a 2.4 GHz Wi-Fi access point. Settings and pairing information are stored in `ms0:/PSP/SAVEDATA/Moonlight/`.

See [INSTALL.md](../INSTALL.md) for setup and [Known Issues](KNOWN_ISSUES.md) for limitations.
