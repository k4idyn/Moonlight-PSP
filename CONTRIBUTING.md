# Contributing to PSP Moonlight

Contributions are welcome. This is a PSP homebrew project targeting real hardware, so platform-specific testing matters.

This is the **v1.2.0 public release** of the rewritten PSP-native Moonlight pipeline. Hardware testing is still highly valuable for ongoing maintenance and compatibility work.

## Before You Start

The PSP's dual-CPU model has constraints that are not obvious from desktop builds alone. Read these first:

- [docs/BUILDING.md](docs/BUILDING.md) - toolchain setup
- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) - asymmetric CPU, ME, GE, and network pipeline
- [docs/DECODER_PIPELINE.md](docs/DECODER_PIPELINE.md) - OpenH264 plus ME conversion path
- [docs/KNOWN_ISSUES.md](docs/KNOWN_ISSUES.md) - current platform limits and troubleshooting notes
- [docs/UI_FLOW.md](docs/UI_FLOW.md) - menu, host, pairing, app list, and stream flow

## PSP Toolchain Rules

- GCC 4.3.5 is old. Use C99-compatible code and avoid C11 atomics.
- `-G0` is mandatory. Without it, globals can land in `.sdata` and break PRX loading.
- Large hot-path buffers should be aligned and allocated deliberately; do not put large locals on thread stacks.
- The decode thread stack is fixed. Put large temporary buffers on the heap or in owned static storage.
- Do not call `printf` from stream, ME, or input hot paths. Use `DIAG_LOG`; retail builds keep diagnostics off by default.
- Keep hardcoded dimensions out of new code. Preset and custom sizes route through `stream_resolution.c`.

## Media Engine Rules

The Media Engine is a second MIPS core with no normal kernel context:

- No system calls from ME code except the project-approved cache and semaphore operations.
- No heap allocation from ME code.
- Handle cache coherency explicitly before and after ME work.
- Pass cached addresses to ME code; do not pass uncached aliases.
- Keep ME helpers small, deterministic, and documented.

## Hardware Testing

PPSSPP is useful for UI and control-flow checks, but it does not emulate ME timing, PSP Wi-Fi, cache coherency, or the same memory pressure as real hardware. Test on real PSP hardware before submitting changes that touch decode, RTP/FEC, audio, input, pairing, networking, or rendering.

Baseline PSP-1000 test configuration:

- ARK-4 on 6.60 or 6.61
- Sunshine host on a local 2.4 GHz 802.11b/g network
- H.264 Baseline with CAVLC enabled
- Performance first: 300x170, 30 fps, 384 kbps, 1056-byte packets, FEC 35 percent, audio disabled
- Balanced follow-up: 360x204, 20 fps, 480 kbps, 1200-byte packets, FEC 35 percent, audio enabled
- At least three minutes of playback for stream changes; short runs do not cover watchdog, reconnect, and buffer edge cases

## Code Style

- C99 (`-std=gnu99`) throughout. C++ is used only where the current UI modules already use it.
- No dynamic dispatch in stream hot paths.
- Prefer explicit error codes over `errno`.
- Keep functions short enough to audit for cache, allocation, and PSP syscall behavior.
- Headers live in `include/`. Avoid circular includes.
- Keep retail behavior low-work: diagnostics, telemetry, dumps, and expensive counters should stay out of retail unless they are required for normal functionality.

## Useful Areas To Improve

- PSP-1000 receive and RTP/FEC processing under burst loss
- Decoder-side artifact recovery that does not exceed the CPU budget
- Display timing and buffer-swap polish
- Pairing compatibility reports across Sunshine versions, CFW variants, and network security modes
- PSP-2000, PSP-3000, and PSP Go validation
- Resume and reconnect behavior after suspend

## Submitting a PR

1. Fork the repo and branch off `main`.
2. Build with `make clean && make RETAIL_BUILD=1`.
3. Test on real hardware when your change touches decode, ME, RTP/FEC, audio, input, pairing, networking, or rendering.
4. Describe what changed, why, and what hardware plus stream preset you tested.
5. Update docs when behavior, settings, limitations, or build requirements change.

## Questions

Open an issue for project-specific questions. For generic PSP toolchain setup, pspdev community channels may be faster.
