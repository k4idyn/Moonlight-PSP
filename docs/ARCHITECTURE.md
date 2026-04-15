# Architecture

_PSP Moonlight v0.2.2-beta_

## Overview

PSP Moonlight uses an **asymmetric dual-core software H.264 decode pipeline** to sustain real-time video on the PSP's constrained hardware. The two processors run distinct, complementary workloads concurrently.

This is a fully custom implementation — no `moonlight-common-c`, no sceMpeg, no external Moonlight library. The network stack, RTSP negotiation, RTP reassembly, FEC repair, H.264 decode, and color conversion are all purpose-built for the PSP's constraints.

---

## Processor Roles

```
Main CPU (Allegrex MIPS32R2 @ 333 MHz)
  Network receive (UDP socket, 1024-slot ring)
  RTP reassembly + frame boundary detection
  Reed-Solomon FEC repair (up to 66% parity) + predictive loss detection
  OpenH264 H.264 decode (Baseline, CABAC/CAVLC)
  Control stream (Moonlight protocol, input forward)
  Opus stereo audio decode (48 kHz, fixed-point) + adaptive PLC
  PID-based adaptive bitrate controller
  Keyboard / scroll / controller battery events
  UI rendering + host discovery

Media Engine (ME, MIPS @ 222 MHz)
  YUV420P → RGBA8888 conversion (VFPU)
  Runs concurrently while main CPU decodes next frame
```

The key insight: while the main CPU is feeding the next frame to OpenH264, the ME is converting the **previous** frame's YUV output to RGBA for display. Throughput = `max(decode_time, convert_time)` rather than their sum.

---

## Component Map

```
src/
├── openh264_decode.cpp       ← OpenH264 decode frontend + ME dispatch
│     WelsCreateDecoder()
│     DecodeFrameNoDelay()  → YUV420P output
│     BeginME(yuv420_to_rgba_entry)  → RGBA output (async on ME)
│     Dual-mode watchdog (Mode A: decode hang, Mode B: RTP stall)
│
├── sw_decoder_thread.c      ← Decoder thread + watchdog driver
│     Wakes on ring data available
│     P-frame skip-ahead when backlog >256 packets
│     Thread priority 0x18 (matched with control stream)
│     Drives force-restart on watchdog trigger
│
├── sw_me_worker.c           ← ME job scheduler
│     BeginME() / WaitME() / CheckME()
│     Pre/post dcache flush
│     Crash recovery: KillME + reinit + retry
│
├── [LEGACY] sw_decode_orchestrator.c ← CPU↔ME handoff (legacy CAVLC path, see legacy/)
├── [LEGACY] sw_cavlc.c               ← Hand-rolled CAVLC decoder (see legacy/)
├── [LEGACY] sw_vfpu_recon.c          ← VFPU reconstruction (see legacy/)
│
├── rtp_reassembly.c         ← RTP frame assembly + FEC trigger
├── rtp_fec.c                ← Reed-Solomon FEC + IDR/RFI policy
│     Predictive WiFi burst loss detection
│     Selective FEC skip when >50% parity lost
│     Multi-FEC block recovery (4 independent blocks)
├── rs.c                     ← Galois field Reed-Solomon codec
├── control_stream.c         ← Moonlight control protocol + input forwarding
│     Per-channel reliable sequence counters (48 entries)
│     Ping / IDR request / input packets
│     IDR exponential backoff (500ms → 4000ms)
│     Quality hysteresis (3-reading minimum)
│     Enhanced watchdog credit restoration (FEC-weighted)
│
├── network_connect.c        ← RTSP + TLS session establishment
│     ServerChallenge / ServerChallengeResponse
│     AES-GCM key negotiation
│     Sunshine Gen7 (clientVersion 19)
│
├── stream_crypto.c          ← AES-GCM video decryption + AES-CBC control
├── stream_resolution.c      ← Dynamic resolution scaling (4-step ladder)
│     256×144 → 320×192 → 368×208 → 480×272
│     EMA-smoothed decode time + loss rate triggers
├── signal_strength.c        ← PID adaptive bitrate controller
│     Composite quality: 40% RSSI + 30% CQ + 30% FEC
│     Anti-windup integral clamping, dead-zone prevention
├── network_me.c             ← Network ME ping thread + ME watchdog
│     Dynamic SO_RCVBUF (128KB–384KB)
│     Packet prioritization (IDR/SOF preferred)
│     RTCP receiver reports
│     WiFi power save disable during streaming
├── main.c                   ← Entry point, display loop, watchdog integration
└── display_gpu.c            ← PSP GE/GU framebuffer output
```

