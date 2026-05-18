# Architecture

_PSP Moonlight v1.2.0_

## Overview

PSP Moonlight uses an **asymmetric dual-core software H.264 decode pipeline** to sustain real-time video on the PSP's constrained hardware. The two processors run distinct, complementary workloads concurrently.

This is a fully custom implementation — no `moonlight-common-c`, no sceMpeg, no external Moonlight library. The network stack, RTSP negotiation, RTP reassembly, FEC repair, H.264 decode, and color conversion are all purpose-built for the PSP's constraints.

Recent networking refinements in this tree:

- Launch/transport bitrate starts from the configured client bitrate without hidden startup downscale.
- Connection quality classification is transport-focused (loss/FEC recovery), while decoder FPS remains diagnostic.
- RTCP Receiver Reports use interval loss accounting and jitter in RTP clock units for more accurate host feedback.
- Adaptive fast-drop threshold is tuned to reduce overreaction to brief transient loss.

---

## Processor Roles

```
Main CPU (Allegrex MIPS32R2 @ 333 MHz)
  Network receive (UDP socket, 512-slot ring)
  RTP reassembly + frame boundary detection
  Reed-Solomon FEC repair (up to 66% parity) + predictive loss detection
  OpenH264 H.264 decode (Baseline, CAVLC required for normal PSP playback)
  Control stream (Moonlight protocol, input forward)
  Opus mono host decode (48 kHz, fixed-point) + adaptive PLC
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
│     Selective FEC skip when >75% parity lost
│     Multi-FEC block recovery (4 independent blocks)
├── rs.c                     ← Galois field Reed-Solomon codec
├── control_stream.c         ← Moonlight control protocol + input forwarding
│     Per-channel reliable sequence counters (48 entries)
│     Ping / IDR request / input packets
│     IDR exponential backoff (500ms ceiling)
│     Quality hysteresis (3-reading minimum)
│     Transport-first quality classification (decode FPS is diagnostic-only)
│     Enhanced watchdog credit restoration (FEC-weighted)
│
├── network_connect.c        ← RTSP + TLS session establishment
│     ServerChallenge / ServerChallengeResponse
│     AES-GCM key negotiation
│     Sunshine Gen7 (clientVersion 19)
│     Launch bitrate uses client-selected target directly
│
├── stream_crypto.c          ← AES-GCM video decryption + AES-CBC control
├── stream_resolution.c      ← PSP-aspect stream sizing
│     300×170 Performance, 360×204 Balanced, 480×272 Quality
│     Custom dimensions normalized to the PSP LCD aspect ratio
├── signal_strength.c        ← PID adaptive bitrate controller
│     Composite quality: 40% RSSI + 30% CQ + 30% FEC
│     Anti-windup integral clamping, dead-zone prevention
│     Fast-drop trigger tuned for consecutive-loss confirmation
├── network_me.c             ← Network ME ping thread + ME watchdog
│     Dynamic SO_RCVBUF (128KB–512KB, capped by runtime-applied socket limit)
│     Packet prioritization (IDR/SOF preferred)
│     RTCP receiver reports (interval loss + RTP-clock jitter)
│     WiFi power save disable during streaming
├── main.c                   ← Entry point, display loop, watchdog integration
│     Quick relaunch: cached hosts + skip_rescan on intentional exits
├── host_discovery.c         ← mDNS + HTTP probe + subnet scan + host list UI
│     mdnsDiscoverHosts(): multicast _nvstream._tcp.local.
│     quickSubnetScan(): /24 TCP scan, interruptible, Square button
│     Smooth-scroll animation, 3-layer shadow, paired status display
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

Current release build accounting from `psp-size moonlight.elf`:

| Segment | Bytes | Notes |
|---|---:|---|
| `.text` | 1,886,224 | Code, OpenH264/Opus/mbedTLS/libpng-linked text |
| `.data` | 16,788 | Initialized globals |
| `.bss` | 9,379,792 | Static runtime pools and fixed buffers |
| Total ELF RAM image | 11,282,804 | About 10.76 MiB before PSP kernel/module overhead |

Largest statically accounted runtime pools:

| Symbol | Bytes | Purpose |
|---|---:|---|
| `s_wq_slots` | 786,480 | Network/decode work queue slots |
| `g_shared` | 769,076 | Shared network/ME packet state (512-slot packet ring + frame handoff ring) |
| `g_rgba_static` | 1,114,112 | Double RGBA frame buffers |
| `s_mbedtls_heap` | 1,048,576 | Fixed mbedTLS allocator heap |
| `s_icon_pool` | 1,048,576 | 16 resident padded RGB565 icon slots |
| `s_png_download_buf` | 524,288 | Fixed PNG download buffer |
| `s_ram_pool` | 512,000 | Safety buffer static RAM pool |
| `display_list` / `s_gu_list` / `assembly_buffer` | 786,432 | GU display and packet assembly buffers |
| `s_png_arena` | 196,608 | Fixed libpng allocation arena |
| `g_pkt_storage` / `g_rec_storage` | 384,000 | RTP/FEC packet reconstruction storage |
| `s_ring` | 131,080 | UDP receive ring |
| `s_http_recv_buf` | 98,304 | App metadata receive buffer |
| `g_default_icon` | 65,536 | Generated default icon |
| `s_decoder_storage` | 32,768 | Fixed Opus decoder storage |

The older approximate table below is kept as subsystem orientation only; the measured segment/symbol accounting above is the release accounting source of truth.

| Region | Size | Contents |
|---|---|---|
| Application code | ~2.5 MB | PRX text/data/BSS |
| RGBA display buffers (×2) | 480×272×4 × 2 = ~1 MB | Double-buffered RGBA output |
| YUV buffers (×2) | 480×272×1.5 × 2 = ~600 KB | Double-buffered YUV from OpenH264 |
| UDP ring buffer | 512 × 1500 B = ~750 KB | RTP packet ring |
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
2. `openh264_pipeline_abandon()` — tears down the old decoder/ME state and retains only the static RGBA storage
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
  Opus 48 kHz when audio is enabled
  Fixed-point Silk+CELT decoder
  Quality-adaptive PLC
  Dynamic ring depth scaling
  Separate crypto error tracking
  Performance preset skips local decode/playback work
```
