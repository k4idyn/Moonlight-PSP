# Moonlight PSP Core Roadmap

This document outlines the development phases, current status, and future goals for the Moonlight PSP project.

## Current Status (v0.1.0-alpha)
- **Status**: Active Debugging & Porting
- **Major Achievement**: Unified build system with automated dependency management (Ubuntu/VM).
- **Core Engine**: Moonlight-common-c protocol logic synchronized for MIPS.
- **Hardware Integration**: AVC hardware decoding partially mapped.
- **Persistent Bug**: ENet control stream timeout (Error 0x74 / ETIMEDOUT).

## Phase 1: Stability (In Progress)
- [ ] Fix ENet `ETIMEDOUT` / Error 116 on PSP socket layer.
- [ ] Stabilize RTSP handshake across both real hardware and PPSSPP.
- [ ] Verify A/V sync over a steady 3-minute stream session.

## Phase 2: Optimization
- [ ] Fine-tune Media Engine (ME) AVC decoding for 60 FPS.
- [ ] Implement VRAM-based zero-copy rendering for the graphics pipeline.
- [ ] Reduce heap fragmentation and optimize for PSP-1000 32MB limit.

## Phase 3: Features
- [ ] Support custom bitrate and resolution settings.
- [ ] Implement multi-controller support via PSP hardware buttons.
- [ ] Add basic UI for server discovery and pairing.

## Phase 4: Release
- [ ] Official "Beta" Release for real hardware testers.
- [ ] Performance benchmarks and hardware compatibility list.
