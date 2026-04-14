# OpenH264 PSP Port — Build & Integration Guide

## Overview

This document describes how to cross-compile the **decoder-only** OpenH264 2.x
library for the Sony PSP (PlayStation Portable) using the
[pspdev](https://github.com/pspdev/pspdev) toolchain, and how to integrate
it into a [Moonlight-PSP](https://github.com/moonlight-stream) game streaming
client.

---

## Why OpenH264 2.x for PSP?

| Feature | Benefit |
|---|---|
| Constrained Baseline Profile | Matches exactly what Sunshine / Nvidia GameStream sends; no B-frames, no cabac complexity path differences |
| `DecodeFrameNoDelay()` | Zero reorder-buffer latency — essential for interactive streaming |
| Single-threaded C fallback | The Allegrex is a single MIPS32r2 core; no SIMD (MMI/MSA/NEON) is used |
| Refined memory allocator | 2.x decoder context allocations have less fragmentation on 32 MB PSP-1000 |
| Decoder-only build | Library < 1 MB thanks to `DISABLE_ENCODER_SIDE` + `DEBUGLVL=0` |

---

## Hardware

| Model | RAM | CPU speed | Recommended stream res |
|---|---|---|---|
| PSP-1000 (Fat) | 32 MB | 222–333 MHz | 480×272 @ 30 fps |
| PSP-2000 / 3000 | 64 MB | 333 MHz | 480×272 @ 60 fps or 720×408 @ 30 fps |
| PSP Go | 64 MB | 333 MHz | same as 2000/3000 |

Set your Moonlight host to **Constrained Baseline, Level 3.1** and a bitrate
of 2–4 Mbit/s for best results on the PSP-1000.

---

## Prerequisites

1. **pspdev toolchain** — includes `psp-gcc`, `psp-g++`, `psp-ar`, `psp-config`

   ```sh
   # On Linux/macOS (official installer):
   git clone https://github.com/pspdev/pspdev.git
   cd pspdev && ./build-all.sh
   # Then add pspdev/bin to PATH
   ```

2. **GNU Make 3.81+**

3. A shell that provides `sh` (bash or dash).  The version header generator
   `codec/common/generate_version.sh` requires a POSIX shell.

---

## Building the decoder library

From the repository root:

```sh
# Generate version header first (one-time setup):
sh codec/common/generate_version.sh .

# Build the static library:
make -f Makefile.psp

# Result: libopenh264_dec_psp.a
```

For a debug build with logging:

```sh
make -f Makefile.psp BUILDTYPE=Debug
```

### Verified compiler flags

| Flag | Reason |
|---|---|
| `-march=allegrex` | Target Allegrex ISA (MIPS32r2-derived) |
| `-mabi=eabi` | PSP EABI calling convention |
| `-G0` | Disable GP-relative addressing (required by PSPSDK) |
| `-DPSP` | Enables PSP-specific code paths in this port |
| `-DDISABLE_DECODER_MT` | Forces single-threaded decode; no per-thread context overhead |
| `-DDISABLE_ENCODER_SIDE` | Strips encoder headers from the build entirely |
| `-O3 -DNDEBUG` | Release: disables all trace/assert overhead |

---

## Integrating with Moonlight-PSP

### Step 1 — Include the wrapper

```c
#include "psp/moonlight_openh264.h"
```

### Step 2 — Create and initialise a decoder

```c
MoonH264Decoder *dec = moonh264_create();
if (!dec) { /* out of memory */ }

// Use your stream's actual resolution here.
if (moonh264_init(dec, 480, 272) != MOONH264_OK) {
    moonh264_destroy(dec);
    /* handle error */
}
```

### Step 3 — Decode each received NALU

Moonlight delivers Annex-B framed NALUs (start-code prefixed — no conversion
needed).

```c
MoonH264Frame frame = {0};
int rc = moonh264_decode(dec, nalu_buf, nalu_len, &frame);
if (rc == MOONH264_OK && frame.got_picture) {
    // frame.y / frame.u / frame.v are planar YUV 4:2:0
    // Upload to GU via sceGuCopyImage or a custom swizzle path.
}
```

### Step 4 — Handle video re-sync events

When Moonlight requests a new IDR (e.g. after packet loss):

```c
moonh264_flush(dec);
// The next IDR packet will be decoded cleanly.
```

### Step 5 — Tear down

```c
moonh264_destroy(dec);
dec = NULL;
```

---

## Linking in your PSP project Makefile

```makefile
OPENH264_DIR = ../openh264-master

INCLUDES += -I$(OPENH264_DIR)/codec/api/wels \
            -I$(OPENH264_DIR)/psp

LIBS += $(OPENH264_DIR)/libopenh264_dec_psp.a \
        -lstdc++ -lc -lpthread
```

Also compile and link the wrapper:

```makefile
EXTRA_OBJS += $(OPENH264_DIR)/psp/moonlight_openh264.o

$(OPENH264_DIR)/psp/moonlight_openh264.o: $(OPENH264_DIR)/psp/moonlight_openh264.cpp
	psp-g++ $(CXXFLAGS) $(INCLUDES) \
	  -I$(OPENH264_DIR)/codec/common/inc \
	  -I$(OPENH264_DIR)/codec/decoder/core/inc \
	  -c $< -o $@
```

---

## Threading model

The PSP Allegrex is a **single-core CPU**.  The Media Engine (ME) is a
separate co-processor accessible only through Sony's ME libraries — it is
**not** exposed via pthreads and cannot run OpenH264 decode tasks.

`DISABLE_DECODER_MT` ensures OpenH264's internal thread pool is disabled at
compile time.  The decoder runs entirely on the main Allegrex core.

The `WelsThreadLib` implementation has been patched in this port to:

- Route the PSP (`__psp__`) through the `pthread_cond_t`-based event path,
  identical to the Apple/iOS path, because PSP newlib does not support POSIX
  named semaphores (`sem_open` / `sem_close`).
- Skip `SCHED_FIFO` and `PTHREAD_SCOPE_SYSTEM` thread attributes that are
  not available in PSP user-mode pthreads.
- Return `ProcessorCount = 1` from `WelsQueryLogicalProcessInfo()`.

---

## Performance notes

| Metric | PSP-1000 estimate |
|---|---|
| 480×272 @ 30 fps Baseline decode | ~80–100 % Allegrex at 222 MHz |
| 480×272 @ 30 fps Baseline decode | ~60–70 % Allegrex at 333 MHz (recommended) |

Set CPU speed to 333 MHz in your PSP project:

```c
#include <psppower.h>
scePowerSetClockFrequency(333, 333, 166);
```

Consider using the PSP Media Engine for YUV→RGB conversion to offload the
Allegrex for network receive and GU blitting.

---

## Files added by this port

```
build/platform-psp.mk           PSP platform Makefile fragment
Makefile.psp                    Decoder-only build entry point
codec/common/src/WelsThreadLib.cpp  Patched: PSP threading support
codec/common/inc/WelsThreadLib.h    Patched: PSP event typedef
psp/moonlight_openh264.h        Public C API for Moonlight-PSP
psp/moonlight_openh264.cpp      Implementation of the public C API
psp/README_PSP_BUILD.md         This file
```
