# PSP Memory Management Strategy for 32MB Limit

## Executive Summary
This document outlines a comprehensive memory management strategy for PSP homebrew development targeting the original PSP-1000 model with 32MB RAM limit. The strategy covers memory layout, allocation functions, fragmentation mitigation, buffer sizing for video/audio/network, thread stack considerations, and practical techniques to stay within limits with safety margins.

## 1. PSP Memory Layout

### User vs Kernel Space
- **Total RAM**: 32MB on PSP-1000, 64MB on PSP-2000+
- **Kernel Space**: Approximately 8MB reserved for kernel (0x80000000 - 0xFFFFFFFF)
- **User Space**: Approximately 24MB available for homebrew applications (0x00000000 - 0x7FFFFFFF)
- **Verified by**: `sceKernelTotalFreeMemSize()` returns ~24MB on standard firmware
- **Note**: With PSP_LARGE_MEMORY=1 in makefile, can access up to 54MB on later models

### Memory Protection
- Hardware memory protection via MIPS MMU
- User mode: MSB (bit 31) = 0 (0x00000000 - 0x7fffffff)
- Kernel mode: MSB (bit 31) = 1 (0x80000000 - 0xffffffff)

## 2. PSP SDK Memory Allocation Functions

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

## 3. Memory Fragmentation Concerns and Mitigation

### Fragmentation Issues
- External fragmentation: Free memory scattered in small blocks
- Internal fragmentation: Allocated memory larger than requested
- Observed via `sceKernelMaxFreeMemSize()` showing smaller blocks than total free

### Mitigation Strategies
1. **Memory Pools**: Pre-allocate fixed-size buffers for common object types
2. **Object Reuse**: Recycle objects instead of freeing/reallocating
3. **Fixed-Size Allocation**: Use power-of-two block sizes to reduce fragmentation
4. **Allocation Ordering**: Allocate large, long-lived blocks first
5. **Memory Defragmentation**: Periodic compaction (if applicable to use case)
6. **Use libpspvram**: Recommended over libpspvalloc for VRAM management (less fragmentation)

### Evidence of Fragmentation
- Forum reports show `sceKernelTotalFreeMemSize()` ~1MB but `sceKernelMaxFreeMemSize()` ~966KB
- Indicates significant fragmentation preventing large contiguous allocations

## 4. Video Buffer Allocation

### Frame Buffer Specifications
- Native resolution: 480x272 pixels
- Actual framebuffer width: 512 pixels (due to hardware constraints)
- Visible area: 480x272 within 512x272 buffer

### Memory Calculations
#### Pixel Formats:
- **RGBA 8888**: 4 bytes/pixel
  - 512x272x4 = 557,056 bytes (~544 KB)
- **RGBA 4444**: 2 bytes/pixel  
  - 512x272x2 = 278,528 bytes (~272 KB)
- **RGB 565**: 2 bytes/pixel
  - 512x272x2 = 278,528 bytes (~272 KB)
- **Indexed 8-bit**: 1 byte/pixel + palette
  - 512x272x1 = 139,264 bytes (~136 KB) + 256x4/2 bytes palette

### Multiple Buffers
- Double buffering: 2x frame buffer size
- Triple buffering: 3x frame buffer size
- Recommended: Double buffering for smooth animation

### VRAM vs Main RAM
- VRAM: Limited size, faster access for GPU operations
- Main RAM: Larger, slower access but more flexible
- Use `sceGeEdramGetAddr()` to get VRAM address
- Consider libpspvram for dynamic VRAM allocation

## 5. Audio Buffer Sizing

### Opus Decode Buffers
#### Frame Size Requirements
- Opus supports frame sizes: 2.5, 5, 10, 20, 40, 60 ms
- At 48kHz: 120, 240, 480, 960, 1920, 2880 samples per channel
- Frame size in samples must match encoder settings

#### Memory Calculations
- **Decoder State**: ~2-4 KB mono, ~4-8 KB stereo
- **Input Buffer**: Compressed Opus packet (variable, typically < 2KB)
- **Output Buffer**: PCM samples
  - Mono: frame_size × 2 bytes (16-bit)
  - Stereo: frame_size × 2 × 2 bytes (16-bit interleaved)
  - Example: 960 samples stereo = 960 × 4 = 3,840 bytes

#### Recommended Buffer Sizes
- Opus decoder state: 8 KB
- Input packet buffer: 4 KB (handles max Opus packet)
- Output PCM buffer: 16 KB (accommodates multiple frames)
- Double buffering: 32 KB for PCM output
- **Total Opus audio memory**: ~50 KB

