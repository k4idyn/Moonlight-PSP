# PSP Moonlight Client Architecture

## Executive Summary

This document details the architecture for a PSP-specific Moonlight fork that streams NVIDIA GameStream content to the PlayStation Portable (PSP-1000) leveraging hardware acceleration where possible. The design addresses the PSP's unique heterogeneous architecture (main CPU + Media Engine), severe memory constraints (32MB total RAM, ~24MB user space), and homebrew development limitations.

## System Overview

The PSP Moonlight client follows a modular pipeline architecture with specialized threads for each functional component, communicating through optimized inter-process communication mechanisms suitable for the PSP's single-core MIPS architecture.

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

## Module Specifications

### 1. Network Receiver Module

**Purpose**: Handle GameStream protocol communication with host PC using enet over PSP's networking stack.

**Key Responsibilities**:
- Host discovery via mDNS/UPnP
- TLS 1.2 session establishment using mbedtls
- Reliable UDP transport via enet for GameStream data channels
- Control stream communication (input reporting)
- Keepalive and connection maintenance

**PSP-Specific Implementation**:
- Uses `sceNet` API for socket operations
- enet socket callbacks wrap sceNet functions:
  ```c
  ENetSocket enet_socket_wrap(void *address) {
      return sceNetSocket(AF_INET, SOCK_DGRAM, 0);
  }
  
  int enet_socket_send(ENetSocket socket, const void *data, size_t dataSize, 
                      const ENetAddress *address) {
      // Convert ENetAddress to sockaddr_in
      return sceNetSendto(socket, data, dataSize, 0, (struct sockaddr *)&addr, sizeof(addr));
  }
  ```
- mbedtls net callbacks similarly wrap sceNet socket operations
- Memory-optimized mbedtls configuration:
  - `MBEDTLS_SSL_MAX_CONTENT_LEN = 4096` (reduces RAM usage from 32KB to 8KB per session)
  - Static memory allocation for TLS buffers
  - Disabled unused cipher suites

**Interfaces**:
- Input: Raw network packets from sceNet
- Output: 
  - H.264 NAL units to video module (via RTP video queue)
  - Opus packets to audio module (via RTP audio queue)
  - Control events to/from input module (via control stream)
  - Status/events to main application loop

**Threading**: Dedicated network thread with moderate stack size (64KB)

### 2. Video Decoder Module

**Purpose**: Decode H.264 video frames using PSP's Media Engine hardware acceleration and deliver to VRAM for display.

**Key Responsibilities**:
- Receive H.264 NAL units from network module
- Interface with PSP Media Engine's AVC decoding block
- Manage frame buffer queueing and VRAM output
- Handle YUV420 to RGB565 color conversion
- Synchronize with display refresh (VBlank)

**PSP-Specific Implementation**:
- **Primary Path (Hardware Accelerated)**:
  - Uses `psp-media-engine-custom-core` library to access ME's AVC functions
  - Initializes ME AVC decoder with appropriate profile/level constraints
  - Feeds NAL units to ME via exposed API
  - Retrieves decoded YUV420 frames from ME
  - Performs YUV420 to RGB565 conversion (software or via VME if available)
  - Writes directly to VRAM via `sceDisplayGetFrameBuf`
  
- **Fallback Path (Software Decode)**:
  - Optimized H.264 baseline/profile decoder (libavcodec from FFmpeg)
  - Used only if ME access proves infeasible
  - Significantly higher CPU usage but functional

**Memory Management**:
- Pre-allocated frame buffers (double/triple buffering)
- YUV420 frame size: 512x272x1.5 bytes = ~209KB per frame
- Triple buffering: ~627KB for video frames
- Reuse buffers instead of allocate/free cycle

**Interfaces**:
- Input: H.264 NAL units from network module (via RTP video queue)
- Output: 
  - Decoded frames to display module (via frame queue)
  - Status/events to main application loop

**Threading**: Dedicated video thread with moderate stack size (64KB), higher priority than network

### 3. Audio Decoder Module

**Purpose**: Decode Opus audio packets to PCM and output via PSP's audio subsystem.

