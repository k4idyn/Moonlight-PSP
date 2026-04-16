# Changelog

All notable changes to PSP Moonlight are documented here.

v0.2.0-beta is the first public release of this codebase. For the history of the original
`moonlight-psp-core` project (the prior public alpha that used `sceMpeg` + `moonlight-common-c`),
see the [archived repo](https://github.com/k4idyn/Moonlight-PSP). Nothing from that codebase
carries into this one — it's a clean-sheet rewrite. The internal dev milestones below were
never published; they're documented here so the commit history makes sense.

---

## [0.2.3-beta] — 2026-04-15

### Added — Host Discovery & Navigation
- **mDNS host discovery:** New `mdnsDiscoverHosts()` sends multicast queries for `_nvstream._tcp.local.` on 224.0.0.251:5353 with multicast group join and 2s listen window (resend at 1s). Near-instant LAN discovery replacing the need for manual IP entry only.
- **Quick subnet scan (Square button):** New `quickSubnetScan()` does sequential non-blocking TCP connect to port 47989 across the /24 subnet with 15ms timeout per host. Shows progress UI, interruptible via Circle. Manual-trigger only to avoid exhausting the PSP socket pool.
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
- **User-cancel connect: no retry:** ret == -2 (user cancelled) now breaks out of the connection retry loop immediately instead of retrying.
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
| Features verified | — | 31/39 ACTIVE across 8-stage test |

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

**This is the first public beta.** The pipeline is proven on real PSP-1000 hardware at
15–18 fps with zero visual artifacts over 3-minute sessions. The architecture is stable
enough to publish for broader hardware testing.

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

## [INTERNAL — 0.3.0-alpha] — 2026-04-11

> *Never published. Internal dev checkpoint.*

### Architecture — FFmpeg Dual-Core Pipeline

Replaced hand-rolled CAVLC+VFPU orchestrated path with **FFmpeg libavcodec** as the H.264
frontend. The ME is retained solely for YUV→RGBA, running concurrently with FFmpeg decode.

#### Added
- `ffmpeg_decode.c` — FFmpeg H.264 decode + ME YUV→RGBA dispatch
  - Double-buffered AVFrame pool (zero-copy ME dispatch)
  - Dual-mode watchdog: Mode A (FFmpeg CPU hang >500ms), Mode B (RTP stall >3s)
  - Force-restart: `sceKernelTerminateThread` + `ffmpeg_pipeline_abandon()` + reinit + new thread
  - Early-skip gate: unconditional non-IDR drop when `g_refs_corrupted == 1` (zero visual artifacts)
  - `ffmpeg_pipeline_flush_buffers()` — flushes FFmpeg AVCodec + clears ME state
  - `ffmpeg_pipeline_abandon()` — nulls globals without free for crash-safe recovery
- `stream_resolution.c` / `stream_resolution.h` — unified resolution table; eliminates hardcoded 480/272 scattered across decode, display, and buffer alloc
- `decode_flags.h` — shared decode state flags and watchdog counter declarations
- `sw_decoder_thread.c` — complete rewrite: dual-mode watchdog, force-restart, ring backlog safety net

#### Fixed
- **Display freeze (run 070):** Queue overrun handler flushed RTP state but not FFmpeg internal state. `avcodec_receive_frame` stuck in permanent `EAGAIN`, destroying arriving IDR packets. Fix: `ffmpeg_pipeline_flush_buffers()` now called from overrun handler.
- **ME data cache bug:** Switched ME buffer pointers from uncached (`0x48xxxxxx`) to cached addresses with pre/post dcache flush. YUV→RGBA went from 47,000 µs → 31 µs (1,500× improvement).
- **ME crash recovery:** Single-shot ME disable replaced with KillME+reinit retry (up to 3 recoveries).

#### Performance (500 kbps @ 15 fps target)
- **17.9 fps** sustained
- **25/25 screenshots clean**
- **Full 180s** — no stalls, no freezes

---

## [INTERNAL — 0.2.0-alpha] — 2026-04-08

> *Never published. Internal dev checkpoint.*

### ENet Per-Channel Sequence Bug Fix

- `control_stream.c`: Replaced two shared reliable sequence counters with `reliable_seq_per_ch[CTRL_CHANNEL_COUNT]` array (48 entries). Each channel now starts at seq 1.
  - **Root cause:** Pings on channel 0x00 and input on channel 0x10 shared a counter. Server expected seq #1 on channel 0x10; counter was at 200+ when first input packet arrived. All input silently dropped, no errors logged.
  - **Symptom:** `[INP] Button transition` confirmed in logs, correct packets sent, zero effect on host. All 11 screenshots were identical.

---

## [INTERNAL — 0.1.5-alpha] — 2026-04-07

> *Never published. Internal dev checkpoint.*

### RTP/FEC Corruption Gating

- `rtp_fec.c` RS failure path now sets `g_refs_corrupted=1` (previously skipped)
- `rtp_reassembly.c` seq-gap frames now set `g_refs_corrupted=1` (previously only dropped)
- **Result:** 100% clean frames (25/25) vs 56% (prior run). Zero visual artifacts.

### IDR Flooding Fix

- `rtp_fec.c` unrecoverable drop path now checks `g_refs_corrupted` before requesting IDR:
  - `== 0` → use RFI (Reference Frame Invalidation) — DPB intact, ask encoder for partial intra
  - `!= 0` → request full IDR
  - **Result:** IDR requests dropped 98% (123 → 2 per session); RFI handles the rest

---

## [INTERNAL — 0.1.0-alpha] — 2026-04-05

> *Never published. First hardware test checkpoint.*

### Overview

First end-to-end streaming confirmation on real PSP-1000 hardware. Custom asymmetric
dual-core CAVLC+VFPU pipeline. Baseline 15+ fps sustained streaming confirmed.

### Dual-Core CAVLC + VFPU Pipeline (original)
- `sw_cavlc.c` — H.264 Baseline CAVLC entropy decoder (CPU)
  - Full SPS/PPS, I16×16, I4×4, P-frame, P_SKIP
  - Exp-Golomb + Golomb-Rice coefficient parsing, emulation prevention byte removal
- `sw_vfpu_recon.c` — VFPU-accelerated reconstruction (ME)
  - 45-instruction vectorised 4×4 IDCT, zero scalar loops
  - Hadamard 4×4 butterfly (spec-correct order)
  - All 9 I4×4 intra prediction modes
  - Inter prediction with half-pixel luma interpolation
  - P_SKIP optimisation: reference plane memcpy bypass (~95% MB skip for static scenes)
- `sw_me_worker.c` — ME job worker, semaphore sync (`BeginME`/`WaitME`)
- `sw_decode_orchestrator.c` — CAVLC→ME handoff coordinator

---

## [moonlight-psp-core] — Public Alpha Archive (separate repo)

> Prior project: [github.com/k4idyn/Moonlight-PSP](https://github.com/k4idyn/Moonlight-PSP)
> Architecture: `moonlight-common-c` + `sceMpegAvcDecode` + ENet + Opus + mbedTLS + MXML
> Status: Abandoned. Never decoded a frame. Final open issue: `0x80` ENOTCONN on control stream.

### v0.1.0.3-alpha — Architectural Cleanup
- Header guard standardization (`MOONLIGHT_*_H` convention) applied project-wide
- XML parsing layer in `xml.c` refactored to use public Mini-XML 3.x accessor APIs (ABI safety)
- Toolchain: replaced hardcoded Windows drive paths with dynamic `psp-config --pspsdk-path`
- Makefile library link order fixed for `psp-fixup-imports` requirements
- Verified: PPSSPP network init + pairing PIN generation successful; `EBOOT.PBP` = 1,794,994 bytes

### v0.1.0.2-alpha — Architectural Stabilization
- **Root cause identified:** `0x80020320` (too many open files) error during pairing/certificate load
  - `libgamestream` HTTP used POSIX `close()` on sockets; PSP requires `closeSocket()` (`sceNetInetClose`)
  - High-frequency logger (50+ open/write/close cycles/sec) overwhelmed the PSP FAT driver's async close queue, leaking handles until the kernel limit was hit
- `logger.c` rewritten with a single persistent global file handle opened at boot
- `http.c` and TLS cleanup macros patched to use `closeSocket()` throughout
- Known issue remaining: `0x80` (ENOTCONN) — connection to control stream ends prematurely; suspected ENet handshake failure or Sunshine server rejection

### v0.1.0.1-alpha — Standalone Build Verification
- First fully standalone build: all dependencies (mbedTLS, ENet, Opus, MXML) bundled in-repo
- No manual pre-compiled dependency step required
- Clean-room build on Windows verified with PSPSDK toolchain

### v0.1.0-alpha — Initial Alpha Build
- Initial alpha release of `moonlight-psp-core`
- Functional but in active debugging phase at time of release

---

## [0.3.0-alpha] — 2026-04-11

### Architecture: FFmpeg Dual-Core Pipeline (Full Replacement)

Replaced the hand-rolled CAVLC+VFPU orchestrated pipeline with **FFmpeg libavcodec** as the H.264 decode frontend. The ME is retained for YUV→RGBA conversion, running concurrently with FFmpeg decode on the main CPU.

#### Added
- **`ffmpeg_decode.c`** — FFmpeg libavcodec H.264 decode + ME YUV→RGBA dispatch
  - Double-buffered AVFrame pool (zero-copy ME dispatch)
  - Dual-mode watchdog: Mode A (FFmpeg CPU hang >3s), Mode B (RTP stall >5s)
  - Force-restart sequence with ME re-init and ring/counter reset
  - Watchdog credit restoration (1 restart slot restored per 15s clean window)
  - Early-skip: unconditional non-IDR skip when `g_refs_corrupted` set (zero visual artifacts)
  - Adaptive `me_stressed` detection (>500ms ME decode → stress flag)
  - `ffmpeg_pipeline_flush_buffers()` — flushes FFmpeg AVCodec + clears ME state
  - `ffmpeg_pipeline_abandon()` — nulls globals without free for crash recovery
- **`stream_resolution.c`** / **`stream_resolution.h`** — unified resolution table; eliminates hardcoded 480/272 scattered across decode, display, and buffer allocation
- **`decode_flags.h`** — shared decode state flags and watchdog counter declarations
- `sw_decoder_thread.c` — complete rewrite: dual-mode watchdog, force-restart, ring backlog safety net, static-local reset via `g_decode_counters_reset_pending`

#### Fixed
- **Display freeze bug (run 070):** Queue overrun handler flushed RTP state but not FFmpeg internal state. `avcodec_receive_frame` permanently returned `EAGAIN`, causing infinite overrun loop that destroyed arriving IDR packets. Fix: `ffmpeg_pipeline_flush_buffers()` called from overrun handler.
- **ME crash recovery:** Permanent ME disable after first crash replaced with KillME+reinit retry (up to 3 recoveries). 11/11 ME timeouts recovered in testing.
- **ME data cache bug:** Switched ME buffer pointers from uncached (`0x48xxxxxx`) to cached addresses with pre/post dcache flush. Eliminated bus-error ME crashes at 640×360. YUV→RGBA: 47,000 µs → 31 µs (1500× faster).
- **Resume/quit dialog** re-enabled (`prompt_existing_session_action()`, `/resume` endpoint).
- **Connection retry:** Auto-retry RTSP launch once after 2s on failure.

#### Performance (VQ#16 — 500 kbps @ 15 fps)
- **17.9 fps** sustained (exceeds 15 fps target)
- **25/25 screenshots clean** — zero artifacts
- **Full 180s** — no stalls, no freezes
- ME decode: 19–31 ms/frame (32–51.5 fps capable on ME alone)

---

## [0.2.0-alpha] — 2026-04-08

### ENet Per-Channel Sequence Bug Fix

- **`control_stream.c`:** Replaced two shared reliable sequence counters with `reliable_seq_per_ch[CTRL_CHANNEL_COUNT]` array (48 entries). Each channel now starts at seq 1.
  - **Root cause:** Pings on channel 0x00 and input on channel 0x10 shared a counter. By the time the first input packet was sent, the shared counter was at 200+. Server expected seq #1 on channel 0x10, buffered packet forever. All input silently dropped.
  - **Symptom:** `[INP] Button transition` in logs, no `Send FAILED`, but all 11 screenshots identical.

---

## [0.1.5-alpha] — 2026-04-07

### RTP/FEC Corruption Gating

- **`rtp_fec.c`:** RS failure path sets `g_refs_corrupted=1` (previously skipped)
- **`rtp_reassembly.c`:** Seq-gap frames set `g_refs_corrupted=1` (previously only dropped)
- **Result (VQ#6):** 100% clean frames (25/25) vs 56% clean (VQ#5b). Zero visual artifacts.

### IDR Flooding Fix

- **`rtp_fec.c`:** Unrecoverable drop path now checks `g_refs_corrupted` before requesting IDR:
  - `g_refs_corrupted == 0` → use lightweight RFI (Reference Frame Invalidation) — DPB intact
  - `g_refs_corrupted != 0` → request IDR (legitimate corruption)
  - `g_last_good_frame == 0` → request IDR (no reference)
  - **Result:** IDR requests dropped 98% (123 → 2 per session); RFI requests replaced wasteful IDRs

---

## [0.1.0-alpha] — 2026-04-05 (first hardware test checkpoint)

### Overview

First hardware test checkpoint. Custom asymmetric dual-core CAVLC+VFPU pipeline. Confirmed working for 15+ fps sustained streaming on real PSP-1000 hardware.

#### Dual-Core CAVLC + VFPU Pipeline (original)
- **`sw_cavlc.c`** — H.264 Baseline CAVLC entropy decoder (CPU)
  - Full SPS/PPS, I16×16, I4×4, P-frame, P_SKIP
  - Exp-Golomb + Golomb-Rice coefficient parsing
  - Emulation prevention byte removal
- **`sw_vfpu_recon.c`** — VFPU-accelerated reconstruction (ME)
  - Fully vectorised 4×4 IDCT: 45 VFPU instructions, zero scalar loops
  - Hadamard 4×4 butterfly (spec-correct order)
  - All 9 I4×4 intra prediction modes
  - Inter prediction with half-pixel luma interpolation
  - P_SKIP optimisation: reference plane memcpy bypass (~95% MB skip for static scenes)
- **`sw_me_worker.c`** — ME job worker with semaphore sync (`BeginME`/`WaitME`)
- **`sw_decode_orchestrator.c`** — CAVLC→ME handoff coordinator
- **`sw_decoder_thread.c`** — Decoder thread entry point

#### H.264 Bug Fixes (found during VFPU integration)
- **Hadamard butterfly permutation:** Outputs 1/2/3 were cyclically wrong; corrected to spec `z0±z3, z1±z2`
- **I16×16 DC double-dequant:** DC inserted before `dequant_4x4_vfpu` caused double-scaling
- **Chroma DC double-dequant:** Same bug in P-frame and intra chroma paths
- **Chroma cbp==1 IDCT skip:** DC residual only reached pixel (0,0); fix: always IDCT when DC present

#### USB Stability Fix (run 041)
- PSPLink keepalive loop replaces `Start-Sleep` during stream
- `-NoKill` flag on `Invoke-PspSh` prevents VRAM overlay on timeout
- Increased yields: 1.5ms/frame + 0.5ms/batch + priority 0x20

#### Networking & Protocol (initial)
- Moonlight Generation 7 (clientVersion 19)
- Encrypted RTSP (AES-GCM)
- Sunshine 8-byte frame header skip
- 1024-slot UDP packet ring (1500 B slots)
- IDR request burst (0x0302, 3×) on queue overrun
- Reed-Solomon FEC (up to 66% parity)
- Opus stereo (48 kHz, fixed-point Silk+CELT)

#### VFPU Hardware Notes Discovered
- `mfv` (VFPU→GP) always returns 0 on ME core — use `sv.s`/`sv.q` instead
- `vi2uc.q` produces wrong output on real PSP — not usable for YUV→RGBA
- Uncached ME addresses crash the ME — use cached aliases only
- PS2 VU0 instructions (`vftoi0`, `vmad.q`) do not exist on PSP VFPU

---

## [0.0.1-archive] — prior attempt (moonlight-common-c + sceMpeg)

Abandoned. Used `moonlight-common-c`, ENet, `libgamestream`, and `sceMpegAvcDecode`. The `sceMpeg` ringbuffer requires MPEG-PS framing; incompatible with the Moonlight RTP stream model. Threading model conflicts between ENet and PSP fixed-stack kernel threads caused additional instability. Archived in `Clean Build Old Github`.
