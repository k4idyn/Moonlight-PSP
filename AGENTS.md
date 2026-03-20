# Moonlight PSP Core Monorepo: Agent Instructions

**To: Jules (or any AI agent operating on this project)**

## Overview
This repository contains the structured monorepo for the `moonlight-psp-core` project. It is designed to act as a proper C/C++ PSP SDK standard project structure for the Antigravity engine port of Moonlight.

## Directory Structure
- `moonlight-psp-core/src/`: Contains the Antigravity engine port logic and source files.
- `moonlight-psp-core/include/`: Contains Moonlight, Sunshine, and local header files.
- `moonlight-psp-core/assets/`: Contains PSP-specific UI resources (8-bit and 4-bit BMP images).
- `moonlight-psp-core/Makefile`: The primary Makefile configured for the PSP toolchain (MIPS).

## Guidelines for Development
1. **Source Code**: Place all `.c` and `.cpp` implementation files strictly in the `src/` directory.
2. **Headers**: Place all `.h` and `.hpp` headers in the `include/` directory so they remain accessible globally across the SDK structure.
3. **Assets**: Any new UI resources, images, or graphical assets must be added to `assets/` and encoded properly (e.g., 8-bit or 4-bit BMPs) due to PSP memory and VRAM constraints.
4. **Makefile Modifications**: When adding new source code files, make sure to update the `OBJS` variable inside the `moonlight-psp-core/Makefile` accordingly.
5. **Toolchain**: All building, linking, and packaging operations should seamlessly invoke PSP SDK configuration macros out of the box. Do not alter MIPS-specific flag optimizations (`-G0`, `-O2`) without verifying memory stability in PPSSPP first.

Follow these rules strictly to ensure stable compilation and a cohesive developer experience.