**Key Responsibilities**:
- Receive Opus packets from network module
- Decode Opus to PCM using fixed-point Opus library
- Manage audio buffer queueing
- Output PCM audio via sceAudio at 48kHz stereo

**PSP-Specific Implementation**:
- Uses fixed-point Opus library for MIPS efficiency
- Decoder initialized once, reused for multiple streams
- Frame size: 20ms (960 samples) - good balance of latency/efficiency
- Output via `sceAudioOutputBlocking` for guaranteed sample delivery
- Stereo 16-bit PCM at 48kHz (native PSP support)

**Memory Management**:
- Opus decoder state: ~4-8KB (stereo)
- Input packet buffer: 4KB
- Output PCM buffer: 16KB (double buffered = 32KB)
- Total audio memory: <50KB

**Interfaces**:
- Input: Opus packets from network module (via RTP audio queue)
- Output:
  - PCM samples to audio output (via sceAudio)
  - Status/events to main application loop

**Threading**: Dedicated audio thread with moderate stack size (64KB), high priority for low latency

### 4. Input Mapper Module

**Purpose**: Read PSP controls and map to GameStream controller format for transmission to host.

**Key Responsibilities**:
- Poll PSP controls (buttons, analog nub) via sceCtrl
- Map to GameStream controller input format
- Send input events via control stream to network module
- Handle button remapping and analog calibration
- Optional: Touchscreen support (for PSP-3000/go)

**PSP-Specific Implementation**:
- Uses `sceCtrlSetSamplingCycle` and `sceCtrlReadBufferPositive`
- Digital mode for button input, analog mode for nub reading
- Button mapping: PSP buttons → GameStream button equivalents
- Analog nub mapping: PSP analog stick → GameStream right analog stick
- Deadzone handling for analog input
- Sampling rate: 60Hz (matches typical game input polling)

**Memory Management**:
- Minimal: Only needs current and previous ctrl state buffers
- ~128 bytes for ctrl state storage

**Interfaces**:
- Input: Raw ctrl data from hardware
- Output:
  - Controller events to network module (via control stream)
  - Status/events to main application loop

**Threading**: Dedicated input thread with small stack size (32KB), moderate priority

### 5. Render Pipeline Module

**Purpose**: Manage frame display with proper synchronization to prevent tearing and ensure smooth output.

**Key Responsibilities**:
- Wait for VBlank synchronization
- Manage frame buffer swapping (double/triple buffering)
- Coordinate with video module for frame delivery
- Handle display initialization and shutdown
- Manage VRAM access conflicts

**PSP-Specific Implementation**:
- Uses `sceDisplayWaitVblankStart()` for synchronization
- Double buffering strategy:
  - Front buffer: Currently displayed
  - Back buffer: Being filled by video module
  - Swap on VBlank to prevent tearing
- VRAM access via `sceDisplayGetFrameBuf` (returns pointer to current back buffer)
- GE (Graphics Engine) configuration for RGB565 output
- Optional: Triple buffering for variable frame rates

**Memory Management**:
- Frame buffers allocated in VRAM (2MB eDRAM)
- 512x272x2 bytes (RGB565) = 278,528 bytes (~272KB) per buffer
- Double buffering: ~544KB VRAM usage
- Leaves ~1.5MB VRAM for other GE operations (textures, etc.)

**Interfaces**:
- Input: Decoded frames from video module (via frame queue)
- Output:
  - Visual output to LCD display
  - Status/events to main application loop

**Threading**: Can be part of video thread or separate display thread with small stack size (32KB)

## Memory Layout & Allocation Strategy

### Memory Constraints (PSP-1000)
- Total RAM: 32MB
- Kernel space: ~8MB reserved
- User space: ~24MB available for applications
- VRAM: 2MB eDRAM (separate from main RAM)

### Recommended Memory Budget Allocation

