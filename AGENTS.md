# AGENTS.md - Repository Governance for Moonlight PSP

## Role: GitHub Architect
This repository follows a strict **MIPS-PSP Monorepo Architecture**. All agents MUST adhere to these guidelines to maintain parallel reliability and cadence across the project.

## Core Architectural Rules
1. **Directory Integrity**:
   - `src/`: All source code. Subdivide into `modules/`, `libgamestream/`, and `common/`.
   - `include/`: All public and project-specific headers.
   - `lib/`: Pre-compiled static libraries `.a` for PSP MIPS.
   - `assets/`: VRAM-optimized resources.
   - `scripts/`: Verification and build automation.
   - `docs/`: Technical research and specifications.

2. **File Handling & Build Cadence**:
   - **DO NOT** modify the `Makefile` LDFLAGS unless explicitly required by a new dependency. Use `LIBS` for core link-ordering.
   - **DO NOT** mix standard POSIX and PSP-native network calls. Always prefer `sceNet` equivalents for socket reliability.
   - **ALWAYS** verify builds using `.\build_psp.bat` and `.\verify_build.ps1` before marking a task as complete.

3. **Versioning & Tagging**:
   - Follow semantic versioning with `-alpha` or `-beta` suffixes as appropriate.
   - Tag all major stabilization milestones (e.g., `v0.1.0.x-alpha`).

## Guidance for Jules
- Maintain the **Exhaustive Absolute Perfection Recipe** for network module loading in `main.c`.
- Ensure all new headers follow the `MOONLIGHT_*_H` guard convention.
- If adding a new module, update the `Makefile` and `exports.c` (if kernel-exported).

---
*Maintained by the GitHub Architect.*
