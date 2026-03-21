<div align="center">

# Moonlight PSP Core
**The Hardware-Accelerated Moonlight Game Streaming Client for Sony PlayStation Portable**

[![License: GPL v3](https://img.shields.io/badge/License-GPL%20v3-blue.svg)](https://www.gnu.org/licenses/gpl-3.0)
[![Platform: PSP](https://img.shields.io/badge/Platform-Sony%20PSP-black.svg)](https://en.wikipedia.org/wiki/PlayStation_Portable)
[![Language: C/C++](https://img.shields.io/badge/Language-C%2FC%2B%2B-00599C.svg)](#)

</div>

## Development Status: Active Debugging & Porting

**Current Focus**: Stabilizing the ENet control stream and optimizing the `moonlight-common-c` integration for the PSP's MIPS architecture.

**⚠️ CURRENT KNOWN PERSISTENT ERROR ⚠️**
The client is currently experiencing a persistent connection error during the GameStream initialization sequence:
`Status: FAILED: control stream establishment (0x00000074)`

This is an `ETIMEDOUT` (Error 116) timeout stemming from the ENet network layer. We have implemented several PSP-specific socket workarounds and are currently verifying them on both real hardware and the PPSSPP emulator.

**Hardware Support Phase**
The project is currently in a **Beta Testing Phase**. While the core engine and protocol logic are functional, streaming stability is limited by ongoing network timing issues. We recommend using the **PPSSPP emulator** for all active debugging and contribution.

---

## Overview

The `moonlight-psp-core` repository is the dedicated C/C++ SDK monorepo for the [Moonlight Game Streaming Client](https://moonlight-stream.org/) targeting the Sony PlayStation Portable (PSP-1000 and later). This project enables users to stream games from an NVIDIA GameStream or Sunshine host directly to their PSP over a local network connection.

This client is engineered from the ground up to operate within the strict hardware constraints of the PSP ecosystem, specifically the absolute 32MB main memory ceiling on PSP-1000 models. It achieves this by effectively balancing the processing workloads between the main Allegrex CPU and the Media Engine (ME) coprocessor.

## Core Features and Architectural Highlights

* **Hardware-Accelerated Video Decoding**
  * Leverages the dedicated PSP Media Engine (ME) hardware AVC block for highly efficient H.264 stream decoding up to Main Profile.
  * Bypasses the main CPU for video decompression, reserving critical MIPS R4000 cycles entirely for network transport and input polling.
  * Dynamic, low-latency color conversion optimized natively for the PSP display format.

* **Direct VRAM Graphics Pipeline**
  * Renders decoded frames directly to the isolated 2MB eDRAM VRAM buffer.
  * Synchronized VBlank swaps utilizing `sceDisplayGetFrameBuf` for perfectly tear-free visualization on the 480x272 LCD screen.

* **Optimized Memory Architecture**
  * Rigid adherence to a strictly calculated physical memory budget to prevent out-of-memory crashes on older hardware revisions.
  * Shared buffer pooling for GameStream protocol networking (ENET over UDP) and modern TLS security layers (mbedTLS) to prevent heap fragmentation.
  * Fixed-size NAL unit buffers aligned accurately for Media Engine processing blocks.

* **Audio Subsystem**
  * Opus packet decoding to PCM utilizing fixed-point math routines optimized specifically for MIPS variants without vector instructions.
  * Rapid audio routing through the Virtual Mobile Engine (VME) channels for flawless 16-bit 48kHz stereo output.

* **Persistent Identity Spoofing Protocol**
  * Custom credential management synchronizing the PSP client identity to match established GameStream certified devices (e.g., NVIDIA Shield or 3DS parameters).
  * Unique ID parameter synchronization within the mbedTLS X.509 generation step to ensure a persistent, unrejected host pairing state across multiple connections.

## Repository Structure

The project has been standardized into a monorepo format strictly compatible with traditional PSP SDK conventions and automated MIPS toolchains:

```text
moonlight-psp-core/
├── assets/          # 8-bit and 4-bit BMP GUI resources (VRAM-optimized)
├── docs/            # Technical documentation, research notes, and architectural maps
├── include/         # Unified headers for Moonlight, Sunshine, and local definitions
├── lib/             # Pre-compiled static libraries for PSP MIPS cross-compilation
├── licenses/        # Open-source dependency licenses (GPLv3, MIT, Apache 2.0, etc.)
├── src/             # Source files for network, video, audio, and control streams
└── Makefile         # Primary configuration for the PSPSDK build environment
```

## Build Instructions

Developing and compiling the client requires a fully configured PSP homebrew environment.

### Quick Start (Linux / Jules VM)

If you are operating in a fresh Ubuntu VM, you can automate the entire environment setup (PSPSDK + Dependencies) using the provided script:

```bash
cd moonlight-psp-core
./jules_setup.sh
make -j$(nproc)
```

### Manual Compilation Process (Host / WSL)

1. Ensure `PSPSDK` is installed and the `PSPDEV` environment variable is set.
2. Navigate to the root directory and invoke the standard build:

```bash
cd moonlight-psp-core
make clean
make -j$(nproc)
```

### Deployment

A successful build will process the source trees and output an `EBOOT.PBP` combined executable package.

To deploy the client on authentic hardware or an emulator (PPSSPP):
1. Mount the PSP Memory Stick.
2. Transfer the compiled `EBOOT.PBP` directly to `ms0:/PSP/GAME/moonlight/EBOOT.PBP`.
3. Launch the application from the PSP's XMB Game interface.

## Licensing Information

The `moonlight-psp-core` client is distributed under the **GNU General Public License v3.0**. Please refer to the [LICENSE](LICENSE) file located in the root directory for standard distribution semantics.

Moonlight is a free, open-source project. Portions of the codebase, explicitly network transport and protocol logic, may be adapted from the core `moonlight-common-c` library authored by Cameron Gutman and original Moonlight contributors. External dependencies such as ENET and mbedTLS are covered under their respective MIT and Apache 2.0 open-source agreements.
