# Contributing to PSP Moonlight

Contributions are welcome. This is a PSP homebrew project targeting real hardware, so there are some platform-specific constraints worth understanding before you start.

This is the **v1.1.0 public release** of the rewritten PSP-native Moonlight pipeline. Hardware testing is still highly valuable for ongoing maintenance and compatibility work.

---

## Before You Start

The PSP's dual-CPU model has constraints that aren't obvious from the source alone. Read these first:

- [docs/BUILDING.md](BUILDING.md) — get the toolchain set up
- [docs/ARCHITECTURE.md](ARCHITECTURE.md) — how the asymmetric dual-core pipeline works
- [docs/DECODER_PIPELINE.md](DECODER_PIPELINE.md) — the OpenH264 + ME decode path in detail
- [docs/KNOWN_ISSUES.md](KNOWN_ISSUES.md) — what's broken and what's actively being worked on

---

## PSP Toolchain Quirks You Will Hit

**GCC 4.3.5 is ancient.** C99 only, no C11, no stdatomic, no builtin atomics. Use `sceKernelIntc*` or critical sections for shared state.

**`-G0` is mandatory.** Without it the linker puts globals in `.sdata` which breaks PRX loading silently.

**PRX memory model:** Global arrays > ~4 KB should use `memalign(64, size)` or `valloc` — large statics go in BSS which is fine, but cache-line alignment matters for the ME.

**Stack sizes are fixed at thread creation.** The decode thread uses 256 KB. Add large locals to anything in the decode path and you'll get a silent stack overflow. Use heap.

**`printf` / `fprintf` crash if called from the ME core.** The ME has no kernel context for I/O. Use the ring-buffer logger (`diag_log.c` / `DIAG_LOG` macro). Never call any `sceKernel*` from ME context except `sceKernelDcacheWritebackAll`.

**`mfv` (VFPU→GP register) always returns 0 on the ME.** Hardware bug. Extract VFPU results via `sv.s`/`sv.q` to memory, then `lw`. See the VFPU notes in [docs/ARCHITECTURE.md](ARCHITECTURE.md).

---

## Media Engine Rules

The ME is a second MIPS core running at 222 MHz with no kernel access. These are hard rules:

1. **No system calls from ME** — except the safe whitelist: dcache flush, semaphore signal.
2. **No heap allocation from ME.** All buffers must be pre-allocated on the main CPU and passed in before `BeginME()`.
3. **Cache coherency must be handled explicitly.** Before `BeginME()`, call `sceKernelDcacheWritebackInvalidateAll()` on every buffer the ME will read. After ME finishes, flush before the main CPU reads output. Missing this produces silent garbage data.
4. **No uncached address aliases on the ME.** Never pass `0x48xxxxxx` uncached pointers to ME code. Use cached `0x00xxxxxx` addresses only. This is the single most common cause of ME bus errors and was responsible for a 1,500× YUV performance regression during development.
5. **Use `sw_me_worker.c` patterns.** The `BeginME`/`WaitME` wrapper handles pre/post dcache flush correctly. Don't reinvent the cache coherency protocol.

---

## Testing: Real Hardware vs PPSSPP

PPSSPP is useful for UI and control flow testing but it has significant limitations on this project:
- Doesn't emulate ME timing or cache coherency
- Doesn't emulate Wi-Fi — all network code is untestable in PPSSPP
- Masks memory alignment bugs that kill real hardware
- May not reproduce ME crashes, watchdog triggers, or RTP stall behavior

Test on real hardware before submitting a PR for anything touching the decode pipeline, ME worker, RTP stack, FEC, or network code. Real hardware is the only reliable test environment for this project.

The debug environment is **PSPLink + pspsh** over USB: `scrshot` for framebuffer capture, `meminfo` for RAM maps, `pokew`/`peekw` for live memory inspection.

Baseline test configuration (validated):
- PSP-1000, ARK-4 on 6.60/6.61
- Sunshine host on a 2.4 GHz 802.11b/g LAN
- 480×272, 15 fps, 384 kbps, H.264 Baseline, CAVLC
- At least 3 minutes — short runs don't exercise the watchdog edge cases

---

## Code Style

- **C99** (`-std=gnu99`) throughout. C++ in UI files only (`game_grid_ui.cpp`, `ui_manager.cpp`, `pairing_pin_ui.cpp`).
- No dynamic dispatch in hot paths. No vtables, no function-pointer indirection in the decode loop.
- Prefer explicit error codes over errno. Use `DIAG_LOG` for diagnostics, never `printf`.
- Functions should be short and auditable for cache behavior — especially anything in the ME path.
- Headers live in `include/`. One header per module. Circular includes will break GCC 4.3.5.
- No hardcoded frame dimensions outside `stream_resolution.c`. Everything routes through the unified resolution table.

---

## What To Work On

Open problems that are worth working on:

- **Deblocking filter** — H.264 in-loop deblocking runs on the encoder side but we don't have a PSP decoder-side implementation. ME bandwidth is the constraint.
- **Display double buffering** — single-buffered display causes tearing. Fix needs careful GE timing.
- **30 fps stability** — playable at 15–18 fps; 30 fps requires RTP/FEC processing improvements or a faster assembly inner loop.
- **PSP-2000/3000 testing** — all confirmed results are PSP-1000. The extra RAM on -2000/-3000 should help; untested.
- **PSP Go support** — no Wi-Fi hardware test on PSP Go yet.
- **Power switch resume** — suspend sends stream pause but resume reconnect is not fully implemented.

---

## Submitting a PR

1. Fork the repo, branch off `main`.
2. `make clean && make` — must build clean with no warnings.
3. Test on real hardware if your change touches decode, ME, RTP, FEC, or network.
4. Write a clear PR description: what changed, why, and what hardware + config you tested on.
5. If you found a hardware bug, document it — `docs/KNOWN_ISSUES.md` or a new doc in `docs/`. Bug documentation is as valuable as fixes for the scene.


---

## Questions

Open an issue. If it's a quick question about PSP toolchain setup there are better places (PSP homebrew Discord, pspdev GitHub discussions) but project-specific questions belong here.
