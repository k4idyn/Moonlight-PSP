# Release Notes v0.1.0.3-alpha (Cleanup)

## Overview
This release focuses on architectural cleanup, toolchain stabilization, and ABI safety. It resolves critical build environment issues that prevented consistent compilation on standard PSP toolchains.

## Technical Findings
1. **Toolchain Incompatibility**: The original PSP SDK paths contained characters (like `$`) that caused recursive `make` failures and character encoding errors in PowerShell/CMD environments.
2. **Include Guard Collision**: Multiple headers used reserved leading underscores (e.g., `_mxml_h_`) or inconsistent naming patterns, potentially leading to collision or IDE indexing issues.
3. **ABI Fragility**: The XML parsing layer relied on direct access to internal `mxml_node_s` fields, which is incompatible with updated Mini-XML versions and violates typical library encapsulation.

## Fixes
- **Toolchain Normalization**: Flattened and renamed the PSP SDK root to `C:\bin_root\psp_sdk_root`. Created a `build_psp.bat` automated wrapper that handles environment variables and provides a `psp-gcc.exe` alias for versioned compilers.
- **Header Guard Standardization**: Applied the `MOONLIGHT_*_H` convention to all project headers, including `mxml.h`, `enet.h`, and the `ark4` subsystem.
- **MXML Compatibility Layer**: Implemented Mini-XML 3.x accessor macros in `xml.c` to allow the codebase to function with both legacy (v2.x) and modern (v3.x+) libraries.
- **Link Order Optimization**: Reorganized the `LIBS` sequence in the `Makefile` to satisfy `psp-fixup-imports` requirements, ensuring SDK libraries are linked in the correct order relative to user dependencies.

## Verification
- **Pairing Consistency**: Verified that the new build successfully initializes the network stack and generates a pairing PIN (e.g., PIN 3967), maintaining functional parity with v0.1.0.2.
- **Build Reproducibility**: Successfully generated a standard `EBOOT.PBP` (1,794,994 bytes) using the new automated build pipeline.

## Known Issues
- **0x80 (ENOTCONN)**: Investigation continues into the ENet control stream disconnect error.
