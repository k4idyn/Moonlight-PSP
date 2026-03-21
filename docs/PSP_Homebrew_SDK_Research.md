# PSP Homebrew SDK and Multimedia Libraries Research

## 1. PSPSDK Overview

### What it includes:
- Open-source SDK for PSP homebrew development
- Provides libc, threading, file I/O, display, audio, networking libraries
- Part of PSPDEV toolchain
- GitHub: https://github.com/pspdev/pspsdk

### Version status:
- Active development on GitHub
- Regular releases available
- Binary distributions available for various platforms

### Homebrew development setup:
- Requires psptoolchain (automated toolchain builder)
- Includes GCC, binutils, GDB for MIPS architecture
- Installation guides available at https://pspdev.github.io/
- Dependencies: GNU autoconf, automake, zlib development libraries

## 2. PSP Graphics/Display Libraries

### Graphics Utility (GU) Library:
- Primary 2D/3D rendering library for PSP
- Functions: gu_init, gu_draw_buffer, gu_display_on, sceGu* functions
- Direct framebuffer access via `sceDisplayGetFrameBuf`
- VRAM access for direct pixel manipulation
- Documentation: https://pspdev.github.io/pspsdk/group__GU.html

### Framebuffer Access:
- Tutorial available: https://psp-dev.org/doku.php?id=tutorial:framebuffer_graphics
- Supports double buffering for smooth rendering
- Can access VRAM directly for pixel manipulation
- PSP native format: RGB565 (16-bit color)

## 3. PSP Audio Libraries

### sceAudio Library:
- Primary audio output library for PSP
- Functions: sceAudioOutput, sceAudioOutputBlocking
- Low-level audio channel allocation and sampling rate control
- Header: pspsdk/src/audio/sceAudio.S

### Additional Audio Options:
- libmad: MPEG audio decoder library (documentation in Japanese)
- pspaudiolib/pspaudio: Access to PSP audio hardware
- Opus: Fixed-point implementations available for embedded systems
- ATRAC3plus: Native Sony codec (licensing considerations)
- Homebrew projects show Opus audio playback support

## 4. PSP Input Handling

### sceCtrl Library:
- Primary input handling for PSP
- Functions: sceCtrlSetSamplingCycle, sceCtrlSetSamplingMode, sceCtrlReadBufferPositive
- Reads buttons: △, ○, ×, ☐, L, R, ←, →, ↑, ↓, START, SELECT
- Reads analog nub (single analog stick on PSP-1000/2000/3000)
- Header: pspsdk/src/ctrl/pspctrl.h

### Features:
- Analog mode support for reading analog stick position
- Digital mode for button-only input
- Hold button handling
- Button remapping capabilities demonstrated in homebrew

## 5. PSP Networking Libraries

### sceNet Family:
- PSP's networking API (BSD-like socket interface)
- WiFi 802.11b support
- Functions for AP control, socket creation, etc.
- Used in PPSSPP emulator for HLE (High-Level Emulation)

### Existing Libraries in Workspace:
- **ENET** (enet-master/): Reliable UDP library used by moonlight-common-c
  - Should compile with pspsdk's net libraries
  - May need to adapt socket error handling
- **mbedTLS** (mbedtls-4.0.0/): TLS 1.2 library for GameStream encryption
  - Needs cross-compilation for MIPS PSP
  - Configuration options to reduce footprint available

### Networking Considerations:
- WiFi 802.11b: Theoretical 11Mbps, practical 4-6Mbps
- Latency: Typically 30-100ms local network
- Reliability: Packet loss possible, ENET handles retransmission
- Power Management: WiFi can be power-cycled to save battery

## 6. Media Engine Access for Homebrew

### Approaches to Access ME:
1. **psp-media-engine-custom-core** library
   - GitHub: https://github.com/mcidclan/psp-media-engine-custom-core
   - Maps ME native core functions for homebrew use
   - Provides access to AVC (H.264) decoding functions
   - Work-in-progress but functional

2. **Sony's libraries from flash**
   - Located in PSP flash0:/kd/ or similar
   - Requires reverse engineering but provides full ME access
   - Some homebrew projects access these proprietary libraries

3. **RPC/Mechanism approaches**
   - mebooter code provides RPC system to control ME codecs
   - Allows loading homebrew code onto ME for custom processing

### ME Capabilities:
- Second MIPS32 R4000-based CPU core @ 333MHz
- Dedicated to multimedia decoding (H.264, audio, etc.)
- Contains specialized hardware blocks:
  - AVC (Advanced Video Coding) hardware for H.264 decoding
  - VME (Virtual Mobile Engine) DSP for audio processing
- Functionally equivalent to main CPU but lacks VFPU

## 7. Existing PSP Homebrew with Video Decoding