### Playback Buffers
- pspaudiolib uses internal buffering
- sceAudioOutputBlocking requires user-provided buffer
- Typical audio buffer: 4-8 KB for low latency
- Consider double buffering for audio output

## 6. Network Buffer Sizing

### ENet Packet Buffers
#### ENet Defaults (from enet.h)
- `ENET_HOST_DEFAULT_MAXIMUM_PACKET_SIZE` = 32 × 1024 × 1024 = 32 MB
- `ENET_HOST_DEFAULT_MAXIMUM_WAITING_DATA` = 32 × 1024 × 1024 = 32 MB
- **Note**: These are theoretical maximums, not recommended for PSP

#### Practical PSP Considerations
- With only 24MB user space, cannot allocate 32MB buffers
- Recommended maximum packet size: 1-4 KB for typical game data
- MTU considerations: Typically 1400 bytes for internet, less for wireless
- Implement application-level packet size limits

#### Buffer Management
- ENet uses reference counting for packets
- Packet reuse reduces allocation/free overhead
- Consider custom allocators for ENet packets

### mbedTLS Buffers
#### Default Memory Usage
- TLS maximum message content length: 16 KB (default)
- Allocates double this in RAM (RX & TX buffer): 32 KB overhead per session
- Handshake allocation: ~2 KB per connection

#### Optimization Strategies
1. Reduce `MBEDTLS_SSL_MAX_CONTENT_LEN`:
   - Minimum: ~4096 bytes (determined by TLS protocols)
   - 4 KB setting frees 24 KB vs 16 KB default
2. Use static memory allocation:
   - `unsigned char memory_buf[100000];`
   - `mbedtls_memory_buffer_alloc_init(memory_buf, sizeof(memory_buf));`
3. Enable `CONFIG_MBEDTLS_DYNAMIC_BUFFER` if available
4. Consider session resumption to reduce handshake frequency

#### Per-Connection Memory Estimate
- Optimized: ~8-12 KB per TLS session
- Default: ~32 KB per TLS session
- Maximum concurrent sessions: Limited by available memory

## 7. Stack Size Considerations for Threads

### Thread Creation
- `sceKernelCreateThread()` requires stack size parameter
- Default in samples: 256 × 1024 = 256 KB
- Minimum viable: Depends on thread function complexity

### Stack Size Guidelines
- **Simple threads** (minimal locals, no recursion): 4-8 KB
- **Moderate threads** (some locals, light recursion): 16-32 KB
- **Complex threads** (deep recursion, large locals): 64 KB+
- **Main thread**: Often larger due to CRT initialization

### Stack Monitoring
- Use `sceKernelGetThreadStackFreeSize()` to monitor usage
- Initialize stack with known pattern to detect overflow
- Guard pages not available on PSP without MMU tricks

### Recommendations
- Start with 64 KB stacks for most threads
- Monitor actual usage and adjust downward
- Consider stack size based on deepest call stack + local variables
- For audio/network threads: 32-64 KB typically sufficient

## 8. Memory Pools vs Dynamic Allocation Trade-offs

### Memory Pools Advantages
- **Predictable performance**: Fixed allocation time
- **Reduced fragmentation**: Pre-allocated fixed-size blocks
- **Deterministic behavior**: No allocation failures after pool exhaustion
- **Cache efficiency**: Better locality for similar objects
- **Lower overhead**: No bookkeeping per allocation (beyond pool management)

### Memory Pools Disadvantages
- **Fixed block sizes**: May waste space if objects vary significantly
- **Pool exhaustion**: Need strategy when pools run out (block, fail, expand)
- **Initial memory cost**: Pools consume memory even when unused
- **Complexity**: More code to manage multiple pools

### Dynamic Allocation Advantages
- **Flexibility**: Allocate exactly what's needed
- **Memory efficient**: Only use memory when actually needed
- **Simplicity**: Standard malloc/familiar interface

### Dynamic Allocation Disadvantages
- **Fragmentation**: External and internal fragmentation over time
- **Unpredictable timing**: Allocation/free time varies
- **Allocation failures**: Can fail even with free memory due to fragmentation
- **Overhead**: Bookkeeping metadata for each allocation

### PSP-Specific Recommendations
1. **Use memory pools for**:
   - Fixed-size objects (game entities, particles, UI elements)
   - Network packets (ENet)
   - Audio buffers (Opus, PCM)
   - Video textures/frames
   - TLS connections (mbedTLS)

2. **Use dynamic allocation for**:
   - Variable-size assets (loaded at runtime)
   - Infrequently allocated large buffers
   - Data with unpredictable size (parsed files, JSON)