---

## Media Engine Bootstrap

`moonlight_me_helper/` is a **separate kernel PRX** that must be built and loaded before the main application. It provides two kernel exports:

- `InitME(entry, stack, stacksize, args, argp)` — loads a function into ME core and starts it
- `KillME()` — terminates the ME core

The main application imports these via `MediaEngine.S` stub. The ME runs `me_yuv420_to_rgba_entry()` defined in `sw_me_worker.c`, which executes the VFPU conversion loop.

---

## Memory Layout (approximate, PSP-1000, 32 MB user RAM)

| Region | Size | Contents |
|---|---|---|
| Application code | ~2.5 MB | PRX text/data/BSS |
| RGBA display buffers (×2) | 480×272×4 × 2 = ~1 MB | Double-buffered RGBA output |
| YUV buffers (×2) | 480×272×1.5 × 2 = ~600 KB | Double-buffered YUV from OpenH264 |
| UDP ring buffer | 1024 × 1500 B = ~1.5 MB | RTP packet ring |
| mbedTLS context | ~128 KB | TLS session state |
| OpenH264 decoder ctx | ~256 KB | WelsDecoder state |
| ME stack | 64 KB | ME thread stack |
| Remaining | ~26 MB | OS, system libs, UI textures |

---

## Error Recovery

### Watchdog (dual-mode)

**Mode A — Decode CPU hang:**
`g_decode_active_us` is set before `openh264_pipeline_decode_frame()` and cleared after. If it remains set for >3s, the decoder has stalled mid-decode (observed on corrupt NAL data at 300 kbps). Action: full force-restart.

**Mode B — RTP stall:**
`s_idle_count` increments each 16ms tick when no frames are decoded. After ~5s (300 ticks) with no frames, the RTP/ring pipeline has stalled. Action: soft recovery first (3× IDR burst); escalates to full force-restart after 3 soft failures. Credit restoration now weighted by FEC recovery rate (600/900/1200 frames). Intermediate flush at 3s provides graceful recovery before Mode B 5s timeout.

**Force-restart sequence:**
1. `sceKernelTerminateThread` + `sceKernelDeleteThread`
2. `openh264_pipeline_abandon()` — nulls globals (leaks ~2 MB per restart, capped at 3 restarts)
3. `openh264_pipeline_init()` — fresh WelsDecoder
4. `rtp_reassembly_reset()` + `rtp_fec_reset()`
5. Ring flush (tail = head)
6. Counters reset
7. New decode thread created

### Corruption Gating

When `g_refs_corrupted = 1` (set by RTP seq gap or RS failure), all non-IDR frames are skipped unconditionally (`return -4`). Display holds the last clean RGBA frame. This produces a freeze-then-resume pattern with **zero visual artifacts** vs the alternative of displaying macroblocked garbage.

---

## Protocol Stack

```
PSP ←→ Sunshine host

TLS 1.2 (mbedTLS)
  RSA-2048 server cert verification
  ECDH-RSA key exchange
  AES-128-GCM session keys

RTSP (RFC 2326 subset)
  SETUP / PLAY / TEARDOWN
  ServerChallenge handshake
  encryptionEnabled:1 header

RTP (UDP, port 47998)
  Moonlight Gen7 framing
  Sunshine 8-byte header skip
  Reed-Solomon FEC packets
  1024-slot receive ring

Control (UDP, port 47999)
  CONNECT / VERIFY handshakes (AES-CBC)
  Per-channel reliable sequence (48 channels)
  INPUT packets (buttons + analog sticks)
  Keyboard events (Type 5 key-down/up)
  Scroll events (Type 0x09, 0x33 hi-res)
  Controller arrival (Type 0x37 Xbox announce)
  Controller battery (Type 0x40 PSP battery %)
  PING / IDR request (exp backoff)

Audio (UDP, port 48000)
  Opus stereo 48 kHz
  Fixed-point Silk+CELT decoder
  Quality-adaptive PLC (35/45/60ms)
  Dynamic ring depth scaling
  Separate crypto error tracking
```
