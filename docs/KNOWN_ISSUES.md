# Known Issues

_PSP Moonlight v1.4.0_

This document lists user-facing limitations, compatibility notes, and practical guidance for stable PSP streaming.

---

## Current Limitations

### CABAC is supported, but CAVLC is still recommended for lower CPU overhead

**Impact:** CABAC is now fully supported and optimized (highly recommended for AMD encoders which force CABAC and ignore CAVLC requests). However, CAVLC still requires slightly less CPU/ME power.

**Guidance:** Configure Sunshine entropy coding to CAVLC if your GPU/encoder supports it, to save battery and reduce heat on the PSP:

- NVIDIA: `nvenc_h264_cavlc = enabled`
- AMD: `amd_coder = cavlc`
- Intel QSV: `qsv_coder = cavlc`

### PSP Wi-Fi is the main bottleneck

**Impact:** PSP hardware is limited to 2.4 GHz Wi-Fi. Burst loss, crowded channels, or weak signal can cause stutter, recovery pauses, or audio artifacts.

**Guidance:** Start with the Performance preset, keep the PSP close to the access point, and avoid congested 2.4 GHz channels.

### Performance preset disables local audio

**Impact:** Performance mode prioritizes video and input responsiveness by skipping local Opus decode and audio playback on the PSP.

**Guidance:** Switch Audio to Enabled, or use Balanced/Quality, if you need sound. Audio Disabled keeps only the host audio liveness ping and skips local PSP audio receive/decode/playback.

### Native resolution costs more decode time

**Impact:** Quality mode uses the native 480x272 PSP resolution, so it runs at 10 fps by default.

**Guidance:** Use Quality when native detail matters. Use Balanced or Performance when smoothness matters more.

### Very high resolutions are not practical

**Impact:** Resolutions above the PSP LCD add host/network/decode work without useful display detail.

**Guidance:** Stay within the built-in presets unless you are testing a specific custom stream size.

### Icon download/cache behavior

**Impact:** Game-library icons use Sunshine box-art paths with static PNG decode and raw RGB565 cache files.

**Guidance:** If icons appear stale or missing, clear `ms0:/PSP/SAVEDATA/Moonlight/cache/` and refresh the game library.

### Power-switch resume is limited

**Impact:** Sleep/resume behavior depends on the host session state and Wi-Fi reconnect timing.

**Guidance:** If resume does not reconnect cleanly, quit the stream and relaunch from the game library.

---

## Hotspot and Remote Session Notes

PSP Moonlight supports UPnP IGD port mapping assistance for hotspot and remote/NAT-constrained sessions.

### What UPnP assist does

- Requests temporary UDP mappings for active video/audio RTP and RTCP ports.
- Cleans mappings on session teardown or failure.
- Improves compatibility when inbound NAT traversal is required.

### When UPnP assist may not work

- Gateway or hotspot does not implement UPnP IGD.
- UPnP is disabled by network policy.
- Carrier-grade NAT or restricted mobile networks block expected routing.

### Recommended remote-session checklist

- Enable UPnP on the gateway or hotspot if available.
- Confirm Sunshine is reachable on the intended route.
- Start with H.264 Baseline + CAVLC and the Performance preset.

---

## Recommended Starting Profile

- Codec: H.264
- Profile: Baseline
- Entropy: CAVLC
- PSP preset: Performance
- Stream: 300x170 @ 30 fps
- Bitrate: 384 kbps
- Packet size: 1056 bytes
- Audio: Disabled

If that is stable and you need more detail or audio, move to Balanced. If you want native PSP resolution, use Quality.

---

## Out of Scope

- H.265/AV1 decode on PSP
- Desktop-class visual quality at modern streaming bitrates
- Non-PSP platform support
