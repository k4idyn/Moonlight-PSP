# Changelog

All notable changes to PSP Moonlight are documented here.

v0.2.0-beta was the first public release of this codebase. For the history of the original
`moonlight-psp-core` project (the prior public alpha that used `sceMpeg` + `moonlight-common-c`),
see the [archived repo](https://github.com/k4idyn/Moonlight-PSP).

---

## [1.5.0] - 2026-10-05

### Application Startup
- Load the adjacent Media Engine helper when launching from XMB.
- Initialize the requested Media Engine mode when the helper starts fresh.
- Show initialization errors with a readable code and save the last fatal error for bug reports.

### Decoders and Playback
- Added Sony hardware AVC decoding with automatic CAVLC/Baseline and CABAC/Main selection.
- Reduced video receive copies and added a six-slot output pool with clear frame ownership.
- Corrected the hardware video color-range request and crop-aware conversion.
- Reduced the wait for ready hardware frames while retaining synchronized display swaps.
- Preserved earlier software decoder source under `legacy/software/`.

### Audio
- Corrected audio playback timing so silence and repeated output do not advance the consumed-source clock.
- Account for intentional audio queue trims without treating them as played samples.

### Input and Interface
- Corrected Moonlight keyboard, wheel, controller-arrival and battery packet layouts.
- Added acknowledged delivery for buttons, keys, wheel and controller state.
- Keep HUD closing buttons local until release to prevent unwanted host input.
- Restore visible analog direction badges in the mapping menu.

### Teardown and Resource Management
- Rebind Sony MPEG imports when reopening a stream.
- Export application stop cleanup and release the application heap, decoder and frame resources during exit.

### Streaming Presets
- Balanced is the recommended default for new installations; existing saved settings are retained.
- Quality: 480x272 at 15 fps, 576 kbps, with audio enabled.
- Balanced: 360x204 at 20 fps, 480 kbps, with audio enabled.
- Performance: 300x170 at 30 fps, 384 kbps, with audio disabled.

---

## [1.4.0] - 2026-07-18

### Decoders and Playback
- Added full support and optimizations for H.264 CABAC streams. This allows smooth, stutter-free playback on hosts using AMD AMF encoders (which often force CABAC and ignore CAVLC requests).
- Implemented a 3-slot RGBA ring buffer in the decoder to avoid frame memory/pointer contention between the presenter and the decoder thread.
- Optimized 8x8 and 4x4 inverse discrete cosine transform (IDCT) code paths with DC-only sparse residual fast paths.
- Skips redundant active-texture dcache writeback for Media Engine clean buffers.

### Networking
- Initialized Net Resolver and implemented DNS resolution support via `gethostbyname()` with a fallback to `inet_addr()`, fixing external IP and host connection issues (Issue #8).

### Teardown and Resource Management
- Implemented safe in-app teardown and graceful self-module unloading, avoiding black screen hangs on exit. Lingering process-owned threads, exit callback threads, and sockets are cleanly closed before memory unloads.

---

## [1.3.0] - 2026-05-29

### Memory Stick Layout
- Moved runtime writes to `ms0:/PSP/SAVEDATA/Moonlight/`, including config, button map, pairing certificate/key, client ID, TLS pins, icon cache, logs, app-list dumps, raw/IDR debug dumps, session caches, and safety-buffer fallback files.
- Added shared storage path helpers so new runtime writes create the savedata directory instead of recreating root-level or install-folder clutter.
- Updaters can delete stale old-build folders/files such as `ms0:/moonlight/`, old root-level Moonlight logs/dumps, and the old `ms0:/PSP/GAME/Moonlight/` install folder before copying v1.3. Pair again after the clean install.

### Remote and Pairing
- Remote/hotspot launches now keep the user-selected public IP for RTSP when Sunshine returns a private LAN address in `sessionUrl`.
- Pairing now persists the paired host after authenticated pairing succeeds, including manual public-IP pairing paths.

### XMB and Exit
- Added PSP-safe `ICON0.PNG` and `PIC1.PNG` artwork to the EBOOT build so the XMB icon and background render correctly.
- HOME/XMB exit now routes through the unified app cleanup path, aborts the idle Wi-Fi helper without waiting, disconnects networking, shuts down input/UI/display resources, and includes a `module_stop()` fallback for shell-driven exits.

---

## [1.2.0] - 2026-05-18

### Streaming Presets
- Added a dedicated Preset row with PSP-focused Performance, Balanced, and Quality defaults.
- Performance now defaults to 300x170 at 30 fps, 384 kbps, 1056-byte packets, and audio disabled.
- Balanced now defaults to 360x204 at 20 fps, 480 kbps, 1200-byte packets, and audio enabled.
- Quality now defaults to native 480x272 at 10 fps, 576 kbps, 1200-byte packets, and audio enabled.
- Built-in stream sizes use the PSP LCD aspect ratio so presets fill the display without fixed black bars.
- The Resolution row is independent from the Preset row, so manual resolution changes no longer reapply the whole preset ladder.
- Packet size is now configurable from the settings menu and is saved to `config.ini`.

### Pairing and Host Flow
- Pairing now completes the authenticated HTTPS `pairchallenge` confirmation after the signed pairing secret.
- Failed or cancelled pair attempts now best-effort unpair from the host so later attempts do not inherit stale half-paired state.
- Stale paired-host entries are cleared when a host reports paired but returns an empty app list, allowing the user to pair again without manual config cleanup.
- App-list loading and launch transitions prime both display buffers to avoid stale host-list/loading frames during slow network work.
- Game-list fetch failure can fall back to a Desktop launch tile when the host app list is unavailable.

### Input
- Reworked the default PSP-to-Xbox mapping for full controller coverage:
  - L + Triangle/Cross/Square/Circle maps to right stick up/down/left/right.
  - L + D-pad Left/Right maps to LT/RT.
  - L + D-pad Down/Up maps to L3/R3.
- The Button Mapping UI now exposes the combo modifier, all virtual actions, and a right-stick source selector.
- Added an optional `modifier + analog nub` right-stick mode.
- Mapping files are versioned and legacy defaults migrate to the new v2 map.
- Browser mode now sends mouse/keyboard input only, with stronger analog mouse acceleration.
- App-owned combos such as HUD toggle and stream exit are consumed locally instead of also sending host input.

### Telemetry and HUD
- Added diagnostics telemetry for CPU, GPU, ME, RAM, media bandwidth, usable video bandwidth, audio bandwidth, local drops, loss, and FEC.
- Diagnostics telemetry updates once per second even when the HUD is hidden.
- HUD layout now places decode timing alongside latency and shows loss/FEC values from the RTP/FEC path.

### Low-Work PSP Path
- Performance preset disables local audio decode/playback by default to reduce PSP CPU, memory, and audio-thread work.
- Audio Disabled remains client-side and can be changed from the PSP settings menu.
- Video rendering applies the stream color/dither adjustment only to decoded video output, not to menu UI or HUD text.
- Retail builds compile out diagnostics-only logging, telemetry formatting, and debug sampling at call sites so public XMB builds do less PSP work.

### Build and Documentation
- Public docs updated for v1.2 presets, packet-size setup, pairing behavior, and controller mapping.
- Generated local build artifacts and internal audit notes are ignored so release diffs stay focused.
- Added `scripts/smoke_checks.sh` so `make smoke` validates release prerequisites and fails fast on unresolved merge markers.
- CI now validates `release/**` branches and uploads CI artifacts only; public release assets remain manual to preserve PSPSDK parity.

## [1.1.0] — 2026-05-06

### Networking
- Client-selected bitrate is now used directly for launch and transport bitrate setup (no implicit startup downscale).
- Connection quality classification now prioritizes transport-side loss/FEC recovery signals and no longer penalizes clean links for decode-side FPS stalls.
- RTCP Receiver Reports now use interval loss accounting and RFC3550-style jitter representation in RTP clock units.
- Adaptive bitrate fast-drop trigger now requires 3 consecutive drop signals before halving.

### Release
- Retail (non-dev) build artifacts are published manually for this release because CI uses a different PSPSDK and does not produce reproducible release binaries for this branch.

### Documentation
- Public docs refreshed for ARK-4-only CFW guidance.
- Installation guidance updated to remove risky flash-write install wording.
- Encoder setup instructions now present vendor-neutral CAVLC guidance for NVIDIA/AMD/Intel users.

---

## [1.0.0] — 2026-05-05

### Release Hardening
- Retail build mode is now the documented default for public packaging.
- Public docs were normalized from pre-release audit wording to release wording.
- CABAC compatibility documentation was clarified as normal-mode unsupported behavior with explicit CAVLC host guidance.

### Fixed (vs v0.2.3-beta)
- Pairing persistence now survives partial connection failures: successful pairing is saved even when launch/RTSP infrastructure fails later in the same attempt.
- RTSP/launch infrastructure failures now return a dedicated retryable status (`-3`) distinct from user cancel (`-2`) and pairing failure (`-1`).
- Settings UI now flushes controller state after OSK/button-mapping flows to prevent stale Start-edge saves on return.
- Config bootstrap now remembers default-loaded config when no file exists so first-session host/pairing additions persist correctly.

### Documentation
- Updated README status badges and version history for public release posture.

### Initial 1.0.0 publication (2026-05-04)

### Release
- Stable v1.0 public documentation set.
- Security, memory, reliability, and architecture hardening work from the beta cycle is now integrated in the release branch.

### Networking
- UPnP IGD hotspot/remote assist is included for RTP/RTCP UDP mapping setup and cleanup during streaming sessions.

### Documentation
- Updated README, install flow, and known-issues content for v1.0 user-facing guidance.

---

## [0.2.3-beta] — 2026-04-15

### Added — Host Discovery & Navigation
- **mDNS host discovery:** New `mdnsDiscoverHosts()` sends multicast queries for `_nvstream._tcp.local.` on 224.0.0.251:5353 with multicast group join and 2s listen window (resend at 1s). Near-instant LAN discovery replacing the need for manual IP entry only.
- **Quick subnet scan (Square button):** New `quickSubnetScan()` does sequential non-blocking TCP connect to port 47989 across the /24 subnet with 15ms timeout per host. Shows progress UI, interruptible via Circle. Manual-trigger only to avoid exhausting the PSP socket pool.
- **UPnP hotspot assist (remote mode):** Added IGD SSDP/SOAP support to request UDP port mappings for PSP video/audio RTP+RTCP ports during RTSP setup, with automatic cleanup on session close/failure. Improves remote/hotspot compatibility when streaming via public host IP.
- **Multi-host pairing (up to 8):** Replaced single `pairedHostIp[16]` with `pairedHostIps[8][16]` array + `pairedHostCount`. MRU ordering (slot 0 = most recent). New `config_is_host_paired()` and `config_add_paired_host()` APIs. Legacy `paired_host_ip` migrates to slot 0 on load.
- **Paired status display:** Host cards show "Paired" (green) / "Unpaired" (muted red) label for online hosts. Uses config-based paired-host list because plain HTTP `/serverinfo` can't verify the TLS client cert.
- **Back navigation (Host → Settings):** Circle button in host discovery returns to settings menu. WiFi state is checked first — if already connected (apctl state 4), the netconf dialog is skipped on re-entry.
- **Auto-resume same app:** When current game matches the target app, stream auto-resumes without the Resume/Quit popup. Different app still prompts. Saves ~12s on quit+relaunch of the same game.
- **Debug log config persistence:** New `debugLog` key in config.ini controls `g_debug_logging` flag.

### Changed — UI/UX Refinements
- **Smooth-scroll animation (host list):** Lerp-based camera scrolling with focus pop animation (selected host grows 2%). Scissor-clipped rendering. Matches settings menu animation system.
- **Unified 3-layer drop shadow + hover lift:** Consistent 3-layer shadow (alpha 0x18/0x28/0x38, all black) across all card UIs (host list, game grid, button mapping, placeholder/error). Selected cards get +1px shadow offset for hover lift effect.
- **Scaled text rendering:** New `ui_draw_text_scaled()` function. Selected items render at 0.50f scale, unselected at 0.45f — making the focused item visually larger.
- **Host card layout overhaul:** Two-row layout: name + IP on left, status + paired on right. Dynamic card sizing from focus pop. Thicker border (2px) on selected items. Rounded status dot (radius 5).
- **Bitrate preset selector:** Replaced linear 100 kbps stepping with codec-aligned presets: 64, 128, 256, 384, 512, 768, 1024, 1280, 1536, 2048, 2560 kbps.
- **Removed auto-coupling:** FPS and bitrate no longer auto-snap when resolution changes. Full manual control over all three settings.
- **Rounded placeholder icon and error modal:** Game placeholder uses rounded pill shapes; error modal border uses `ui_draw_hollow_rect_rounded`.
- **Footer hint updates:** Host list shows `{O}: Back`, game grid empty state shows refresh hint, settings shows `{X}: Edit` for custom FPS row.

### Changed — Performance & Streaming
- **IDR refresh interval: 60s → 5s:** More frequent IDR resets prevent P-frame drift during fast motion.
- **IDR backoff ceiling: 4s → 500ms:** Exponential backoff clears ghosting in ~3s (was ~6s).
- **FEC recovery threshold: 50% → 75%:** Even 25–50% partial parity gives Reed-Solomon a fair chance; error-concealed result beats a dropped frame.
- **Preemptive IDR on consecutive gaps:** After 2+ consecutive gap frames, fires a second preemptive IDR request.
- **Error concealment upgrade:** Changed from `ERROR_CON_SLICE_COPY` to `ERROR_CON_FRAME_COPY_CROSS_IDR` — copies entire previous frame on error, even across IDR boundaries.
- **Deblocking filter re-enabled:** Removed `PSP_SKIP_DEBLOCKING` compile flag. At sub-native resolutions (256×144, 368×208) the CPU cost (~2–3ms/frame) is acceptable, and it removes blocking artifacts at low bitrates.
- **Bitrate recovery doubled:** `ADAPT_SLOW_RECOVER_KBPS` increased from 25 to 50 kbps/s for faster recovery after signal drops.
- **Minimum bitrate floor: 100 → 32 kbps:** Allows deeper adaptive reduction on poor WiFi.
- **Default bitrate: 500 → 384 kbps:** WiFi-safe default that balances quality and reliability.
- **Quick relaunch (skip host re-probe):** All exit-to-menu paths keep the cached host list. No re-discovery unless the user explicitly presses Square or returns through settings.

### Fixed
- **Stale pairing cleanup (401 handling):** Three separate 401-response handlers now remove the specific host IP from the paired array (shift + decrement) instead of just clearing a single field.
- **User-cancel connect:** Cancelling a connection immediately stops further retries.
- **WiFi re-prompt on back-nav:** Checks `sceNetApctlGetState()` — skips netconf dialog if WiFi is already connected (state 4).
- **PairStatus trust from HTTP probe:** After host selection, if `selected_host->paired` is true, sets `g_is_paired = 1` — trusts probe status rather than config-only.
- **Offline hosts no longer show "Unpaired":** Paired/Unpaired label hidden for offline hosts (status 0) since they can't be verified.
- **Back from game list error instant return:** Skip_rescan set for all connection failure returns, so pressing back from "Failed to Load Games" instantly shows the cached host list.

### Build
- **`RETAIL_BUILD` flag:** Added `-DRETAIL_BUILD` to CFLAGS and CXXFLAGS, suppressing non-FATAL diagnostic logging for release builds.

### Performance (368×208 @ 15fps, 384 kbps)
| Metric | v0.2.2-beta | v0.2.3-beta |
|---|---|---|
| Host discovery | Manual IP entry | mDNS + subnet scan |
| Paired host tracking | 1 host | 8 hosts (MRU) |
| IDR backoff ceiling | 4000ms | 500ms |
| FEC recovery threshold | 50% parity | 75% parity |
| Deblocking filter | Disabled | Re-enabled |
| Default bitrate | 500 kbps | 384 kbps |
| Min adaptive bitrate | 100 kbps | 32 kbps |
| Quit+relaunch same app | ~15–24s (2 loading screens) | ~2–4s (auto-resume) |
| UI shadows | 1-layer accent glow | 3-layer black + hover lift |

---

## [0.2.2-beta] — 2026-04-14

### Added — New Protocol Features
- **Keyboard event support:** Sends keyboard key-down/key-up packets (Type 5) to host. Enables text input in streamed applications via PSP combo triggers.
- **Scroll event support:** Sends mouse wheel scroll packets — both Gen5 (Type 0x09) and high-resolution (Type 0x33) — for scrolling in browser mode and menus.
- **Controller arrival announcement:** Sends Type 0x37 packet announcing PSP as an Xbox controller with analog trigger capability and supported button flags.
- **Controller battery reporting:** Sends Type 0x40 packets with PSP battery percentage and charge state (discharging/charging/full) to host, displayed in Steam overlay.
- **RTCP receiver reports:** Network layer now sends RTCP RR feedback packets to server for bidirectional quality negotiation.
- **Dynamic resolution scaling:** New `stream_resolution.c` — auto-scales between 4 resolution steps (256×144 → 320×192 → 368×208 → 480×272) based on EMA-smoothed decode time and packet loss with hold-off timers to prevent thrashing.
- **Protocol comparison documentation:** Two new docs (`PROTOCOL_COMPARISON.md`, `FULL_PROTOCOL_COMPARISON.md`) mapping PSP Moonlight features against the Moonlight desktop protocol spec.

### Added — Adaptive Quality System (Phase 4–5)
- **PID-based adaptive bitrate controller:** Replaces simple RSSI threshold with composite quality score: 40% RSSI + 30% connection quality + 30% FEC recovery rate. Full PID controller (Kp/Ki/Kd) with anti-windup integral clamping and dead-zone oscillation prevention.
- **IDR exponential backoff:** IDR requests now use 500ms → 1000ms → 2000ms → 4000ms backoff with automatic cooldown resets. Reduces IDR flood during sustained loss from hundreds to single digits.
- **Quality hysteresis:** Requires 3 consecutive readings in same quality band before state transition, preventing oscillation on borderline WiFi.
- **FEC predictive loss detection:** Detects WiFi burst loss patterns and pre-requests IDR frames before unrecoverable frame arrives. Selective FEC skip when >50% parity lost saves CPU.
- **Packet prioritization:** IDR/SOF packets preferred over P-frame body and FEC during congestion for faster keyframe delivery.
- **Dynamic SO_RCVBUF:** Socket receive buffer scales with quality — 128KB (good) → 256KB (fair) → 384KB (poor) — to absorb WiFi jitter bursts.
- **WiFi power save disable:** Disables PSP WiFi power save mode during active streaming to eliminate 100ms+ wakeup latency spikes.
- **Aggressive ping + burst recovery:** Faster ping interval for connection monitoring; burst ping (multiple sends × 1ms spacing) on reconnect for faster recovery.

### Changed — Audio
- **Quality-adaptive PLC thresholds:** PLC gap tolerance now adjusts with connection quality — 35ms (good) / 45ms (fair) / 60ms (poor) — reducing false-positive PLC during degraded WiFi.
- **Dynamic audio ring depth:** Ring buffer scales with packet loss rate to absorb longer burst gaps without underrun.
- **Audio crypto separate error tracking:** Audio crypto failures tracked independently with 100-failure threshold before fatal disconnect, preventing video crypto issues from killing audio.

### Changed — Video Decode
- **Frame pacing:** Extra VBlank wait if frame decoded <4ms ago to reduce tearing on fast-motion scenes.
- **P-frame skip-ahead:** When decoder backlog exceeds 256 packets, scans forward to next IDR instead of decoding 85+ stale P-frames. Dramatically reduces recovery time after loss bursts.
- **Decoder thread priority boost:** Thread priority elevated from 0x1C to 0x18, matching control stream priority for lower decode latency.
- **Enhanced watchdog:** Credit restoration now weighted by FEC recovery rate (600/900/1200 frames). Intermediate flush at 3s before Mode B 5s timeout for graceful recovery.

### Changed — UI
- **HUD dynamic height:** HUD overlay auto-sizes based on displayed metrics instead of fixed height.
- **HUD new metrics:** Added packet loss %, FEC recovery %, and host processing latency display to in-stream HUD.
- **Settings menu:** Resolution/FPS selector improvements for custom resolution support.
- **Game grid UI:** Improved tile layout and icon rendering.

### Fixed
- **Config resolution index:** Custom resolutions (anything not 480×272 or 256×144) now correctly map to `resolutionIndex=2` instead of defaulting to 0.
- **Config FPS index rebuilder:** FPS values correctly map to `FPS_VALUES[]` array indices on INI reload, preventing mismatched FPS after config changes.
- **RTP reassembly stats:** Additional sequence gap tracking for diagnostic accuracy.
- **Stream connect UI:** Minor connection progress display fix.
- **Pairing PIN UI:** Layout and interaction improvements.

### Performance (480×272 @ 15fps, 500 kbps)
| Metric | v0.2.1-beta | v0.2.2-beta |
|---|---|---|
| Adaptive bitrate | Threshold-based | PID controller (composite quality) |
| IDR requests/min | ~40–100 | ~5–15 (exp backoff) |
| FEC recovery | Basic RS | Predictive + selective skip |
| Audio PLC | Fixed 45ms | Adaptive 35–60ms |
| Input types | Gamepad only | Gamepad + keyboard + scroll + battery |
| Resolution modes | Fixed | Auto-scale 4-step ladder |
| HUD metrics | FPS, latency | +loss%, FEC%, battery, host latency |
| Feature coverage | — | Expanded and stabilized during v0.2.x cycle |

---

## [0.2.1-beta] — 2026-04-13

### Added — New Features
- **Audio streaming (first time enabled):** Full Opus 48kHz stereo decode with AES-CBC decryption, PKCS#7 padding, FEC recovery, and Packet Loss Concealment (PLC). Audio is now on by default with an option to disable in the settings menu.
- **PLC volume ducking:** Consecutive PLC frames are progressively attenuated (87.5% → 68.75% → 50%) to mask WiFi-loss-induced static artifacts. Uses fixed-point integer math for PSP-safe operation.
- **Custom button mapping UI:** Interactive in-app menu to remap L2, R2, right stick axes, L3, and R3 to any PSP button/combo. Settings persist to config file.
- **WiFi keepalive during streaming:** Network keepalive thread now stays active during stream sessions (was idle-only). Monitors WiFi state every 3s and prevents server-side stream stall from idle timeout.
- **FEC piggyback acceleration:** Control stream FEC piggyback frequency doubled (every 5th ping → 2× per second), stall advance rate effectively doubled with cap raised 3600→7200.
- **Audio enable/disable toggle:** New settings menu option to enable/disable audio decode. When disabled, audio thread is not started and all audio packets are silently consumed.
- **OpenH264 third-party library:** Added `third_party/openh264/` with PSP-specific Makefile, decoder-only static build, deblocking disabled compile flag (`PSP_SKIP_DEBLOCKING`).

### Changed — Decoder Replacement
- **Replaced FFmpeg with OpenH264:** Switched H.264 decoder from FFmpeg libavcodec to a custom PSP port of OpenH264. Benefits: smaller code footprint, CABAC/CAVLC support, faster error concealment, no GPL dependency on FFmpeg. Note: CABAC is technically supported but only works ~25% of the time on PSP hardware (causes stalls and rubber-banding) — **CAVLC is strongly recommended**. Old `ffmpeg_decode.c` moved to `legacy/`.
- **Decoder pipeline:** New `openh264_decode.cpp` handles OpenH264 decode + ME YUV→RGBA dispatch with spin-wait ME completion (500K iterations, yield threshold 4).
- **Deblocking filter disabled:** Compile-time `PSP_SKIP_DEBLOCKING` flag skips deblocking for ~15% decode speed improvement on PSP hardware.
- **Decoder thread optimizations:** Batch decode size 512, ring threshold 512, semaphore timeout 500ms, optimized for throughput on PSP-1000.

### Changed — UI Overhaul
- **Settings menu revamp:** Added audio toggle, button mapping entry point, resolution/FPS selector, theme picker. Settings save to MS0 config file.
- **Game grid UI:** Improved tile layout, icon rendering, visual polish.
- **HUD overlay:** Updated FPS/latency display, Settings/Pause/Quit in-stream menu overlay.
- **OSK input improvements:** Enhanced on-screen keyboard for IP/PIN entry with better cursor and validation.
- **Exit dialog:** Improved stream exit confirmation dialog.

### Changed — Network & Streaming
- **Audio PLC threshold:** Increased from 25ms to 45ms (2.25× frame interval) to reduce false-positive PLC triggers that caused unnecessary static.
- **Control stream:** Enhanced IDR request logic with rate limiting (1/sec after initial 5 rapid-fire), periodic IDR refresh every 60s, stall detection at 5s/10s.
- **Thread priorities optimized:** net_recv=0x12, ctrl=0x18, audio=0x1A, decoder=0x1C, update=0x20, keepalive=0x30. Ensures network receives are highest priority.
- **ME spin-wait tuning:** Reduced from 5M to 500K iterations, yield threshold from 64 to 4, for better CPU utilization.

### Fixed
- **Server-side stream stall:** WiFi keepalive now active during streaming prevents Sunshine from timing out the session. Sessions now run the full requested duration without server-initiated stop.
- **IDR flood:** Three-part fix across RTP FEC, reassembly, and control stream reduces IDR requests from 66+ per session to near-zero during normal operation.
- **Audio underruns:** Progressive optimization from 31% to 0% underrun rate through priority tuning, polling backoff, ring buffer management, and FEC recovery.
- **Input forwarding reliability:** Per-channel reliable sequence numbers ensure all button presses register on the host.
- **ME data cache coherence:** Proper dcache flush before/after ME dispatch eliminates corrupted YUV→RGBA output.

### Performance (480×272 @ 15fps, 500 kbps)
| Metric | v0.2.0-beta | v0.2.1-beta |
|---|---|---|
| Decoder | FFmpeg libavcodec | OpenH264 (smaller, CAVLC recommended) |
| Audio | Decode-only, no playback | Full playback with PLC |
| FPS (480×272) | 15–18 fps | 10–18 fps |
| FPS (256×144@30fps) | N/A | 17 fps |
| Audio underruns | N/A | 0.0–0.5% |
| WiFi stability | Server stalled at ~35s | Full 120s+ sessions |
| Watchdog restarts | Frequent | 0 |
| Button mapping | Fixed L+D-pad | Customizable |

---

## [0.2.0-beta] — 2026-04-11 — First Public Release

**This is the first public beta** of the PSP-native Moonlight client, with host
discovery, pairing, video streaming, audio and controller support.

### What works
- Wi-Fi connect + host discovery (LAN mDNS probe + manual IP entry)
- TLS 1.2 pairing with Sunshine (RSA + ECDH + AES-GCM, 5-step protocol)
- Game library fetch + box art icon cache (ms0:)
- RTSP session setup (custom, Sunshine Gen7, no moonlight-common-c)
- UDP video receive + Reed-Solomon FEC (up to 66% parity)
- H.264 Baseline decode (FFmpeg libavcodec, CAVLC only)
- VFPU YUV→RGBA on Media Engine (~31 µs/frame, >99.9% success)
- Opus stereo audio decode (48 kHz, fixed-point Silk+CELT)
- Controller input forwarding (all PSP buttons mapped, L+D-pad virtual L2/R2)
- Dual-mode watchdog with auto-restart (ME hang recovery, RTP stall recovery)
- HUD overlay, signal strength monitor, safety buffer, power switch suspend

### Known gaps
- No deblocking filter (ME bandwidth constraint)
- Single-buffered display (tearing on fast motion)
- No 30 fps support yet (overhead ceiling at ~18 fps on PSP-1000 at 500 kbps)

---

## Earlier development versions

- 0.3.0-alpha: introduced the FFmpeg dual-core video pipeline and unified stream resolution handling.
- 0.2.0-alpha: corrected per-channel control sequencing.
- 0.1.5-alpha: improved reference-frame recovery and reduced unnecessary keyframe requests.
- 0.1.0-alpha: introduced the custom CAVLC decoder and Media Engine reconstruction pipeline.
- moonlight-psp-core: earlier project archived at [k4idyn/Moonlight-PSP](https://github.com/k4idyn/Moonlight-PSP).
