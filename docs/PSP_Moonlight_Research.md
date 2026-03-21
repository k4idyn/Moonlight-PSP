# PSP Moonlight Client Research Document

## Executive Summary
This document summarizes research conducted to design a minimal Moonlight fork for the PSP 1000 that utilizes hardware decoding via the Media Engine (ME) and direct VRAM output. The research covers PSP hardware specifications, Moonlight architecture (particularly the moonlight-N3DS implementation), PSP homebrew development ecosystem, available dependencies, memory constraints, networking considerations, and provides a recommended architecture approach.

## 1. PSP 1000 Hardware Specifications

### Core Specifications
- **CPU**: MIPS R4000-based "Allegrex" @ 333MHz (32-bit)
  - Architecture: MIPS32 R4000-based with custom extensions (Allegrex)
  - Instruction Set: MIPS32 Release 2 with Sony custom instructions
  - Cache: 32KB L1 cache (16KB instruction, 16KB data), no L2 cache
  - Features: Integrated FPU (Floating Point Unit) and VFPU (Vector Floating Point Unit)
  - Clock Speed: Variable from 33MHz to 333MHz (capped at 222MHz for licensed software, homebrew can access full 333MHz)
- **Secondary CPU**: Media Engine (ME) - MIPS R4000-based @ 333MHz (handles multimedia decoding)
  - Architecture: MIPS32 R4000-based (functionally equivalent to main CPU but lacks VFPU)
  - Purpose: Dedicated to multimedia decoding (H.264, audio, etc.)
- **RAM**: 32MB main RAM (PSP-1000 model)
  - Memory Architecture: 
    - User/Kernel Split: 2GB each (0x00000000-0x7fffffff user, 0x80000000-0xffffffff kernel)
    - Valid PSP memory range is limited to physical 32MB within this space
    - Memory Protection: User mode cannot access kernel space without exploits
- **VRAM**: 2MB eDRAM for GPU (Graphics Core @ 166MHz)
  - Shared Access: Both main CPU and graphics core can access VRAM
  - Bus: Local bus to prevent congestion between CPU and GE access
- **Audio**: 16-bit stereo @ 48kHz
  - Capabilities: Stereo signed 16-bit audio
  - Supported Codecs: AAC (24kHz for MPEG-4 SP, 48kHz for H.264/AVC), MP3, ATRAC3plus
- **Display**: 480×272 pixels LCD
- **Storage**: Memory Stick Pro Duo (up to 32GB native, expandable to 128GB+ with adapters)

### Key Components
 1. **Main CPU (Allegrex)**:
    - MIPS32 R4000-based architecture
    - Instruction Set: MIPS32 Release 2 with Sony custom instructions
    - Cache: 32KB L1 cache (16KB instruction, 16KB data), no L2 cache
    - Includes FPU (Floating Point Unit) and VFPU (Vector Floating Point Unit)
    - Clock Speed: Variable from 33MHz to 333MHz (capped at 222MHz for licensed software, homebrew can access full 333MHz)

 2. **Media Engine (ME)**:
    - Second MIPS32 R4000-based CPU core @ 333MHz
    - Dedicated to multimedia decoding (H.264, audio, etc.)
    - Contains specialized hardware blocks:
      - AVC (Advanced Video Coding) hardware for H.264 decoding
      - VME (Virtual Mobile Engine) DSP for audio processing
    - Not directly accessible to commercial developers but accessible via homebrew
    - Functionally equivalent to main CPU but lacks VFPU

 3. **Graphics Core (GE)**:
    - Runs at 166MHz
    - 2MB eDRAM VRAM
    - Bus: 256-bit bus width
    - Memory Bandwidth: 5.3Gbps (664 MB/s) bus bandwidth
    - Performance: 664 million pixels per second fill rate, 35 million polygons per second
    - Supports 3D graphics, texture mapping, anti-aliasing, compressed textures
    - Features: 3D curved surface and 3D polygon engine, hardware clipping, morphing, bone, tessellation, bezier
    - Maximum resolution: 480×272 (native LCD resolution)
    - Shared Access: Both main CPU and graphics core can access VRAM via local bus to prevent congestion

 4. **Memory Architecture**:
    - 32MB main RAM (accessible to main CPU)
      - User/Kernel Split: 2GB each theoretically (0x00000000-0x7fffffff user, 0x80000000-0xffffffff kernel)
      - Valid PSP memory range limited to physical 32MB within this space
    - 2MB VRAM (eDRAM) for GPU
    - 2MB audio memory (for ME/VME)
    - 4MB embedded DRAM total (split between GPU and Media Engine)
    - Total: ~36MB accessible memory (32MB main + 4MB eDRAM)