| Component | Allocation | Notes |
|-----------|------------|-------|
| **Code & Static Data** | 4-6MB | libgamestream, ME interface, audio/video decoders |
| **Heap/Dynamic Allocation** | 8-10MB | Video buffers, audio buffers, network packets |
| **Video Framebuffers** | 4-6MB | Multiple H.264 frames (YPUV420 format) in main RAM |
| **Audio Buffers** | 1-2MB | Opus packets, PCM buffers |
| **Networking Buffers** | 1-2MB | enet packets, mbedtls contexts (optimized) |
| **OS/PSP SDK Overhead** | 2-4MB | Threading, syscalls, library reserves |
| **Stack Space** | 1-2MB | Multiple threads (6 threads × 256-512KB) |
| **VRAM Usage** | 512KB-1MB | Double/triple framebuffers (RGB565 format) |
| **Total** | 21-32MB | Leaves room for optimization and fragmentation |

### Memory Management Techniques

1. **Pre-allocation**: All major buffers allocated at startup
2. **Memory Pools**: 
   - ENet packets (fixed-size objects)
   - Video frame buffers (YUV420 format)
   - Audio input/output packets
   - TLS session contexts
3. **Object Reuse**: Recycle buffers instead of free/alloc cycle
4. **Buffer Sharing**: Where safe, share buffers between modules
5. **Alignment**: Proper alignment for VRAM/GE access (64-byte boundaries)
6. **Monitoring**: Use `sceKernelGetFreeMemorySize()` and `sceKernelMaxFreeMemSize()` to detect fragmentation

### VRAM-Specific Considerations
- VRAM: 2MB eDRAM @ 5.3Gbps bandwidth
- Shared access: Both CPU and GE can access via local bus
- Alignment: GE often requires 64-byte aligned addresses
- Usage: Primarily for final framebuffers (RGB565 format)
- Alternative: Use main RAM for framebuffers if VRAM constrained, copy to VRAM via GE

## Inter-Module Communication Mechanisms

### Primary Mechanisms

1. **Message Queues** (Preferred for streaming data):
   - Implement using PSP's semaphore and mutex primitives
   - Fixed-size buffer pools for efficiency
   - Used for:
     - H.264 NAL units (network → video)
     - Opus packets (network → audio)
     - Decoded frames (video → display)
     - PCM samples (audio → output)
     - Control events (input ↔ network)

2. **Callbacks** (For event signaling):
   - Function pointers for asynchronous notifications
   - Used for:
     - Connection status changes
     - Error conditions
     - Frame ready notifications

3. **Shared Memory** (Limited use due to fragmentation concerns):
   - Only for large, long-lived buffers
   - Protected by mutexes when accessed by multiple threads
   - Example: Shared frame buffer pool

### Queue Implementation Example

```c
typedef struct {
    void* buffer[MAX_QUEUE_SIZE];
    u32 head;
    u32 tail;
    u32 count;
    u32 max_size;
    SceUID mutex;
    SceUID semaphore_not_empty;
    SceUID semaphore_not_full;
} FrameQueue;

// Initialize with pre-allocated buffer pool
void frame_queue_init(FrameQueue* q, void* buffers[], u32 count) {
    q->head = 0;
    q->tail = 0;
    q->count = 0;
    q->max_size = count;
    q->mutex = sceKernelCreateMutex("frame_queue_mutex", 0, NULL);
    q->semaphore_not_empty = sceKernelCreateSemaphore("frame_queue_not_empty", 0, 0, NULL);
    q->semaphore_not_full = sceKernelCreateSemaphore("frame_queue_not_full", 0, count, NULL);
    
    // Store buffer pointers
    for (u32 i = 0; i < count; i++) {
        q->buffer[i] = buffers[i];
    }
}

// Push frame (blocking if full)
void frame_queue_push(FrameQueue* q, void* frame) {
    sceKernelWaitSemaphore(q->semaphore_not_full, 1, NULL);
    sceKernelWaitMutex(q->mutex, NULL);
    
    q->buffer[q->tail] = frame;
    q->tail = (q->tail + 1) % q->max_size;
    q->count++;
    
    sceKernelSignalMutex(q->mutex);
    sceKernelSignalSemaphore(q->semaphore_not_empty, 1);
}

// Pop frame (blocking if empty)
void* frame_queue_pop(FrameQueue* q) {
    sceKernelWaitSemaphore(q->semaphore_not_empty, 1, NULL);
    sceKernelWaitMutex(q->mutex, NULL);
    
    void* frame = q->buffer[q->head];
    q->head = (q->head + 1) % q->max_size;
    q->count--;
    
    sceKernelSignalMutex(q->mutex);
    sceKernelSignalSemaphore(q->semaphore_not_full, 1);
    
    return frame;
}
```