3. **Hybrid approach**:
   - Pool for common small allocations
   - Fallback to dynamic for outliers
   - Memory profiling to guide pool sizing

### Available PSP Pool Libraries
- **libpspvram**: Recommended for VRAM management (less fragmentation than libpspvalloc)
- Custom pool implementations straightforward for fixed-size objects

## 9. Memory Measurement and Debugging Tools

### SDK Measurement Functions
- `sceKernelTotalFreeMemSize()`: Total free memory in user space
- `sceKernelMaxFreeMemSize()`: Largest contiguous free block
- `sceKernelGetMemBlockInfo()`: Details about specific memory block
- `sceKernelGetHeapInfo()`: Heap usage information
- `sceKernelGetThreadStackFreeSize()`: Free stack size for thread

### Debugging Tools
- **PSPLINK/PSPLINKUSB**: 
  - Memory inspection and modification
  - Breakpoints, watchpoints, single-stepping
  - Requires building homebrew as unencrypted .prx
- **PSPLink GUI**: Graphical interface for memory/process viewing
- **Kernel Memory Dumper**: Dump kernel memory ranges (0x88000000-0x883fffff, 0xbfc00000-0xbfd00000)
- **MemoryStick-Tool**: Includes memory dumping capabilities
- **psp-gprof**: Profiling tool (requires specific setup)

### Debugging Techniques
1. **Memory leak detection**:
   - Track allocations/frees with custom wrappers
   - Periodic memory snapshots to detect growth
   - Use `sceKernelTotalFreeMemSize()` trend analysis

2. **Corruption detection**:
   - Fill allocated memory with known patterns (0xDEADBEEF)
   - Check patterns before free to detect over/under writes
   - Use hardware watchpoints via PSPLINK if available

3. **Fragmentation analysis**:
   - Monitor ratio of `sceKernelMaxFreeMemSize()` / `sceKernelTotalFreeMemSize()`
   - Low ratio (<0.5) indicates significant fragmentation
   - Consider pool implementation if ratio consistently low

4. **Stack overflow detection**:
   - Initialize stack with known value (0xCDCDCDCD)
   - Periodically check for unchanged values at stack end
   - Increase stack size if corruption detected

## 10. Techniques to Stay Within 32MB Limit with Safety Margin

### Memory Budget Allocation (Recommended)
| Component | Recommended Allocation | Notes |
|-----------|----------------------|-------|
| **Game Assets** | 8-10 MB | Textures, models, levels (compressed) |
| **Video Buffers** | 1-2 MB | Double/triple framebuffers (512x272x4x2-3) |
| **Audio System** | 0.1-0.5 MB | Opus decoder + PCM buffers + pspaudiolib |
| **Network Stack** | 0.5-2 MB | ENet + mbedTLS sessions (optimized) |
| **Thread Stacks** | 1-2 MB | 4-8 threads × 256-512 KB each |
| **Memory Pools** | 2-4 MB | Fixed-size object pools |
| **OS/Libraries** | 2-3 MB | PSP SDK, libc, Opus, etc. |
| **Free/Safety Margin** | 2-4 MB | For fragmentation, unexpected needs |
| **Total** | 24 MB | Leaves room in 24MB user space |

### Specific Optimization Techniques

#### 1. Asset Management
- Compress textures (PNG, JPEG, or custom formats)
- Use audio streaming instead of loading entire tracks
- Implement asset loading/unloading based on usage
- Consider procedural generation for some content

#### 2. Buffer Optimization
- Use smallest viable pixel format (RGBA4444 vs RGBA8888)
- Implement buffer pooling for frequent allocations
- Double buffer only when necessary (some effects may need triple)
- Reuse buffers where possible (ping-pong vs allocating new)

#### 3. Audio Optimization
- Select appropriate Opus frame size for use case
- Use lower bitrates when quality permits (16-32 kbps speech, 64-128 kbps music)
- Consider fixed-point Opus implementation if available
- Share decoder instances when possible

#### 4. Network Optimization
- Implement application-level message size limits
- Use ENet packet reuse features
- Optimize mbedTLS settings:
  - Reduce `MBEDTLS_SSL_MAX_CONTENT_LEN` to 4096
  - Use static memory allocation for TLS buffers
  - Limit concurrent TLS sessions
- Consider UDP without reliability for non-critical data

#### 5. Thread Optimization
- Analyze actual stack usage and right-size stacks
- Use thread pools instead of creating/destroying threads
- Consider coroutines or state machines for some tasks
- Affinitize threads to reduce cache thrashing if SMP available

