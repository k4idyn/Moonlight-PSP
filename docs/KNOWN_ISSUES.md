# Known Issues

_PSP Moonlight v1.0.0_

This document lists user-facing limitations, compatibility notes, and practical guidance for stable streaming on PSP hardware.

---

## Current Limitations

### CAVLC host output is required for stable normal playback
**Impact:** CABAC streams are intentionally treated as unsupported in normal playback mode.
**Guidance:** Configure Sunshine entropy coding to CAVLC (`amd_coder=cavlc`, `nvenc_h264_cavlc=enabled`, `qsv_coder=cavlc`).

### Icon download/cache behavior
**Impact:** Game-library icons use the normal Sunshine box-art path with static-memory decode and raw RGB565 cache files.
**Guidance:** If icons appear stale or missing, clear `ms0:/PSP/GAME/Moonlight/cache/` and refresh the game library.

### No tearless double buffering yet
**Impact:** Fast horizontal motion can show tearing.
**Status:** Planned future improvement.

### Practical performance ceiling on PSP hardware
**Impact:** Effective framerate depends on stream profile, signal quality, and host encode settings. Overly aggressive settings can cause stutter or delayed recovery.
**Guidance:** Start with 480x272 at 15 fps and tune upward only if stable.

### High resolutions are not practical on PSP
**Impact:** Resolutions significantly above 480x272 can exceed decode/network limits and degrade usability.
**Guidance:** 368x208 and 480x272 are recommended operating ranges.

### Wi-Fi quality remains the primary bottleneck
**Impact:** Burst loss or weak 2.4 GHz conditions can still cause occasional recovery events and audio artifacts.
**Guidance:** Improve signal quality, reduce bitrate, and avoid congested channels.

---

## Hotspot and Remote Session Notes (UPnP)

PSP Moonlight supports UPnP IGD port mapping assistance for hotspot and remote/NAT-constrained sessions.

### What UPnP assist does

- Requests temporary UDP mappings for active video/audio RTP/RTCP ports
- Cleans mappings on session teardown/failure
- Improves connection reliability when direct inbound NAT traversal is required

### When UPnP assist may not work

- Gateway/hotspot does not implement UPnP IGD
- UPnP is disabled by network policy
- Carrier-grade NAT or restricted ISP/mobile network behavior blocks expected routing

### Recommended remote-session checklist

- Enable UPnP on the gateway/hotspot if available
- Confirm Sunshine host is reachable on the intended route
- Keep host profile conservative first (H.264 Baseline, CAVLC, moderate bitrate)

---

## Host Compatibility Guidance

### Recommended baseline stream profile

- Codec: H.264
- Profile: Baseline
- Entropy: CAVLC (required; CABAC is treated as unsupported in normal PSP mode)
- Initial target: 480x272 @ 15 fps
- Initial bitrate: around 384 kbps

### If video is unstable

- Reduce bitrate first
- Then reduce resolution to 368x208
- Verify Wi-Fi strength and channel congestion

---

## Out of Scope

- H.265/AV1 decode on PSP
- Full parity with modern desktop-class hardware decoders
- Non-PSP platform-specific support requirements
