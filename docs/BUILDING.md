# Building PSP Moonlight

_PSP Moonlight v1.5.0 build guide._

v1.5 uses Sony hardware AVC with unified Main-mode priming and 40 ms audio packets. It links Sony PSP MPEG/AVC imports. Earlier software decoder sources are preserved under `legacy/software/` and excluded from the application build.

Build with a community PSP toolchain on Windows, Linux or macOS.

---

## Prerequisites

### 1. PSPSDK Toolchain

The project requires the community PSP toolchain for the MIPS Allegrex architecture. 

**Recommended: pspdev/psptoolchain (Docker or native)**

#### Docker (fastest, no PATH conflicts)

```bash
docker pull pspdev/pspdev:latest
```

#### Linux / WSL2 (Ubuntu/Debian)

```bash
# Install build deps
sudo apt-get install -y cmake build-essential libgmp-dev libmpfr-dev \
    libmpc-dev libusb-dev texinfo bison flex

# Clone and build the toolchain (takes ~30 min)
git clone https://github.com/pspdev/psptoolchain.git
cd psptoolchain
./toolchain.sh
```

Add to your shell profile:
```bash
export PSPDEV=$HOME/pspdev
export PATH=$PATH:$PSPDEV/bin
```

#### Windows (using pspdev installer)

Download the pre-built toolchain from [pspdev/psptoolchain releases](https://github.com/pspdev/psptoolchain/releases).

Extract to a path **with no spaces** (e.g. `C:\pspdev`), then add `C:\pspdev\bin` to your PATH:

```powershell
# PowerShell — prepend for the current session
$env:PATH = "C:\pspdev\bin;$env:PATH"

# Verify
psp-gcc --version
# Expected: psp-gcc (GCC) 4.3.5
```

To set permanently: **Settings → System → About → Advanced system settings → Environment Variables**

### 2. GNU Make

Included with the pspdev toolchain (`make.exe` in the bin directory).

---

## Building

### Step 1 — Build the Media Engine Helper PRX

The ME helper is a separate kernel PRX. The root make all target builds it as a dependency; build it separately only when you need the helper module on its own.

```bash
cd moonlight_me_helper
make
```

Expected output:
```
  CC   main.c
  AS   MediaEngine.S
  AS   sceMeCore_driver.S
  LD   moonlight_me_helper.elf
  PRX  moonlight_me_helper.prx
```

## Application Build

### Sony hardware AVC

The v1.5 hardware build selects CAVLC/CABAC mode from the in-band PPS and submits H.264 access units to Sony sceMpeg AVC.

```bash
make clean
make -j2 RETAIL_BUILD=1 PSP_HARDWARE_AVC=1 PSP_AVC_UNIFIED_MAIN_MODE=1 PSP_VIDEO_FEC_PERCENT=35 PSP_VIDEO_FEC_MIN_REQUIRED=1 PSP_AUDIO_PACKET_DURATION_MS=40
```

The root build creates the matching Media Engine helper PRX and EBOOT.PBP. Install both in the same PSP game directory.

## Output Files

| File | Size (approx) | Description |
|---|---|---|
| `EBOOT.PBP` | ~2.5 MB | PSP executable (PARAM.SFO + PRX wrapped) |
| `moonlight.prx` | ~1.5 MB | Stripped PRX module |
| `moonlight_me_helper/moonlight_me_helper.prx` | ~8 KB | Kernel ME bootstrap PRX |

---

## Installing on PSP

**Custom firmware required.** ARK-4 on 6.60/6.61 is the supported target for public releases.

```
ms0:/PSP/GAME/Moonlight/
    EBOOT.PBP
    moonlight_me_helper.prx
```

Both files go in the **same directory**. `EBOOT.PBP` loads `moonlight_me_helper.prx` at startup via `sceKernelLoadModule`.

---

## Cleaning

```bash
# Clean main build
make clean

# Clean ME helper
cd moonlight_me_helper && make clean
```

---

## Compiler Flags

| Flag | Purpose |
|---|---|
| `-std=gnu99` | C99 with GNU extensions |
| `-O2` | Optimisation level 2 |
| `-G0` | Disable small data section (required for PRX) |
| `-Wall -Werror` | All warnings treated as errors |
| `-DPSP` | Platform guard for PSP-specific paths |
| `-D_PSP_FW_VERSION=660` | Firmware 6.60 syscall table |

---

## Troubleshooting

| Symptom | Cause | Fix |
|---|---|---|
| `psp-gcc: command not found` | PATH not set | Add toolchain `bin/` to PATH |
| `undefined reference to sceMe*` | ME helper not built first | Run `make` in `moonlight_me_helper/` first |
| `mksfoex: command not found` | PSPSDK bin not in PATH | Same fix as above |
| `make[1]: *** [moonlight.elf] Error 1` | Compile or link failed | Inspect the first compiler/linker error and rebuild successfully; an existing EBOOT can be stale |
| `warning: overriding commands for target moonlight.elf` | The project replaces an SDK recipe | Check that make exits zero and the fresh main/helper/EBOOT match the selected flags |
| Unexpected PRX size | Diagnostics or symbols | Rebuild the intended retail mode from clean objects and preserve its matching helper and package hashes |
| Black screen on PSP | ME helper PRX not found | Confirm both files are in the same XMB directory |
| `avcodec.h: No such file` | Legacy FFmpeg path referenced | Legacy path no longer built by default; see `legacy/` |
