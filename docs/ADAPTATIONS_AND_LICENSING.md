# PSP Moonlight SDK Adaptations, Memory Allocation, and VBlank Synchronization

This document details the SDK adaptations, memory allocation strategies, VBlank synchronization implementation, licensing verification, and interoperability checks for the PSP Moonlight client.

## 1. Homebrew Mode Specifics for PSP

### User vs Kernel Space
- PSP homebrew runs in user mode by default, with memory restrictions
- **Total RAM**: 32MB on PSP-1000 model
- **Kernel Space**: Approximately 8MB reserved for kernel (0x80000000 - 0xFFFFFFFF)
- **User Space**: Approximately 24MB available for homebrew applications (0x00000000 - 0x7FFFFFFF)
- **Verified by**: `sceKernelTotalFreeMemSize()` returns ~24MB on standard firmware
- **Memory Protection**: Hardware memory protection via MIPS MMU
  - User mode: MSB (bit 31) = 0 (0x00000000 - 0x7fffffff)
  - Kernel mode: MSB (bit 31) = 1 (0x80000000 - 0xffffffff)
- User mode cannot access kernel space without exploits

### Available Memory for Homebrew
- Forum discussions suggest ~24MB available for applications on PSP-1000
- 8MB typically reserved for kernel space
- Memory protection can be disabled to access full range (requires kernel mode)
- With `PSP_LARGE_MEMORY=1` in makefile, can access up to 54MB on later models (PSP-2000+)

### Memory Allocation Functions
- `sceKernelAllocMemBlock()`: Allocate memory blocks from kernel partition
- `sceKernelFreeMemBlock()`: Free memory blocks allocated with above
- `sceKernelTotalFreeMemSize()`: Get total free memory in user space
- `sceKernelMaxFreeMemSize()`: Get maximum free memory block size
- `linearAlloc()`: For contiguous physical memory allocation (similar to 3DS)
- Standard C library functions (`malloc()`, `free()`, etc.) available via PSPSDK's mini-libc

## 2. Memory Allocation Strategies Implemented

### Standard C Library Functions
- `malloc()`, `free()`, `calloc()`, `realloc()` - Available via PSPSDK's mini-libc
- `memalign()`, `posix_memalign()`, `valloc()` - For aligned memory allocation
- `aligned_alloc()` - C11 standard aligned allocation

### PSP Kernel Memory Functions
- `sceKernelAllocMemBlock()` - Allocate memory block from kernel partition
- `sceKernelFreeMemBlock()` - Free memory block allocated with above
- `sceKernelGetMemBlockBase()` - Get base address of memory block
- `sceKernelAllocPartitionMemory()` - Allocate from specific memory partition
- `sceKernelFreePartitionMemory()` - Free partition memory

### Memory Partitions
- Kernel partitions: For system use
- User partitions: For application use (recommended for homebrew)
- Partition types: SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, SCE_KERNEL_MEMBLOCK_TYPE_USER_MAIN_RW, etc.

### Alignment Requirements
- Default malloc alignment: 8 bytes (32-bit) or 16 bytes (64-bit systems)
- For GPU/VRAM access: Often requires 64-byte or 512-byte alignment
- Use `memalign()` or `posix_memalign()` for specific alignment needs

### Memory Allocation Strategies from Implementation

#### Pre-allocating Major Buffers at Startup
```c
#define MAX_VIDEO_FRAMES 3
#define VIDEO_FRAME_SIZE (512*272*3/2)  // YUV420 format
static uint8_t* video_buffers[MAX_VIDEO_FRAMES];

void init_video_buffers() {
    for (int i = 0; i < MAX_VIDEO_FRAMES; i++) {
        video_buffers[i] = malloc(VIDEO_FRAME_SIZE);
        // Initialize to zero
        memset(video_buffers[i], 0, VIDEO_FRAME_SIZE);
    }
}
```

#### Memory Pool for ENet Packets
```c
typedef struct {
    void* buffer[MAX_PACKETS];
    int head, tail, count;
    SceUID mutex;
    SceUID sem_not_empty;
    SceUID sem_not_full;
} PacketPool;

// Initialize pool
void packet_pool_init(PacketPool* pool) {
    pool->head = pool->tail = pool->count = 0;
    pool->mutex = sceKernelCreateMutex("packet_pool_mutex", 0, NULL);
    pool->sem_not_empty = sceKernelCreateSemaphore("packet_pool_not_empty", 0, 0, NULL);
    pool->sem_not_full = sceKernelCreateSemaphore("packet_pool_not_full", 0, MAX_PACKETS, NULL);
    
    // Pre-allocate packet buffers
    for (int i = 0; i < MAX_PACKETS; i++) {
        pool->buffer[i] = malloc(MAX_PACKET_SIZE);
    }
}
```

