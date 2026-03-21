# Moonlight PSP Core Monorepo: Developer Instructions

## Overview
This repository contains the structured monorepo for the `moonlight-psp-core` project. It is designed to act as a proper C/C++ PSP SDK standard project structure for the specialized port of Moonlight.

## Directory Structure
- `moonlight-psp-core/src/`: Contains the core engine logic and source files.
- `moonlight-psp-core/include/`: Contains Moonlight, Sunshine, and local header files.
- `moonlight-psp-core/assets/`: Contains PSP-specific UI resources (8-bit and 4-bit BMP images).
- `moonlight-psp-core/Makefile`: The primary Makefile configured for the PSP toolchain (MIPS).

## Environment Setup

### 1. Developer Environment
For developers to operate, a standard MIPS toolchain (PSPSDK) and several compiled dependencies alongside `make` are required.

- **Note**: This repository includes all necessary pre-compiled dependencies (`lib/`) and headers (`include/`) out-of-the-box. No additional dependency configuration is required for core development.

- **Environment Setup**: Ensure your PSPSDK is initialized in your PATH. The `Makefile` relies on `psp-config --pspsdk-path` to dynamically link the SDK.
  ```bash
  export PATH=$PATH:/path/to/pspsdk/bin
  ```

### 2. Building
Since the project relies solely on standard PSPSDK functionality, building is executed generically on any host environment (Linux/Windows/macOS):

```bash
make clean
make
```

---

## Guidelines for Development
1. **Source Code**: Place all `.c` and `.cpp` implementation files strictly in the `src/` directory.
   - **Note**: Critical fixes for `moonlight-common-c` are synchronized into `src/common/` to ensure the project remains portable and buildable with a standard `Makefile`.
2. **Headers**: Place all `.h` and `.hpp` headers in the `include/` directory.
   - **Note**: Common headers are located in `include/common/`.
3. **Assets**: Any new UI resources, images, or graphical assets must be added to `assets/` and encoded properly (e.g., 8-bit or 4-bit BMPs) due to PSP memory and VRAM constraints.
4. **Makefile Modifications**: When adding new source code files, make sure to update the `OBJS` variable inside the `moonlight-psp-core/Makefile` accordingly.
5. **Toolchain**: All building, linking, and packaging operations should seamlessly invoke PSP SDK configuration macros out of the box. Do not alter MIPS-specific flag optimizations (`-G0`, `-O2`) without verifying memory stability in PPSSPP first.

Follow these rules strictly to ensure stable compilation and a cohesive developer experience.
