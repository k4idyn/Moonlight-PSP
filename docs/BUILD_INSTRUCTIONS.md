# PSP Moonlight Build Instructions

This document provides detailed build instructions for compiling the PSP Moonlight client, including setting up the PSP toolchain, cross-compiling dependencies, configuring the build system, and creating a signed EBOOT.PBP file.

## Table of Contents
1. [PSP Toolchain Setup](#1-psp-toolchain-setup)
2. [Cross-compiling Dependencies](#2-cross-compiling-dependencies)
3. [Build System Configuration](#3-build-system-configuration)
4. [Linking Against PSPSDK Libraries](#4-linking-against-pspsdk-libraries)
5. [Creating a Signed EBOOT.PBP](#5-creating-a-signed-ebootpbp)
6. [SDK Adaptations](#6-sdk-adaptations)
7. [Licensing and Interoperability](#7-licensing-and-interoperability)

---

## RECOMMENDED: Automated Setup (Linux / Jules VM)

For a simplified development experience, it is highly recommended to use the automated setup script. This script handles the installation of the PSPSDK, all necessary system dependencies, and cross-compiles ENet, mbedTLS, Opus, and Mini-XML specifically for the PSP architecture.

```bash
cd moonlight-psp-core
./jules_setup.sh
make -j$(nproc)
```

This ensures your environment matches the verified build configuration used by the core team.

---

## 1. PSP Toolchain Setup

### 1.1 Installing psptoolchain
The psptoolchain is an automated builder for the PSP development toolchain, including GCC, binutils, and GDB for MIPS architecture.

```bash
# Clone the psptoolchain repository
git clone https://github.com/pspdev/psptoolchain.git
cd psptoolchain

# Install dependencies (Ubuntu/Debian)
sudo apt-get install autoconf automake bison flex gettext texinfo \
                    libgmp3-dev libmpfr-dev libncurses5-dev libz-dev \
                    libusb-1.0-0-dev libpng-dev

# Run the automated toolchain builder
./toolchain.sh

# Add toolchain to PATH (add to ~/.bashrc or ~/.zshrc)
export PSPDEV=$HOME/pspdev
export PATH=$PATH:$PSPDEV/bin
```

### 1.2 Installing pspsdk
The PSPSDK provides the core libraries for PSP homebrew development.

```bash
# Clone the pspsdk repository
git clone https://github.com/pspdev/pspsdk.git
cd pspsdk

# Build and install
make
make install
```

### 1.3 Verifying Installation
Verify the toolchain is properly installed:

```bash
# Check PSP-GCC availability
psp-gcc --version

# Check PSPSDK installation
psp-config --pspsdk-prefix
```

---

## 2. Cross-compiling Dependencies

### 2.1 ENet (Reliable UDP Library)
ENet is used for reliable UDP transport in the GameStream protocol.

```bash
# Clone ENet repository
git clone https://github.com/LSaints/ENet.git enet-master
cd enet-master

# Create a standalone build directory
mkdir build-psp && cd build-psp

# Configure for PSP cross-compilation
PSP_CONFIGURE_FLAGS="--host=mipsel-psp-elf --prefix=$PSPDEV/mipsel-psp-elf"
../configure $PSP_CONFIGURE_FLAGS

# Build and install
make
make install
```

### 2.2 Opus Audio Codec
Opus is used for audio decoding in the GameStream protocol.

```bash
# Clone Opus repository
git clone https://github.com/xiph/opus.git opus-1.5.2
cd opus-1.5.2

# Create build directory
mkdir build-psp && cd build-psp

# Configure for PSP cross-compilation
PSP_CONFIGURE_FLAGS="--host=mipsel-psp-elf --prefix=$PSPDEV/mipsel-psp-elf --disable-intrinsics"
../configure $PSP_CONFIGURE_FLAGS

# Build and install
make
make install
```

### 2.3 mbedTLS (TLS 1.2 Library)
mbedTLS is used for TLS 1.2 encryption in the GameStream protocol.

```bash
# Copy mbedtls from workspace
cp -r ../mbedtls-4.0.0 .  # Assuming you're in the psp_moonlight directory
cd mbedtls-4.0.0

# Create build directory
mkdir build-psp && cd build-psp

# Configure for PSP with memory optimizations
cmake .. \
    -DCMAKE_TOOLCHAIN_FILE=$PSPDEV/mipsel-psp-elf/share/cmake/psp.cmake \
    -DENABLE_TESTING=OFF \
    -DENABLE_PROGRAMS=OFF \
    -DUSE_SHARED_MBEDTLS_LIBRARY=OFF \
    -DCMAKE_BUILD_TYPE=Release \
    -DMBEDTLS_SSL_MAX_CONTENT_LEN=4096

# Build and install
make
make install
```

### 2.4 psp-media-engine-custom-core (Optional - For Hardware Acceleration)
This library provides access to the PSP Media Engine's H.264 decoding capabilities.

```bash
# Clone the repository
git clone https://github.com/mcidclan/psp-media-engine-custom-core.git
cd psp-media-engine-custom-core

# Build and install
make
make install
```

---

## 3. Build System Configuration

### 3.1 Adapting moonlight-N3DS CMakeLists.txt for PSP
The PSP build system uses a makefile-based approach rather than CMake. We need to adapt the moonlight-N3DS CMakeLists.txt to work with the PSPSDK.

Create a `Makefile.psp` in the project root:

```makefile
# PSP Moonlight Makefile
# Based on PSPSDK makefile conventions

PSPSDK := $(shell psp-config --pspsdk-prefix)
PSPDEV := $(shell psp-config --pspdev-prefix)

# Compiler and tools
CC := psp-gcc
CXX := psp-g++
AR := psp-ar
LD := psp-ld
OBJCOPY := psp-objcopy

# Flags
CFLAGS := -G0 -Wall -O2 -march=4 -mtune=4 -mabi=abi0
CXXFLAGS := $(CFLAGS) -fno-exceptions -fno-rtti
ASFLAGS := $(CFLAGS)

# Libraries
LIBS := -lpspgu -lpspgum -lpspdisplay -lpspkernel -lpspsdk -lm -lc
LIBS += -lenet -lopus -lmbedtls -lmbedx509 -lmbedcrypto
LIBS += -lpspmediaengine  # If using hardware acceleration

# Source files
SRCDIRS := src src/modules
SRCS := $(foreach dir,$(SRCDIRS),$(wildcard $(dir)/*.c))
OBJS := $(SRCS:.c=.o)

# Target
TARGET := PSP_Moonlight

# Build rules
all: $(TARGET).prx

$(TARGET).prx: $(OBJS)
	$(LD) -o $@ $^ $(LIBS) -T$(PSPSDK)/lib/linkfile.x
	$(OBJCOPY) -O binary $@ $@.bin
	make-fself $@.bin $@

%.o: %.c
	$(CC) $(CFLAGS) -I. -Isrc -Isrc/modules -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET).prx $(TARGET).prx.bin

# EBOOT.PBP creation
EBOT.PBP: $(TARGET).prx
	make-fself $< $<.elf
	pack-pbp $@ \
		-TITLE="PSP Moonlight" \
		-ICON0=ICON0.PNG \
		-PIC0=PIC0.PNG \
		-SND0=SND0.AT3 \
		-BOOT=$<.elf
```

### 3.2 Creating a PSP-specific Toolchain File (for CMake alternative)
If preferring to use CMake, create a toolchain file:

```cmake
# psp-toolchain.cmake
SET(CMAKE_SYSTEM_NAME PSP)
SET(CMAKE_SYSTEM_PROCESSOR mips)

SET(CMAKE_C_COMPILER psp-gcc)
SET(CMAKE_CXX_COMPILER psp-g++)

SET(CMAKE_FIND_ROOT_PATH $ENV{PSPDEV}/mipsel-psp-elf)
SET(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
SET(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
SET(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

SET(CMAKE_C_FLAGS "-G0 -Wall -O2 -march=4 -mtune=4 -mabi=abi0" CACHE STRING "" FORCE)
SET(CMAKE_CXX_FLAGS "${CMAKE_C_FLAGS}" CACHE STRING "" FORCE)

SET(CMAKE_EXECUTABLE_SUFFIX .prx)
```

Then configure with:
```bash
cmake -DCMAKE_TOOLCHAIN_FILE=psp-toolchain.cmake ..
```

---

## 4. Linking Against PSPSDK Libraries

### 4.1 Core PSPSDK Libraries
Link against these essential PSPSDK libraries:

```makefile
LIBS := -lpspgu -lpspgum -lpspdisplay -lpspkernel -lpspsdk -lm -lc
```

### 4.2 Networking Libraries
```makefile
LIBS += -lpspnet -lpspnet_inet -lpspnet_apctl -lpspnet_resolver
LIBS += -lenet  # Our cross-compiled ENet
```

### 4.3 Audio Libraries
```makefile
LIBS += -lpspaudio -lpspaudiolib -lpspvfpu
LIBS += -lopus  # Our cross-compiled Opus
```

### 4.4 Cryptography Libraries
```makefile
LIBS += -lmbedtls -lmbedx509 -lmbedcrypto  # Our cross-compiled mbedTLS
```

### 4.5 Media Engine Library (Optional)
```makefile
LIBS += -lpspmediaengine  # If using hardware acceleration
```

### 4.6 Example Linking in Source Code
In your modules, you'll use functions like:

```c
// Networking (sceNet)
sceNetSocket(AF_INET, SOCK_DGRAM, 0);
sceNetSendto(socket, data, size, 0, (struct sockaddr *)&addr, sizeof(addr));

// Audio (sceAudio)
sceAudioOutputBlocking(channel, volume_left, volume_right, pcm_buf);

// Input (sceCtrl)
sceCtrlReadBufferPositive(&pad, 1);

// Display (sceDisplay)
void* vram = sceDisplayGetFrameBuf(0, &pixel_format);
sceDisplayWaitVblankStart();
sceDisplaySetFrameBuf(vram, width*4, PSP_DISPLAY_PIXEL_FORMAT_8888, 1);

// GE (Graphics Engine)
sceGuStart(GU_DIRECT, list);
sceGuDrawBuffer(GU_PSM_8888, fbptr0, BUF_WIDTH);
sceGuDispBuffer(SCREEN_WIDTH, SCREEN_HEIGHT, fbptr1, BUF_WIDTH);
sceGuFinish();
sceGuSync(0,0);
```

---

## 5. Creating a Signed EBOOT.PBP

### 5.1 Using PSPSDK Tools
The PSPSDK provides tools to create signed EBOOT.PBP files:

```bash
# Compile to PRX first (as shown in Makefile)
make -f Makefile.psp

# Create a signed EBOOT.PBP
make-fself PSP_Moonlight.prx PSP_Moonlight.elf
pack-pbp EBOOT.PBP \
    -TITLE="PSP Moonlight" \
    -ICON0=ICON0.PNG \
    -PIC0=PIC0.PNG \
    -SND0=SND0.AT3 \
    -BOOT=PSP_Moonlight.elf
```

### 5.2 Required Files for EBOOT.PBP
- `BOOT`: The main executable (ELF format)
- `ICON0`: 144x80 PNG icon (optional but recommended)
- `PIC0`: 480x272 PNG background image (optional)
- `SND0`: AT3 audio file for background music (optional)

### 5.3 Alternative: Using CMAKE_EBOOT_PSP
If using CMake with the PSP toolchain, you can enable EBOOT creation:

```cmake
# In your CMakeLists.txt
SET(CMAKE_EXECUTABLE_SUFFIX .prx)
ADD_EXECUTABLE(PSP_Moonlight ${SRCS})
TARGET_LINK_LIBRARIES(PSP_Moonlight ${LIBS})

# Create EBOOT.PBP
ADD_CUSTOM_COMMAND(TARGET PSP_Moonlight POST_BUILD
    COMMAND make-fself $<TARGET_FILE:PSP_Moonlight> $<TARGET_FILE:PSP_Moonlight>.elf
    COMMAND pack-pbp EBOOT.PBP
        -TITLE="PSP Moonlight"
        -ICON0=${CMAKE_SOURCE_DIR}/ICON0.PNG
        -PIC0=${CMAKE_SOURCE_DIR}/PIC0.PNG
        -SND0=${CMAKE_SOURCE_DIR}/SND0.AT3
        -BOOT=$<TARGET_FILE:PSP_Moonlight>.elf
)
```

---

## 6. SDK Adaptations

### 6.1 Homebrew Mode (User/Kernel Space)
PSP homebrew runs in user mode by default, with memory restrictions:

```c
// Check available memory
int free_mem = sceKernelTotalFreeMemSize();  // Returns ~24MB on PSP-1000
int max_free_block = sceKernelMaxFreeMemSize();  // Check for fragmentation

// Allocate memory from user partition
void* ptr = malloc(size);  // Standard C library (recommended)
// Or for large blocks:
SceUID block_id = sceKernelAllocMemBlock("MyBlock", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, size, NULL);
void* ptr = sceKernelGetMemBlockBase(block_id);
```

### 6.2 Memory Allocation Strategies
Following the memory management strategy from `psp_memory_management_strategy.md`:

```c
// Pre-allocate major buffers at startup
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

// Memory pool for ENet packets
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

### 6.3 VBlank Synchronization Implementation
Proper synchronization prevents tearing:

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

// Alternative: Check VBlank status without blocking
int vblank_count = sceDisplayGetVcount();
if (vblank_count >= SCREEN_HEIGHT) {
    // In VBlank period, safe to swap
}
```

### 6.4 Memory Allocation for 32MB Limit
Following recommendations from `psp_memory_management_strategy.md`:

```c
// Recommended memory budget allocation (from architecture document):
// - Code & Static Data: 4-6MB
// - Heap/Dynamic Allocation: 8-10MB
// - Video Framebuffers: 4-6MB
// - Audio Buffers: 1-2MB
// - Networking Buffers: 1-2MB
// - OS/PSP SDK Overhead: 2-4MB
// - Stack Space: 1-2MB
// - VRAM Usage: 512KB-1MB

// Example: Pre-allocating video buffers (YUV420 format)
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

// Monitor memory usage
void print_memory_stats() {
    int free_total = sceKernelTotalFreeMemSize();
    int free_max = sceKernelMaxFreeMemSize();
    debugPrintf("Memory: %d KB free total, %d KB free max\n",
                free_total / 1024, free_max / 1024);
}
```

---

## 7. Licensing and Interoperability

### 7.1 Open-source Licensing Compliance
Verify licenses of all dependencies:

| Dependency | License | Compatibility |
|------------|---------|---------------|
| Opus | BSD-like | Compatible with GPL, commercial use |
| ENet | MIT | Permissive, compatible with all licenses |
| mbedTLS | Apache-2.0 | Permissive, compatible with GPLv3 |
| PSPSDK | GPL | Requires GPL-compatible licensing for linked works |
| psp-media-engine-custom-core | Custom (check repository) | Verify compatibility |
| moonlight-common-c | MPL 2.0 | Weak copyleft, compatible with many licenses |

**Note**: Since PSPSDK is GPL, your final executable must be GPL-compatible if you link directly against it. Consider using dynamic loading or interface abstractions if GPL compatibility is a concern.

### 7.2 Ensuring Interoperability with Existing Moonlight Servers
To maintain compatibility with existing Moonlight servers:

1. **Protocol Version**: Implement GameStream protocol version matching moonlight-common-c
2. **Encryption**: Use mbedTLS for TLS 1.2 with same cipher suites
3. **Transport**: Use ENet for reliable UDP with same channel configuration:
   - Control channel: Port 47999
   - Video stream: Port 48000
   - Audio stream: Port 48001
   - Events: Port 48002
4. **Message Formats**: Maintain same message structures for:
   - Session setup (RTSP-like over TLS)
   - Control events (button presses, analog input)
   - Video metadata (resolution, fps, profile)
   - Audio metadata (sample rate, channels, frame size)
5. **Codec Requirements**:
   - Video: H.264 Baseline/Main/High profile (up to Level 2.1 for PSP)
   - Audio: Opus (supporting standard frame sizes: 2.5, 5, 10, 20, 40, 60 ms)

### 7.3 Verification Checklist
Before building, verify:

1. [ ] All dependencies cross-compiled successfully for MIPS PSP
2. [ ] Build system correctly links against PSPSDK libraries
3. [ ] EBOOT.PBP creation produces a valid, signed executable
4. [ ] Memory usage stays within 24MB user space limit
5. [ ] VBlank synchronization prevents tearing
6. [ ] Input mapping correctly translates PSP controls to GameStream format
7. [ ] Audio output produces correct PCM format (16-bit stereo, 48kHz)
8. [ ] Video output uses correct pixel format (RGB565 or ARGB8888)
9. [ ] Networking properly establishes TLS connection to host
10. [ ] Error handling and recovery strategies are implemented

---

## Troubleshooting

### Common Issues and Solutions

**Issue**: "undefined reference to `sceKernel*' functions"
**Solution**: Ensure `-lpspkernel` is in your LIBS and that PSPSDK is properly installed.

**Issue**: Failed to allocate VRAM buffers
**Solution**: 
- Check VRAM availability with `sceGeEdramGetAddr()`
- Try smaller buffer sizes or different pixel formats
- Consider using main RAM buffers and copying to VRAM via GE

**Issue**: EBOOT.PBP fails to run on PSP
**Solution**:
- Verify the ELF file is properly stripped and optimized
- Check that all required libraries are linked
- Ensure the BOOT section in EBOOT.PBP points to valid executable
- Try running with PSPLink to debug

**Issue**: Audio crackling or dropouts
**Solution**:
- Increase audio buffer sizes
- Use `sceAudioOutputBlocking` instead of streaming mode
- Check sample rate conversion (PSP native is 48kHz)
- Ensure audio thread has sufficient priority

**Issue**: Video tearing or artifacts
**Solution**:
- Verify VBlank synchronization is working
- Check double-buffer implementation
- Ensure proper pixel format conversion (YUV420 to RGB565)
- Verify VRAM alignment requirements (often 64-byte aligned)

## References

1. PSPSDK Documentation: https://pspdev.github.io/pspsdk/
2. PSP Developer Wiki: https://www.psdevwiki.com/psp/
3. moonlight-common-c: https://github.com/moonlight-stream/moonlight-common-c
4. psp-media-engine-custom-core: https://github.com/mcidclan/psp-media-engine-custom-core
5. ENet Library: https://github.com/LSaints/ENet
6. Opus Codec: https://opus-codec.org/
7. mbedTLS: https://tls.mbed.org/