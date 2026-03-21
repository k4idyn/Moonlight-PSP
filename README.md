<div align="center">

# Moonlight PSP Core
**The Hardware-Accelerated Moonlight Game Streaming Client for Sony PlayStation Portable**

[![License: GPL v3](https://img.shields.io/badge/License-GPL%20v3-blue.svg)](https://www.gnu.org/licenses/gpl-3.0)
[![Platform: PSP](https://img.shields.io/badge/Platform-Sony%20PSP-black.svg)](https://en.wikipedia.org/wiki/PlayStation_Portable)
[![Language: C/C++](https://img.shields.io/badge/Language-C%2FC%2B%2B-00599C.svg)](#)

</div>

## Latest Release: [v0.1.0.3-alpha](https://github.com/k4idyn/Moonlight-PSP/releases/tag/v0.1.0.3-alpha)
**Status**: Active / Experimental Alpha

**Release Highlight**: **Architectural Cleanup & Toolchain Stabilization**. This release resolves critical build errors, standardizes project-wide header guards, and implements Mini-XML 3.x compatibility for improved ABI safety.

**⚠️ CURRENT KNOWN PERSISTENT ERROR ⚠️**
The client is currently experiencing a connection error during the GameStream initialization sequence:
`Status: FAILED: control stream establishment (0x00000074)`

This is an `ETIMEDOUT` (Error 116) timeout stemming from the ENet network layer. We have verified functional parity with previous builds using the **PPSSPP emulator** and are investigating the ENet handshake failure.

---

## Overview

The `moonlight-psp-core` repository is the dedicated C/C++ SDK monorepo for the [Moonlight Game Streaming Client](https://moonlight-stream.org/) targeting the Sony PlayStation Portable (PSP-1000 and later).

## Repository Structure

The project follows a standard monorepo format compatible with the PSPSDK build environment:

```text
moonlight-psp-core/
├── assets/          # BMP GUI resources (VRAM-optimized)
├── docs/            # Architectural documentation and research
├── include/         # Unified headers (mbedTLS, ENet, Opus, MXML, etc.)
├── lib/             # Pre-compiled static libraries for PSP MIPS
├── scripts/         # Verification and testing automation
├── src/             # Source files for network, video, audio, and controls
└── Makefile         # Primary configuration for the PSPSDK build environment
```

## Build Instructions (Windows)

We provide an automated build script for Windows environments:

```batch
.\build_psp.bat
```

To verify the build in PPSSPP:
```powershell
.\verify_build.ps1
```

## Build Instructions (Linux / WSL2)

```bash
make -j$(nproc)
```

## Licensing Information

The `moonlight-psp-core` client is distributed under the **GNU General Public License v3.0**.

---
**Build Status**
- [x] v0.1.0.1-alpha: Initial boot
- [x] v0.1.0.2-alpha: MFILE & SOCKET STABILIZATION
- [x] v0.1.0.3-alpha: **ARCHITECTURAL CLEANUP**

See [RELEASE_NOTES.md](RELEASE_NOTES.md) for full technical breakdown.
