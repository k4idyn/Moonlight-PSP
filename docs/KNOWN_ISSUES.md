# Known Issues

_PSP Moonlight v0.2.1-beta_

This document tracks confirmed bugs, hardware limitations, and fixed issues. All issues listed under "Fixed" were resolved during internal development and are included in v0.2.1-beta.

---

## Critical / Active Blockers

### Deblocking filter disabled
**Status:** Compile-time disabled (`PSP_SKIP_DEBLOCKING`)  
**Impact:** At bitrates below ~500 kbps, decoded frames show visible 8×8 block boundaries. Deblocking is disabled via compile flag for ~15% decode speed improvement. The H.264 spec treats deblocking as optional for Baseline Profile. Workaround: use ≥500 kbps.

### No display double-buffering
**Status:** Not implemented  
**Impact:** Tearing visible during fast motion. The GE/GU display pipeline writes directly to the active framebuffer. Double-buffering requires allocating a second buffer and flipping on vsync, which interacts with the ME output pipeline.

### 30 fps ceiling (network-bound)
**Status:** By design at current architecture  
**Impact:** RTP/FEC/reassembly processing overhead on the main CPU limits practical sustained frame rate to ~15–18 fps on PSP-1000 at 500 kbps. Internal decode speed (OpenH264 + VFPU) can sustain 40+ fps; the bottleneck is network processing.  
**Run 073 data:** At 30 fps target, actual delivery was 7.8 fps with constant ring overflow (1023/1024). Decoder itself capable of 40.6 fps.  
**Mitigation:** Lower resolution (368×208) reduces RTP packet count and improves network throughput.

---

## Known Hardware Quirks

### `mfv` returns 0 on ME core
Reading a VFPU scalar register into a MIPS GP register via `mfv` always returns 0 on the Media Engine. This is a PSP-specific hardware bug not documented in Sony's official SDK. Workaround: extract VFPU data via `sv.s`/`sv.q` to cached memory, then `lw`.

### `vi2uc.q` wrong output on real PSP
The `vi2uc.q` instruction (vector int-to-unsigned char pack) does `val >> 23` before byte packing on real hardware, producing garbage for normal float ranges. Not usable for YUV→RGBA on hardware. Workaround: use `sv.s` stores + manual byte packing.

### ME crash on uncached addresses
Passing `0x48xxxxxx` (uncached alias) to ME code causes a bus error and ME halt. Always use cached addresses (`0x00xxxxxx`) for ME data. Verified repeatedly across ME development.

### WiFi degradation during active streaming
802.11b packet loss increases over time during streaming sessions, particularly after 40–45 seconds of continuous RTP traffic. Observed consistently across test runs. FEC absorbs most loss but burst losses trigger IDR recovery cycles. Use a strong 2.4 GHz signal or 802.11g.

### Audio PLC static at high packet loss
**Status:** Partially mitigated  
**Impact:** When WiFi drops >3 consecutive audio packets, PLC (Packet Loss Concealment) generates synthetic audio that sounds like static. Volume ducking (87.5% → 68.75% → 50%) reduces but doesn't eliminate the artifact. At ~37% WiFi packet loss (typical for 802.11b), PLC generates ~14% of audio timeline as synthetic frames.  
**Mitigation:** PLC threshold increased from 25ms to 45ms to reduce false positives. Volume ducking fades PLC output. Further improvement would require increasing `packetDuration` from 20ms to 40ms (halving packet count) but requires buffer resize.

---

## Fixed (included in v0.2.1-beta)

### Server-side stream stall — Fixed in v0.2.1-beta
WiFi keepalive was disabled during streaming, causing Sunshine to time out and stop sending video after ~35 seconds. Fixed by keeping the keepalive thread active during streaming with 3-second check intervals.

### Audio underruns (31% → 0%) — Fixed in v0.2.1-beta
Audio playback thread starved due to low priority and inefficient polling. Fixed through progressive optimization: thread priority tuning (0x1A), polling backoff (500µs/1ms/2ms), ring buffer management (64 slots), and FEC recovery. Final rate: 0.0–0.5% underruns.

### IDR flood during streaming — Fixed in v0.2.1-beta
Three-part fix: (1) RTP FEC unrecoverable drops check `g_refs_corrupted` before requesting IDR, (2) RTP reassembly seq-gap frames set corruption flag, (3) control stream rate-limits IDR requests to 1/sec after initial 5 rapid-fire. Reduced IDR requests from 66+ to near-zero.

## Fixed (included in v0.2.0-beta)

### Display freeze after queue overrun (run 070) — Fixed in internal 0.3.0-alpha
Queue overrun handler flushed RTP state but not OpenH264's internal codec context. The decoder entered a permanent error state, triggering an infinite overrun loop that destroyed each arriving IDR before reassembly completed. Fixed by calling `oh264_pipeline_flush_buffers()` (which flushes the decoder and clears ME state) from the overrun handler.

### ME permanent disable after first crash — Fixed in internal 0.3.0-alpha
ME was permanently disabled after the first timeout. Replaced with KillME+reinit+retry (up to 3 recoveries). 11/11 ME timeouts recovered in extended testing.

### ME bus error at 640×360 — Fixed in internal 0.3.0-alpha
ME YUV conversion used uncached buffer pointers. At 640×360 (1.7 MB/frame uncached traffic), the ME bus saturated and crashed. Fixed by switching to cached addresses with dcache flush protocol.

### ENet per-channel sequence desync (all input dropped) — Fixed in internal 0.2.0-alpha
Two shared reliable sequence counters (one for channel 0x01, one shared for all others) caused channel 0x10 (input) to arrive with seq 200+ when server expected seq 1 on that channel. Server buffered packet forever. All button input silently dropped. Fixed with per-channel array.

### Green/magenta artifact frames (VQ#5b) — Fixed in internal 0.1.5-alpha
RS failures and sequence gaps did not set `g_refs_corrupted`. P-frames decoded against corrupted DPB produced horizontal banding. Fixed by gating `g_refs_corrupted` on both RS failures and seq gaps.

### IDR flooding 802.11b (123 IDR requests, 0 delivered) — Fixed in internal 0.1.5-alpha
Each unrecoverable FEC drop set `g_idr_fully_decoded = 0`, triggering full IDR requests every 5s. IDRs at 480×272 are 17–20 packets (~23 KB) — too large to survive 802.11b burst loss. Fixed with RFI (Reference Frame Invalidation) for the non-corrupted case.

---

## Won't Fix / Out of Scope

- **PSP Go support:** Different Wi-Fi hardware. Not a target platform.
- **H.265/AV1 decode:** PSP hardware cannot decode these; far beyond CPU capability in software.
- **NVIDIA GameStream backend:** Untested; Sunshine is the supported host. GameStream is deprecated.
- **Encrypted audio channel:** Audio stream received unencrypted in current sessions; no change planned.
