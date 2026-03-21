# Release Notes v0.1.0.3-alpha (Architectural Alignment)

## Overview
This release implements critical architectural improvements and toolchain standardizations in response to the recent CodeRabbit architectural audit. The focus is on ensuring ABI safety, project-wide consistency, and build reproducibility for the Moonlight PSP ecosystem.

## Architectural Improvements (CodeRabbit Response)
1. **Toolchain Path Normalization**: Resolved inconsistencies in the PSP SDK pathing that affected cross-platform build reliability. The SDK environment is now standardized to `C:\bin_root\psp_sdk_root`.
2. **Header Guard Standardization**: Performed a project-wide sweep to align all include guards with the `MOONLIGHT_*_H` convention, replacing reserved identifiers and non-standard naming patterns to ensure clean symbol resolution.
3. **MXML API Encapsulation**: Refactored `src/libgamestream/xml.c` to use public Mini-XML accessor APIs (`mxmlGetFirstChild`, etc.) instead of direct struct field access. This migration ensures compatibility with both v2.x and v3.x Mini-XML libraries and improves long-term ABI stability.
4. **Linker Sequence Optimization**: Adjusted the `Makefile` library sequence to satisfy the `psp-fixup-imports` tool's requirement for specific SDK-to-user library ordering.

## Verification
- **System Parity**: Verified that the updated codebase maintains functional parity with v0.1.0.2. Automated tests in PPSSPP confirm successful network initialization and pairing PIN generation (e.g., PIN 3967).
- **Executable Integrity**: Successfully generated a standard `EBOOT.PBP` (1,794,994 bytes) using the standardized build pipeline.

## Known Issues
- **0x80 (ENOTCONN)**: Investigation continues into the ENet control stream disconnect error.