### Hardware Capabilities Relevant to Moonlight
- **Hardware H.264 Decoding**: Via ME's AVC (Advanced Video Coding) block
  - Profiles Supported: Baseline, Main, High (up to H.264/MPEG-4 AVC Main Profile per official specs)
  - Levels: Typically up to Level 2.1 (based on Handbrake encoder level observations for PSP compatibility)
  - Resolution Support: Up to 720x576, 720×480, 352×480, and 480×272 (per Wikipedia)
  - Note: Homebrew access via psp-media-engine-custom-core or reverse-engineered Sony libraries
- **Video Output**: Direct framebuffer access to VRAM
  - Graphics Engine (GE) runs at 166MHz with 2MB eDRAM VRAM
  - Maximum resolution: 480×272 (matches native LCD)
  - Performance: 664 million pixels per second fill rate, 35 million polygons per second
  - Features: 3D rendering, texture mapping, anti-aliasing, compressed textures, hardware clipping
- **Audio Output**: PCM audio via ME/VME or main CPU
  - Capabilities: 16-bit stereo @ 48kHz
  - Supported Codecs: AAC (24kHz for MPEG-4 SP, 48kHz for H.264/AVC), MP3, ATRAC3plus
  - ME contains VME (Virtual Mobile Engine) DSP for audio processing
- **Networking**: 802.11b WiFi (up to 11Mbps theoretical, 4-6Mbps practical)
  - Protocol: IEEE 802.11b
  - Theoretical throughput: 11Mbps
  - Practical throughput: 4-6Mbps (sufficient for lower-bitrate GameStream)
- **Input**: 
  - Digital buttons: △, ○, ×, ☐, L, R, ←, →, ↑, ↓, START, SELECT
  - Analog nub: Single analog stick (PSP-1000/2000/3000 models)
  - Touchscreen: Not present on PSP-1000 (added in PSP-3000/go models)
- **Storage**: 
  - Primary: Memory Stick Pro Duo slot (left side)
  - Capacity: Up to 32GB native, expandable to 128GB+ with adapters (Mark2)
  - Secondary: UMD drive (1.8GB capacity, slower access)
- **OS/Firmware Limitations**:
  - User Mode Restrictions: Homebrew runs in user mode by default
  - Kernel Access: Requires exploit or custom firmware (e.g., 1.50 kernel mode) for full hardware access
  - ME Access: Not officially documented for homebrew but accessible via:
    * psp-media-engine-custom-core library
    * Reverse-engineered Sony libraries from flash0:/kd/
    * RPC/Mechanism approaches (mebooter code)
  - Firmware Compatibility: Some homebrew requires earlier firmware modes (e.g., 1.50) for kernel access
- **Performance Characteristics**:
  - CPU: MIPS R4000-based Allegrex @ 333MHz (homebrew accessible)
  - Memory Bandwidth: 
    - Main RAM: Not explicitly specified in sources, but DDR SDRAM
    - VRAM Bus: 256-bit, 5.3Gbps (664 MB/s) bandwidth
    - Official IGN specs: Main Memory Bus Bandwidth :2.6GB/sec
  - GE Performance: 664M pixels/sec fill rate, 35M polygons/sec
  - ME: Second 333MHz MIPS core for multimedia offloading