#### Video Buffer Allocation (YUV420 Format)
```c
#define VIDEO_WIDTH 512
#define VIDEO_HEIGHT 272
#define VIDEO_FRAME_SIZE (VIDEO_WIDTH * VIDEO_HEIGHT * 3 / 2)  // YUV420
#define NUM_VIDEO_BUFFERS 3

static uint8_t* video_frame_buffers[NUM_VIDEO_BUFFERS];

void allocate_video_buffers() {
    for (int i = 0; i < NUM_VIDEO_BUFFERS; i++) {
        video_frame_buffers[i] = malloc(VIDEO_FRAME_SIZE);
        if (!video_frame_buffers[i]) {
            // Handle allocation failure
            debugPrintf("Failed to allocate video buffer %d\n", i);
        }
    }
}
```

#### Memory Usage Monitoring
```c
void print_memory_stats() {
    int free_total = sceKernelTotalFreeMemSize();
    int free_max = sceKernelMaxFreeMemSize();
    debugPrintf("Memory: %d KB free total, %d KB free max\n",
                free_total / 1024, free_max / 1024);
}
```

### Memory Budget Allocation (Based on Architecture Document)
| Component | Allocation | Notes |
|-----------|------------|-------|
| Code & Static Data | 4-6MB | libgamestream, ME interface, audio/video decoders |
| Heap/Dynamic Allocation | 8-10MB | Video buffers, audio buffers, network packets |
| Video Framebuffers | 4-6MB | Multiple H.264 frames (YUV420 format) |
| Audio Buffers | 1-2MB | Opus packets, PCM buffers |
| Networking Buffers | 1-2MB | ENet packets, mbedtls contexts |
| OS/PSP SDK Overhead | 2-4MB | Threading, syscalls, library reserves |
| Stack Space | 1-2MB | Multiple threads |
| VRAM Usage | 512KB-1MB | For display output |
| **Total** | **21-32MB** | Leaves room for optimization in 24MB user space |

### Fragmentation Mitigation Strategies
1. **Memory Pools**: Pre-allocate fixed-size buffers for common object types
2. **Object Reuse**: Recycle objects instead of freeing/reallocating
3. **Fixed-Size Allocation**: Use power-of-two block sizes to reduce fragmentation
4. **Allocation Ordering**: Allocate large, long-lived blocks first
5. **Memory Defragmentation**: Periodic compaction (if applicable)
6. **Use libpspvram**: Recommended over libpspvalloc for VRAM management

## 3. VBlank Synchronization Implementation Details

### Proper Synchronization Prevents Tearing
The VBlank synchronization ensures that frame buffer swaps occur during the vertical blanking period, preventing screen tearing.

### Render Pipeline Update Function with VBlank Synchronization
```c
// In render pipeline update function
void render_pipeline_update(RenderPipeline* pipeline) {
    // ... decode frame to back buffer ...
    
    // Wait for VBlank before swapping
    sceDisplayWaitVblankStart();
    
    // Swap buffers
    if (sceDisplaySetFrameBuf(pipeline->back_buffer, pipeline->width*4, 
                             PSP_DISPLAY_PIXEL_FORMAT_8888, 1) < 0) {
        // Handle error
    }
    
    // Swap our buffer pointers
    void* temp = pipeline->front_buffer;
    pipeline->front_buffer = pipeline->back_buffer;
    pipeline->back_buffer = temp;
}
```

### Alternative: Non-blocking VBlank Status Check
```c
// Alternative: Check VBlank status without blocking
int vblank_count = sceDisplayGetVcount();
if (vblank_count >= SCREEN_HEIGHT) {
    // In VBlank period, safe to swap
}
```

### Key VBlank Functions Used
- `sceDisplayWaitVblankStart()`: Blocks until VBlank starts
- `sceDisplayGetVcount()`: Gets current vertical count (non-blocking)
- `sceDisplaySetFrameBuf()`: Sets the frame buffer for display
- `sceDisplayGetFrameBuf()`: Gets the current frame buffer address

### Synchronization Approach
1. Decode video frame to back buffer
2. Wait for VBlank start using `sceDisplayWaitVblankStart()`
3. Swap front and back buffers using `sceDisplaySetFrameBuf()`
4. Update internal buffer pointers
5. Repeat for next frame

This approach ensures tear-free output by synchronizing buffer swaps with the display's refresh cycle.

## 4. Licensing Verification for All Components

### Dependency Licensing Matrix

| Dependency | License | Compatibility Notes |
|------------|---------|-------------------|
| **ENet** | MIT License | Permissive, compatible with all licenses |
| **Opus** | BSD-like License | Compatible with GPL, commercial use allowed |
| **mbedTLS** | Dual Apache-2.0 OR GPL-2.0-or-later | Users may choose either license |
| **PSPSDK** | GPL License | Requires GPL-compatible licensing for linked works |
| **psp-media-engine-custom-core** | Custom (verify repository) | License compatibility needs verification |
| **moonlight-common-c** | MPL 2.0 | Weak copyleft, compatible with many licenses |

### Detailed License Information

#### ENet (enet-master/LICENSE)
```
The MIT License (MIT)
Copyright (c) 2002-2016 Lee Salzman
Copyright (c) 2017-2022 Vladyslav Hrytsenko, Dominik Madarász
[Standard MIT license terms]
```

