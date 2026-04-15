# Building PSP Moonlight

_v0.2.2-beta_

This guide covers building PSP Moonlight from source on **Windows (WSL2)**, **Linux**, and **macOS**.

---

## Prerequisites

### 1. PSPSDK Toolchain

The project requires the community PSP toolchain, which provides `psp-gcc 4.3.5` for the MIPS Allegrex architecture.

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

### 3. OpenH264 (PSP cross-compiled)

`openh264_decode.cpp` links against PSP-cross-compiled OpenH264 (`libopenh264.a`). The pre-compiled
static library must be present in `$PSPDEV/psp/lib`. Build OpenH264 from source for the PSP MIPS
target, or download a pre-built package.

> **Note:** The legacy FFmpeg decode path (`legacy/ffmpeg_decode.c`) is no longer built by default.
> If you need the FFmpeg path for comparison, see the `legacy/` directory and its original build
> instructions in the [FFmpeg PSP port](https://github.com/pspdev/psp-ports).

---

## Building

### Step 1 — Build the Media Engine Helper PRX

The ME helper is a separate kernel PRX that must be built **before** the main application.

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

### Step 2 — Build the Main Application

```bash
cd ..
make
```

Expected output (abbreviated):
```
  CC   src/main.c
  CXX  src/openh264_decode.cpp
  CC   src/sw_me_worker.c
  CC   src/sw_decoder_thread.c
  CC   src/stream_resolution.c
  CC   src/signal_strength.c
  ...
  LD   moonlight.elf
  STRIP moonlight.prx
  PACK  EBOOT.PBP
```

> **Known harmless warning:** The build prints a warning about a duplicate `moonlight.elf`
> target from PSPSDK's `build.mak`. **This is not an error.** `EBOOT.PBP` is produced
> successfully. Exit code will be 1 from make but all outputs exist.

### Step 3 — Verify Outputs

```bash
ls -lh EBOOT.PBP moonlight.prx moonlight_me_helper/moonlight_me_helper.prx
```

---

## Output Files

| File | Size (approx) | Description |
|---|---|---|
| `EBOOT.PBP` | ~2.5 MB | PSP executable (PARAM.SFO + PRX wrapped) |
| `moonlight.prx` | ~1.5 MB | Stripped PRX module |
| `moonlight_me_helper/moonlight_me_helper.prx` | ~8 KB | Kernel ME bootstrap PRX |

---

## Installing on PSP

**Custom firmware required.** Tested on 6.60 PRO-C2 and 6.61 LME.

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
| `make[1]: *** [moonlight.elf] Error 1` | Duplicate target warning from build.mak | Harmless — check if `EBOOT.PBP` was produced |
| `PRX > 2 MB` | Object files not stripped | Run `psp-strip moonlight.prx` manually |
| Black screen on PSP | ME helper PRX not found | Confirm both files are in the same XMB directory |
| `avcodec.h: No such file` | Legacy FFmpeg path referenced | Legacy path no longer built by default; see `legacy/` |