## 2. Moonlight Architecture & moonlight-N3DS Blueprint

### Moonlight Overview
Moonlight is an open-source implementation of NVIDIA's GameStream protocol, allowing streaming of games from a GeForce-equipped PC to various clients.

### moonlight-N3DS Specifics
The moonlight-N3DS project (https://github.com/zoeyjodon/moonlight-N3DS) serves as an excellent blueprint for our PSP port due to similar constraints:
- Limited hardware resources
- Custom hardware acceleration requirements
- Homebrew development environment

#### Key Architecture Components
1. **libgamestream**: Core GameStream protocol implementation
   - Handles connection, discovery, encryption, session management
   - Uses mbedtls for TLS 1.2 encryption
   - Uses enet for reliable UDP transport

2. **Video Pipeline**:
   - Receives H.264 encoded video frames from host
   - Decodes using hardware acceleration (on 3DS: OpenMAX IL)
   - Renders decoded frames to screen

3. **Audio Pipeline**:
   - Receives Opus audio packets
   - Decodes Opus to PCM
   - Outputs via audio subsystem

4. **Input Handling**:
   - Captures local input (buttons, touchscreen)
   - Sends to host via control channel

5. **Threading Model**:
   - Network receiver thread
   - Video decode/display thread
   - Audio decode/output thread
   - Input polling thread

#### moonlight-N3DS Adaptations for PSP
- Replace 3DS-specific video rendering with PSP GE/VRAM access
- Replace 3DS audio subsystem with PSP audio ME/VME
- Replace 3DS input handling with PSP button/nub reading
- Maintain core libgamestream networking and protocol handling
- Adapt build system for PSP toolchain (pspsdk)

## 3. PSP Homebrew Development Ecosystem

### Toolchain & SDK
- **PSPSDK**: Primary open-source SDK for PSP homebrew
  - Provides libc, threading, file I/O, display, audio, networking libraries
  - Part of PSPDEV toolchain
  - GitHub: https://github.com/pspdev/pspsdk
- **psptoolchain**: Automated toolchain builder
  - Includes GCC, binutils, GDB for MIPS architecture
  - GitHub: https://github.com/pspdev/psptoolchain

### Available Libraries
1. **Graphics/Video**:
   - `gu` (Graphics Unit) library for 2D/3D rendering
   - Direct framebuffer access via `sceDisplayGetFrameBuf`
   - VRAM access for direct pixel manipulation

2. **Audio**:
   - `sceAudio` library for PCM audio output
   - Low-level audio channel allocation and sampling rate control

3. **Input**:
   - `sceCtrl` for button/analog nub reading
   - `sceTouch` for touchscreen (on supported models)

4. **Networking**:
   - `sceNet` family for WiFi networking
   - Socket-based BSD-like API
   - HTTPS support via mbedtls integration

5. **Utilities**:
   - `sceKernel` for threading, mutexes, memory management
   - `sceUtils` for various system utilities
   - `sceLibc` for standard C library functions

### Media Engine Access
While not officially documented for homebrew, several approaches exist:
1. **psp-media-engine-custom-core** library (https://github.com/mcidclan/psp-media-engine-custom-core)
   - Maps ME native core functions for homebrew use
   - Provides access to AVC (H.264) decoding functions
   - Work-in-progress but functional

2. **Sony's libraries from flash**:
   - Some homebrew projects access Sony's proprietary ME libraries
   - Located in PSP flash0:/kd/ or similar
   - Requires reverse engineering but provides full ME access

3. **RPC/Mechanism approaches**:
   - mebooter code provides RPC system to control ME codecs
   - Allows loading homebrew code onto ME for custom processing

## 4. Prebuilt Dependencies & Hardware Acceleration Options

### FFmpeg for PSP
Several PSP-optimized FFmpeg builds exist:
1. **PPSSPP FFmpeg** (https://github.com/hrydgard/ppsspp-ffmpeg)
   - Slimmed-down build used in PPSSPP emulator
   - Includes H.264, AAC, Atrac3+ codecs
   - Software-based decoding only

2. **Custom builds with ME acceleration**:
   - Theoretically possible to compile FFmpeg with ME hardware acceleration
   - Would require custom hwaccel implementation for ME's AVC block
   - No known public implementations exist

### Audio Codecs
- **Opus**: Well-suited for low-bitrate audio
  - Fixed-point implementations available for embedded systems
  - Can be compiled with pspsdk
  - Alternative: Use Sony's native ATRAC3+ if licensing permits

### Other Libraries
- **mbedtls**: Already present in workspace (mbedtls-4.0.0/)
  - Can be cross-compiled for PSP using pspsdk
  - Used in existing PSP TLS applications
- **enet**: Already present in workspace (enet-master/)
  - Simple reliable UDP library
  - Used in moonlight-common-c
  - Easily portable to PSP

### Hardware Acceleration Reality Check
- **ME's AVC block**: Capable of H.264 Baseline/Main/High profile decoding
- **Resolution limits**: Up to 1920×1080 (though practical limits lower)
- **Bitrate limits**: Depends on implementation, but should handle GameStream bitrates
- **Frame rate**: Should support 30-60fps typical for GameStream

## 5. Memory Constraints Analysis (32MB Limit)

### Memory Budget Allocation
| Component | Estimated Size | Notes |
|-----------|----------------|-------|
| **Code & Static Data** | 4-6MB | libgamestream, ME interface, audio/video decoders |
| **Heap/Dynamic Allocation** | 8-10MB | Video buffers, audio buffers, network packets |
| **Video Framebuffers** | 4-6MB | Multiple H.264 frames (YPUV420 format) |
| **Audio Buffers** | 1-2MB | Opus packets, PCM buffers |
| **Networking Buffers** | 1-2MB | enet packets, mbedtls contexts |
| **OS/PSP SDK Overhead** | 2-4MB | Threading, syscalls, library reserves |
| **Stack Space** | 1-2MB | Multiple threads |
| **Total** | 21-32MB | Leaves room for optimization |

### Optimization Strategies
1. **Video Buffer Management**:
   - Use double/triple buffering for decoded frames
   - Reuse buffers instead of allocating/freeing
   - Consider YUV420 to RGB conversion in hardware if possible

2. **Audio Buffer Optimization**:
   - Small Opus packets (20ms frames)
   - Direct PCM output to audio channels
   - Minimal resampling if needed

3. **Networking Efficiency**:
   - enet already optimized for low memory usage
   - mbedtls can be configured for smaller stack usage
   - Buffer pooling for network packets

4. **Code Size Reduction**:
   - Strip unused symbols from libgamestream
   - Use -Os optimization flag
   - Remove debug logging in release builds

5. **ME Offloading**:
   - Offload H.264 decoding to ME frees main CPU cycles
   - ME has its own memory subsystem (2MB audio mem)
   - Main CPU only handles networking, input, frame display

## 6. Networking & Security Layers

### Existing Components in Workspace
- **enet-master**: Reliable UDP library used by moonlight-common-c
- **mbedtls-4.0.0**: TLS 1.2 library for GameStream encryption

### Adaptation Requirements
1. **enet Porting**:
   - Already uses standard C and POSIX-like sockets
   - Should compile with pspsdk's net libraries
   - May need to adapt socket error handling

2. **mbedtls Porting**:
   - Workspace shows mbedtls-4.0.0 directory
   - Needs cross-compilation for MIPS PSP
   - Configuration options to reduce footprint:
     - Disable unused cipher suites
     - Reduce buffer sizes
     - Optimize for speed over memory if needed

3. **GameStream Protocol Specifics**:
   - Initial connection: HTTPS to discover host
   - Session setup: RTSP-like over TLS
   - Data channels: enet over UDP (ports 47999, 48000, 48001, 48002)
   - Encryption: AES-128 GCM for data channels

### PSP Networking Considerations
- **WiFi 802.11b**: Theoretical 11Mbps, practical 4-6Mbps
- **Latency**: Typically 30-100ms local network
- **Reliability**: Packet loss possible, enet handles retransmission
- **Power Management**: WiFi can be power-cycled to save battery

## 7. Recommended Architecture Approach

### High-Level Design
```
+---------------------+     +---------------------+     +---------------------+
|  Network Thread     |     |  Video Thread       |     |  Audio Thread       |
| (enet + mbedtls)    |     | (ME H.264 Decode)   |     | (Opus Decode +     |
| - Host Discovery    |     | - Frame Queue       |     |  Audio Output)      |
| - Session Setup     |     | - ME Interface      |     | - Audio Buffers     |
| - Keepalive Packets |     | - VRAM Output       |     |                     |
+---------------------+     +---------------------+     +---------------------+
          ^                         ^                         ^
          |                         |                         |
          |     Control Channel     |     Video Frames        |     Audio Packets
          v                         v                         v
+---------------------------------------------------------------------+
|                    Core libgamestream (moonlight-common-c)          |
|  - Connection Management                                           |
|  - Session Handling                                                |
|  - Message Parsing                                                 |
|  - Thread Coordination                                             |
+---------------------------------------------------------------------+
          ^                         ^                         ^
          |                         |                         |
          |     Input Events        |     Decoded Frames      |     PCM Samples
          v                         v                         v
+---------------------+     +---------------------+     +---------------------+
|   Input Thread      |     |  Display Thread     |     |  Audio Thread       |
| (Button/Nub Read)   |     | (VSYNC Wait)        |     | (Already listed)    |
| - Local Input       |     | - Frame Swap        |     |                     |
| - Host Reporting    |     | - VBlank Sync       |     |                     |
+---------------------+     +---------------------+     +---------------------+
```

### Detailed Component Implementation

#### 1. Network Layer (enet + mbedtls)
- Use existing moonlight-common-c Network.cpp/PlatformSockets.c
- Adapt socket calls to PSP's `sceNet` API
- Maintain enet for reliable UDP transport
- Use mbedtls for TLS 1.2 encryption (handshake + record layer)

#### 2. Video Pipeline
**Input**: H.264 NAL units from libgamestream RTP video queue
**Processing**:
- Option A (Preferred): Offload to ME via custom ME interface
  - Initialize ME AVC decoder
  - Feed NAL units to ME
  - Retrieve decoded YUV frames from ME
- Option B (Fallback): Software decode with optimized H.264
  - Use libavcodec from FFmpeg with ARM optimizations
  - Less ideal due to CPU usage

**Output**:
- Convert YUV420 to RGB565 (PSP native format)
- Direct VRAM write via `sceDisplayGetFrameBuf`
- VSYNC synchronization for tear-free output

#### 3. Audio Pipeline
**Input**: Opus packets from libgamestream RTP audio queue
**Processing**:
- Decode Opus to PCM using fixed-point opus library
- Optional: Resample to 48kHz if needed
**Output**:
- PCM audio via `sceAudioOutput` blocking or streaming mode
- Stereo 16-bit @ 48kHz

#### 4. Input Handling
- Poll `sceCtrl` for buttons and analog nub
- Map to GameStream controller format
- Send via control channel (libgamestream ControlStream)
- Optional: Touchscreen support for later PSP models

#### 5. Threading & Synchronization
- Use `sceKernel` for thread creation and synchronization
- Mutexes for shared buffers
- Condition variables for producer/consumer queues
- Priority-based scheduling (video/audio higher than network)

### Memory Management Strategy
- Pre-allocate all major buffers at startup
- Use memory pools for frequent allocations (network packets, video frames)
- Implement buffer recycling to minimize fragmentation
- Monitor free memory with `sceKernelGetFreeMemorySize`

### Build System
- Adapt moonlight-N3DS CMakeLists.txt for PSP
- Use pspsdk's build system (makefile-based)
- Create PSP-specific CMake toolchain file
- Output: EBOOT.PBP for PSP execution

## 8. Risks & Mitigations

### Technical Risks
1. **ME Access Difficulty**:
   - Risk: Unable to reliably access ME's AVC decoding functions
   - Mitigation: Start with software H.264 decode (optimized), add ME later
   - Alternative: Use PPSSPP's H.264 implementation as reference

2. **Performance Insufficiency**:
   - Risk: Main CPU overloaded with networking + software decode + output
   - Mitigation: Profile early, optimize critical paths, consider fixed-point math
   - Mitigation: Offload as much as possible to ME

3. **Memory Limitations**:
   - Risk: Exceeding 32MB limit with buffers and overhead
   - Mitigation: Strict memory budgeting, memory profiling, buffer sharing
   - Mitigation: Reduce video buffer count if necessary

4. **WiFi Bandwidth Constraints**:
   - Risk: GameStream bitrate exceeds WiFi capacity
   - Mitigation: Lower quality settings on host, optimize enet parameters
   - Mitigation: Implement adaptive bitrate if protocol allows

### Development Risks
1. **Limited Documentation**:
   - Risk: ME hardware registers undocumented for homebrew
   - Mitigation: Leverage existing homebrew ME libraries, reverse engineering
   - Mitigation: Start with software decode path

2. **Toolchain Complexity**:
   - Risk: Difficulty setting up PSP development environment
   - Mitigation: Use psptoolchain automated builder
   - Mitigation: Document build process thoroughly

## 9. Recommended Next Steps

### Phase 1: Foundation (Weeks 1-2)
1. Set up PSP development environment (psptoolchain + pspsdk)
2. Port enet and mbedtls to PSP (verify basic networking)
3. Create minimal "hello world" PSP application with framebuffer access
4. Study libgamestream structure from moonlight-common-c

### Phase 2: Core Networking (Weeks 3-4)
1. Implement basic GameStream client using moonlight-common-c
2. Establish TLS connection to host
3. Achieve successful session setup (without video/audio)
4. Implement input reporting to host

### Phase 3: Video Pipeline (Weeks 5-6)
1. Implement software H.264 decode baseline (FFmpeg or libavcodec)
2. Display decoded frames to VRAM
3. Achieve low-latency video streaming (even if choppy)
4. Begin ME interface investigation

### Phase 4: Audio & Optimization (Weeks 7-8)
1. Implement Opus decode and audio output
2. Synchronize audio-video playback
3. Optimize memory usage and performance
4. Integrate ME hardware acceleration if feasible

### Phase 5: Polish & Testing (Weeks 9-10)
1. Battery life optimization
2. Input latency reduction
3. Compatibility testing with various games
4. Documentation and cleanup

## 10. Conclusion

A PSP 1000 Moonlight client leveraging hardware Media Engine decoding is feasible within the 32MB memory constraint. The key to success lies in:

1. **Leveraging existing work**: moonlight-N3DS provides proven architecture
2. **Strategic offloading**: Move H.264 decoding to ME to free main CPU
3. **Careful memory management**: Pre-allocate and recycle buffers
4. **Adaptation of proven components**: enet and mbedtls already exist in workspace
5. **Phased approach**: Start with software decode, add hardware acceleration later

The PSP's unique heterogeneous architecture (main CPU + ME) aligns well with Moonlight's modular pipeline design. While challenging, particularly regarding ME access, the project has a clear path to success using established homebrew development practices.

---
*Research conducted using Brave Search tool and analysis of provided workspace components.*