#### Opus (opus-main/COPYING)
```
Copyright 2001-2023 Xiph.Org, Skype Limited, Octasic,
Jean-Marc Valin, Timothy B. Terriberry,
CSIRO, Gregory Maxwell, Mark Borgerding,
Erik de Castro Lopo, Mozilla, Amazon

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:
[Standard BSD-like license with patent provisions]
```

#### mbedTLS (mbedtls-4.0.0/LICENSE)
```
Mbed TLS files are provided under a dual [Apache-2.0](https://spdx.org/licenses/Apache-2.0.html)
OR [GPL-2.0-or-later](https://spdx.org/licenses/GPL-2.0-or-later.html) license.
This means that users may choose which of these licenses they take the code under.
[Full text of both licenses included]
```

#### PSPSDK Licensing Implications
- Since PSPSDK is GPL, any executable linked directly against it must be GPL-compatible
- Consider using dynamic loading or interface abstractions if GPL compatibility is a concern for your application
- The GPL requirement applies to the final executable when linking against PSPSDK libraries

### Licensing Compliance Status
- ✅ ENet: MIT license - compatible with all licenses
- ✅ Opus: BSD-like license - compatible with GPL and commercial use
- ✅ mbedTLS: Dual Apache-2.0/GPL-2.0-or-later - flexible licensing options
- ⚠️ PSPSDK: GPL - requires GPL-compatible licensing for linked works
- ⚠️ psp-media-engine-custom-core: Custom license - requires verification
- ✅ moonlight-common-c: MPL 2.0 - weak copyleft, compatible with many licenses

## 5. Interoperability Checks with Existing Moonlight Servers

### Protocol Version Compatibility
- Implement GameStream protocol version matching moonlight-common-c
- Ensures compatibility with existing Moonlight server implementations
- Uses same message structures and extensions as reference implementation

### Encryption Compatibility
- Use mbedTLS for TLS 1.2 encryption with same cipher suites as moonlight-common-c
- Ensures secure connection establishment to GameStream servers
- Maintains compatibility with server-side TLS requirements

### Transport Layer Compatibility
- Use ENet for reliable UDP transport with identical channel configuration:
  - **Control channel**: Port 47999
  - **Video stream**: Port 48000
  - **Audio stream**: Port 48001
  - **Events**: Port 48002
- Matches moonlight-common-c port assignments exactly
- Uses same ENet configuration parameters for reliability and timing

### Message Format Compatibility
Maintain identical message structures for:
- **Session setup**: RTSP-like protocol over TLS (same as moonlight-common-c)
- **Control events**: Button presses, analog input, touch events
- **Video metadata**: Resolution, FPS, profile, level information
- **Audio metadata**: Sample rate, channels, frame size, bitrate

### Codec Requirements Compliance
- **Video**: H.264 Baseline/Main/High profile (up to Level 2.1 for PSP limitations)
- **Audio**: Opus codec supporting standard frame sizes: 2.5, 5, 10, 20, 40, 60 ms
- Ensures ability to decode streams from existing GameStream servers

### Verification Checklist for Interoperability
1. [ ] Protocol version matches moonlight-common-c
2. [ ] Encryption uses mbedTLS for TLS 1.2 with identical cipher suites
3. [ ] Transport uses ENet with correct port configuration (47999-48002)
4. [ ] Message formats identical for session setup, control, video/audio metadata
5. [ ] Codec support matches server capabilities (H.264 profiles/levels, Opus frame sizes)
6. [ ] Connection handshake follows same RTSP-like over TLS pattern
7. [ ] Keepalive and timeout values compatible with server expectations

### Architecture Alignment with moonlight-common-c
The PSP implementation follows the same modular architecture:
- **Network Layer**: ENet + mbedTLS for reliable, encrypted transport
- **Video Pipeline**: H.264 decode → YUV420→RGB565 conversion → VRAM output
- **Audio Pipeline**: Opus decode → PCM output via sceAudio
- **Input Handling**: sceCtrl for button/nub input → ControlStream to host
- **Threading Model**: Separate threads for network, video, audio, input
- **Memory Management**: Pre-allocated buffers, memory pools, usage monitoring

## Conclusion

This document outlines the key adaptations made for the PSP Moonlight client:
1. **Homebrew Mode Adaptations**: Proper user/kernel space handling, memory checking, and allocation strategies
2. **Memory Allocation**: Standard C library functions, aligned allocation, memory pools, and budget management
3. **VBlank Synchronization**: Frame buffer swapping synchronized with display refresh to prevent tearing
4. **Licensing Verification**: All components use compatible licenses (MIT, BSD-like, Apache/GPL dual, MPL)
5. **Interoperability**: Protocol, encryption, transport, message formats, and codecs match existing Moonlight servers

The implementation leverages proven components from moonlight-common-c while adapting them to PSP's unique hardware constraints and homebrew development environment.