### Notable Projects:
- **PSP Media Center**: H.264 video support (using Sony's libraries from flash)
- **PMFplay H.264 Decoder**: Codec for playing PMF files
- **PSP Media Engine Accessed**: Sample code by "crazyc" demonstrating ME usage
- Homebrew forums show projects attempting H.264 playback

### Technical Details:
- H.264 Baseline/Main/High profile decoding supported
- Resolution support: Up to 720x576, 720×480, 352×480, 480×272
- Bitrate and frame rate should support typical GameStream requirements
- Hardware acceleration via ME's AVC block significantly reduces CPU load

## 8. PSP Toolchain Details

### Compiler and Tools:
- **GCC**: Version varies with psptoolchain build (typically 4.x series)
- **Binutils**: GNU binutils for MIPS architecture
- **Newlib**: C library implementation used in pspsdk
- **GDB**: MIPS debugger available

### Build System:
- Makefile-based system in pspsdk
- Supports creation of EBOOT.PBP files for PSP execution
- Can generate both .cia (installable) and .3dsx (homebrew launcher) formats
- Docker builds available for consistent environment

## 9. Memory Management for Homebrew

### Memory Architecture:
- **Main RAM**: 32MB total (PSP-1000 model)
- **VRAM**: 2MB eDRAM for GPU
- **Audio Memory**: 2MB for ME/VME
- **Total accessible**: ~36MB (32MB main + 4MB eDRAM)

### User/Kernel Split:
- Theoretical 2GB each (0x00000000-0x7fffffff user, 0x80000000-0xffffffff kernel)
- Valid PSP memory range limited to physical 32MB within this space
- User mode cannot access kernel space without exploits

### Available Memory for Homebrew:
- Forum discussions suggest ~24MB available for applications
- 8MB typically reserved for kernel space
- Memory protection can be disabled to access full range (requires kernel mode)

### Memory Allocation Functions:
- `sceKernelAllocMemBlock`: Allocate memory blocks
- `sceKernelFreeMemBlock`: Free memory blocks
- `sceKernelTotalFreeMemSize()`: Get total free memory
- `sceKernelMaxFreeMemSize()`: Get maximum free memory block
- `linearAlloc()`: For contiguous physical memory allocation (similar to 3DS)

### Optimization Strategies:
- Pre-allocate major buffers at startup
- Use memory pools for frequent allocations
- Implement buffer recycling to minimize fragmentation
- Monitor free memory with `sceKernelGetFreeMemorySize`
- Double/triple buffering for video frames
- Reuse buffers instead of allocating/freeing

## 10. Popular PSP Homebrew Libraries

### Beyond PSPSDK Core:
- **psplib**: Community-developed library collection
- **pspsdk extras**: Additional functionality built on top of pspsdk
- **OSLIB** (OldSchool Library): Good for 2D homebrew
- **triEngine**: 3D engine for PSP
- **JGE++**: 2D game engine with hardware acceleration
- **PSPSeq**: Music composition homebrew

### Media-Specific Libraries:
- **psp-media-engine-custom-core**: For ME access
- **libmad**: MP3 decoding
- **ffmpeg-psp**: PSP-optimized FFmpeg builds
- **ppsspp-ffmpeg**: Slimmed-down build used in PPSSPP emulator

## 11. Development Recommendations

### For a PSP Moonlight Client:
1. **Toolchain Setup**: Use psptoolchain to build cross-compilation environment
2. **Networking**: Port ENET and mbedTLS from existing workspace
3. **Video Pipeline**: 
   - Primary: Offload H.264 decoding to ME via psp-media-engine-custom-core
   - Fallback: Software decode with optimized H.264 (libavcodec from FFmpeg)
   - Output: Convert YUV420 to RGB565, direct VRAM write via sceDisplayGetFrameBuf
4. **Audio**: Decode Opus to PCM, output via sceAudio
5. **Input**: Use sceCtrl for button/analog nub reading
6. **Threading**: Use sceKernel for thread creation and synchronization
7. **Memory Management**: Pre-allocate buffers, implement pooling strategies
8. **Build System**: Adapt moonlight-N3DS CMakeLists.txt for PSP makefile-based system

### Memory Budget Allocation (Estimated):
- Code & Static Data: 4-6MB
- Heap/Dynamic Allocation: 8-10MB
- Video Framebuffers: 4-6MB
- Audio Buffers: 1-2MB
- Networking Buffers: 1-2MB
- OS/PSP SDK Overhead: 2-4MB
- Stack Space: 1-2MB
- Total: 21-32MB (leaves room for optimization)

## 12. References and Resources

### Key Repositories:
- PSPSDK: https://github.com/pspdev/pspsdk
- Psptoolchain: https://github.com/pspdev/psptoolchain
- Psp-media-engine-custom-core: https://github.com/mcidclan/psp-media-engine-custom-core
- PSPDev Package Index: https://pspdev.github.io/psp-packages/

### Documentation:
- PSPSDK Documentation: https://pspdev.github.io/pspsdk/
- PSP Developer Wiki: https://www.psdevwiki.com/psp/
- PSP Programming Wikibooks: https://en.wikibooks.org/wiki/PSP_Programming
- Forum Discussions: https://forums.ps2dev.org/

### Example Projects:
- moonlight-N3DS: https://github.com/zoeyjodon/moonlight-N3DS (architecture reference)
- PPSSPP: https://github.com/hrydgard/ppsspp (PSP emulator with HLE)