#### 6. Memory Pool Implementation
- Create pools for:
  - Game objects (entities, bullets, particles)
  - UI elements (buttons, labels, panels)
  - Network packets (ENet)
  - Audio frames (Opus input/output)
  - Texture objects
- Use power-of-two block sizes for efficient splitting/coalescing
- Implement pool statistics to tune sizes

#### 7. Monitoring and Telemetry
- Implement memory tracking wrappers around alloc/free
- Log memory usage at key points (level load, scene change)
- Display memory usage in debug builds
- Set up alerts when memory usage exceeds thresholds

#### 8. Safety Margins
- Never allocate more than 80% of available user space
- Reserve 20% for fragmentation and unexpected allocations
- Monitor `sceKernelMaxFreeMemSize()` to ensure adequate contiguous space
- Implement graceful degradation when memory low

### Example Memory Tracking Implementation
```c
#include <pspkernel.h>
#include <stdio.h>

// Simple memory tracker
typedef struct {
    u32 total_allocated;
    u32 allocation_count;
    u32 max_allocated;
} MemoryTracker;

static MemoryTracker mem_tracker = {0};

void* tracked_malloc(size_t size) {
    void* ptr = malloc(size);
    if (ptr) {
        mem_tracker.total_allocated += size;
        mem_tracker.allocation_count++;
        if (mem_tracker.total_allocated > mem_tracker.max_allocated) {
            mem_tracker.max_allocated = mem_tracker.total_allocated;
        }
        // Optional: fill with pattern for corruption detection
        // memset(ptr, 0xCD, size);
    }
    return ptr;
}

void tracked_free(void* ptr) {
    if (ptr) {
        // In a real implementation, you'd need to track size per allocation
        // This is a simplified example
        free(ptr);
    }
}

void print_memory_stats() {
    u32 free_total = sceKernelTotalFreeMemSize();
    u32 free_max = sceKernelMaxFreeMemSize();
    printf("Memory: %d KB free total, %d KB free max\n", 
           free_total / 1024, free_max / 1024);
    printf("Tracked: %d KB allocated (%d allocs, peak %d KB)\n",
           mem_tracker.total_allocated / 1024,
           mem_tracker.allocation_count,
           mem_tracker.max_allocated / 1024);
}
```

## 11. Specific Recommendations for Opus Audio on PSP

### Memory Footprint
- Opus decoder: 4-8 KB RAM (stereo)
- Frame buffers: Depends on frame size and channels
- PCM output: 2 bytes/sample × channels × frame_size
- **Total typical usage**: < 20 KB for decoder + buffers

### Implementation Guidelines
1. **Decoder Lifetime**: Create once, reuse for multiple streams
2. **Frame Size Selection**: 
   - 20 ms (960 samples) good balance for most applications
   - Lower latency: 10 ms (480 samples) for interactive use
   - Higher efficiency: 40-60 ms for streaming music
3. **Bitrate Selection**:
   - Speech: 16-32 kbps
   - Music: 64-128 kbps
   - Avoid very low (<12 kbps) or very high (>256 kbps) unless necessary
4. **Buffer Management**:
   - Pre-allocate input/output buffers
   - Consider circular buffer for streaming
   - Double buffer PCM output if needed for audio pipeline
5. **Integration**:
   - Decode Opus → PCM buffer → pspaudiolib/sceAudio
   - Handle sample rate conversion if needed (PSP supports 48kHz natively)
   - Use audio callbacks for low-latency output

### Performance Expectations
- CPU usage: 30-70 MHz depending on bitrate and complexity
- Well within PSP 333 MHz capabilities
- Leaves significant headroom for game logic, rendering, networking

## Conclusion

By implementing the strategies outlined in this document, PSP homebrew developers can effectively manage memory within the 32MB constraint while maintaining system stability and performance. Key principles include:

1. **Understand the memory layout**: 24MB user space available on PSP-1000
2. **Use appropriate allocation functions**: Mix of standard C and PSP kernel functions
3. **Combat fragmentation**: Through memory pools, object reuse, and careful allocation patterns
4. **Right-size buffers**: Calculate exact needs for video, audio, and network components
5. **Monitor usage**: Employ SDK measurement tools and custom tracking
6. **Maintain safety margins**: Never exceed 80% utilization to account for fragmentation
7. **Optimize for use case**: Tailor settings (bitrates, frame sizes, pool sizes) to actual requirements

With careful planning and implementation, it's possible to create feature-rich PSP homebrew applications that operate reliably within the original hardware's memory constraints.