## Error Handling & Recovery Strategies

### Network Layer
- **Connection Loss**: Automatic reconnection with exponential backoff
- **Packet Loss**: enet handles retransmission; application-level timeout for stalled streams
- **Authentication Failure**: Clear error message, return to pairing screen
- **Bandwidth Exceeded**: Automatic quality reduction if protocol supports it

### Video Pipeline
- **Decode Errors**: 
  - Skip corrupted frames
  - Request keyframe from host if persistent
  - Fallback to software decode if ME fails repeatedly
- **Display Errors**: 
  - Reset GE/VRAM state
  - Reinitialize display subsystem
  - Fallback to software rendering if GE unavailable

### Audio Pipeline
- **Decode Errors**: 
  - Insert silence for lost frames
  - Reset decoder state on persistent errors
- **Output Errors**: 
  - Restart audio channels
  - Buffer underrun/overrun protection

### Input Module
- **Controller Disconnection**: 
  - Use last known state
  - Periodic reconnection attempts
- **Input Lag**: 
  - Adjust sampling rate based on network conditions
  - Predictive input for high-latency scenarios

### General Recovery
- **Watchdog Timer**: Periodic health checks for each module
- **Graceful Degradation**: 
  - Disable video if audio/network working (audio-only mode)
  - Disable audio if video/network working (muted video)
  - Text-only diagnostic mode if core fails
- **State Persistence**: 
  - Save pairing info and settings to memory stick
  - Restore session after temporary failures

## Power Management Considerations

### CPU Power Scaling
- Utilize PSP's variable clock speed (33-333MHz)
- Dynamically adjust based on workload:
  - Idle/waiting: 33MHz (minimum)
  - Active decoding: 333MHz (maximum)
  - Network wait: Reduced frequency during enet waits
- Use `scePowerSetClockFrequency` for dynamic adjustment

### Peripheral Power Management
- **WiFi**: 
  - Power cycle during long idle periods
  - Use PSP's wifi power saving modes
- **Audio**: 
  - Power down audio codec when silent for extended periods
  - Maintain minimal bias current to prevent pop/noise
- **Display**: 
  - Reduce brightness during buffering periods
  - Utilize display power saving features
- **Media Engine**: 
  - Power down ME when not actively decoding
  - Rapid power-on/power-off capabilities

### Battery Life Optimization
- Target: >3 hours continuous gameplay
- Strategies:
  - Aggressive idle power saving
  - Efficient ME usage (hardware decode << software decode)
  - Minimize VRAM/GE activity during static scenes
  - Optimize audio buffer sizes to reduce DMA transfers

## Threading Model

### Thread Configuration
PSP is single-core but supports preemptive multithreading via `sceKernel`. Recommended thread setup:

| Thread | Priority | Stack Size | Affinity | Purpose |
|--------|----------|------------|----------|---------|
| Main Application | Lowest | 64KB | CPU0 | UI, menu, thread supervision |
| Network | High | 64KB | CPU0 | enet/mbedtls operations, keepalive |
| Video Decode | Highest | 64KB | CPU0 | ME interface, frame decoding |
| Audio Decode | High | 64KB | CPU0 | Opus decode, audio output prep |
| Input Polling | Medium | 32KB | CPU0 | sceCtrl polling, event generation |
| Display Sync | High | 32KB | CPU0 | VBlank wait, frame buffer swap |

### Synchronization Primitives
- **Mutexes**: `sceKernelCreateMutex` for shared resource protection
- **Semaphores**: `sceKernelCreateSemaphore` for producer/consumer signaling
- **Event Flags**: `sceKernelCreateEventFlag` for complex multi-condition waiting
- **Condition Variables**: Built on mutex+semaphore combinations

### Priority Inversion Mitigation
- Use priority inheritance mutexes where available
- Keep critical sections extremely short
- Avoid nested locking when possible
- Priority ceiling protocol for shared resources

## Implementation Recommendations

### Phased Approach

**Phase 1: Foundation**
1. Set up PSP toolchain (psptoolchain + pspsdk)
2. Port and test enet with sceNet wrappers
3. Port and test mbedtls with optimized configuration
4. Create basic "hello world" with framebuffer access
5. Study libgamestream structure from moonlight-common-c

**Phase 2: Core Networking**
1. Implement basic GameStream client using moonlight-common-c
2. Establish TLS connection to host
3. Achieve successful session setup (without video/audio)
4. Implement input reporting to host

**Phase 3: Video Pipeline**
1. Implement software H.264 decode baseline (FFmpeg/libavcodec)
2. Display decoded frames to VRAM
3. Achieve low-latency video streaming (even if choppy)
4. Begin ME interface investigation

**Phase 4: Audio & Optimization**
1. Implement Opus decode and audio output
2. Synchronize audio-video playback
3. Optimize memory usage and performance
4. Integrate ME hardware acceleration if feasible

**Phase 5: Polish & Testing**
1. Battery life optimization
2. Input latency reduction
3. Compatibility testing with various games
4. Documentation and cleanup

### Key Implementation Tips

1. **Start Simple**: Begin with software decode path to establish baseline
2. **Profile Early**: Use psp-gprof or custom timing to identify bottlenecks
3. **Memory First**: Implement memory tracking from day one
4. **Modular Design**: Keep modules loosely coupled with well-defined interfaces
5. **Error Resilience**: Assume failures will happen and plan for recovery
6. **Leverage Existing Work**: 
   - Use psp-media-engine-custom-core for ME access
   - Reference moonlight-N3DS for overall structure
   - Use existing opus_psp_research.md for audio implementation
7. **Test Incrementally**: Validate each module before integrating
8. **Optimize for PSP**: 
   - Use fixed-point math where possible
   - Minimize syscalls in hot paths
   - Optimize for MIPS instruction scheduling
   - Utilize VFPU for vector operations when beneficial

## Dependencies & Build System

### External Dependencies
- **libgamestream**: From moonlight-common-c (adapted for PSP)
- **enet**: Reliable UDP library (enet-master/)
- **mbedtls**: TLS 1.2 library (mbedtls-4.0.0/)
- **opus**: Fixed-point Opus codec (opus-1.5.2/)
- **psp-media-engine-custom-core**: For ME H.264 access (optional but preferred)
- **pspsdk**: Core PSP homebrew libraries

### Build System
- Adapt moonlight-N3DS CMakeLists.txt for PSP makefile-based system
- Use pspsdk's build system with proper MIPS toolchain
- Output: EBOOT.PBP for PSP execution
- Configuration options:
  - `USE_ME_DECODE=1` (enable hardware acceleration)
  - `OPTIMIZE_FOR_SIZE=1` (-Os flag)
  - `ENABLE_DEBUG_LOGGING=0` (disable in release)

### Source Structure
```
src/
├── network/          # enet + mbedtls wrapper
├── video/            # ME interface + software fallback
├── audio/            # Opus decode + sceAudio output
├── input/            # sceCtrl polling + mapping
├── display/          # VRAM management + VBlank sync
├── threading/        # Thread creation + synchronization
├── memory/           # Memory pools + allocation tracking
└── main/             # Application entry point + supervision
```

## Conclusion

This architecture provides a feasible path to implementing a PSP Moonlight client that leverages the PSP's unique hardware capabilities while respecting its severe constraints. By strategically offloading H.264 decoding to the Media Engine, carefully managing memory within the 24MB user space limit, and using efficient inter-module communication mechanisms, the client can deliver a playable GameStream experience.

The design balances ideal hardware utilization with practical fallback paths, ensuring functionality even if certain optimizations prove challenging to implement. The phased implementation approach allows for early validation and incremental improvement toward the final goal.

Key success factors include:
1. Effective ME utilization for video decode (primary path)
2. Careful memory budgeting and pooling strategies
3. Efficient use of PSP's networking and audio subsystems
4. Proper threading and synchronization for responsive interaction
5. Robust error handling and power management for usability

With this architecture, a PSP Moonlight client is achievable within the constraints of the original PSP-1000 hardware while providing a foundation for enhancement on later PSP models with more memory.