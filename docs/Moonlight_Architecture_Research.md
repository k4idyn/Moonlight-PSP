# Moonlight Architecture Research

## Overall Moonlight Architecture

Moonlight is an open-source implementation of NVIDIA's GameStream protocol, allowing users to stream games from their PC to various devices. The architecture consists of several key components:

### Core Components
1. **Network Receiver**: Handles incoming game stream data using ENET for reliable UDP-based transport
2. **Video Decoder**: Processes H.264 video streams, with hardware acceleration support when available
3. **Audio Decoder**: Handles audio streams from the game session
4. **Input Handling**: Captures and transmits user input back to the host PC
5. **Rendering Pipeline**: Displays decoded video frames on the target device

### Key Architectural Elements
- **libgamestream**: Core library implementing the GameStream protocol shared across all Moonlight clients
- **Platform Abstraction Layer**: Allows Moonlight to run on diverse hardware platforms
- **Hardware Acceleration**: Uses platform-specific APIs (VDPAU, VA-API, OMX, MVD) for video decoding when available
- **Fallback Software Decoding**: Uses FFmpeg/libav when hardware decoding is not available or disabled

## moonlight-N3DS Specific Adaptations

The moonlight-N3DS port adapts Moonlight for the Nintendo 3DS hardware with several key modifications:

### Hardware Decoding (MVD)
- Uses the Nintendo 3DS's Media Video Decoder (MVD) hardware for H.264 decoding on New 3DS models
- Original 3DS models must disable hardware decoding due to insufficient CPU power
- Hardware decoding is enabled via the `hwdecode=1` setting in moonlight.conf
- The MVD API requires specific buffer management and configuration

### Hardware Limitations
- **Memory Constraints**: The 3DS has limited RAM (128MB total, ~96MB available to applications)
- **CPU Limitations**: Original 3DS has a slower ARM11 CPU requiring software decoding fallback
- **Display Resolution**: Top screen is 400x240, bottom screen is 320x240
- **Input Mapping**: Maps 3DS controls (circle pad, D-pad, touchscreen, buttons) to PC game inputs

### Key Adaptations in Source Code
1. **n3ds_video_mvd.cpp**: Implements MVD hardware decoding with proper buffer allocation
2. **Platform-specific initialization**: Uses 3DS SDK functions for graphics, audio, and input
3. **Memory management**: Careful allocation using linearAlloc for video buffers
4. **Renderer abstraction**: Multiple renderers for different display configurations (top, bottom, dual-screen modes)

## libgamestream Component

libgamestream is the shared core implementation of the NVIDIA GameStream protocol used across all Moonlight clients:

### Responsibilities
- **Protocol Handling**: Implements the GameStream protocol suite (control, video, audio streams)
- **Connection Management**: Establishes and maintains connections to the host PC
- **Message Parsing**: Handles GameStream-specific messages and commands
- **Cryptography**: Manages encryption and decryption of stream data
- **Platform Abstraction**: Provides platform-independent interfaces for hardware-specific implementations

### Key Files in moonlight-N3DS
- `libgamestream/client.c`: Main client implementation
- `libgamestream/discover.c`: Service discovery for finding host PCs
- `libgamestream/http.c`: HTTP handling for initial connection
- `libgamestream/sps.c`: Stream parameter set handling for video
- `libgamestream/xml.c`: XML parsing for protocol messages

### Dependencies
- **moonlight-common-c**: Shared codebase between Moonlight implementations
- **ENET**: Reliable UDP networking library
- **mbedTLS**: Cryptographic library for secure connections
- **h264bitstream**: H.264 bitstream parsing library
- **libuuid**: UUID generation for protocol identification
- **reedsolomon**: Error correction for unreliable network conditions

## Networking Implementation

Moonlight uses a combination of libraries for networking:

### ENET
- **Purpose**: Provides reliable UDP-based transport for game streaming
- **Usage**: Main transport protocol for video, audio, and control data
- **Features**: Reliable packet delivery, congestion control, bandwidth management
- **Integration**: Found in `third_party/moonlight-common-c/enet`

### mbedTLS
- **Purpose**: Provides cryptographic security for the connection
- **Usage**: Encrypts and decrypts all stream data
- **Features**: TLS/DTLS support, X.509 certificate handling
- **Integration**: Used throughout the codebase with `-DUSE_MBEDTLS` flag
- **Dependencies**: `-lmbedtls -lmbedx509 -lmbedcrypto`

### Connection Flow
1. **Discovery**: Uses libgamestream's discover module to find available PCs
2. **Initial Connection**: Establishes HTTP connection for initial handshake
3. **Pairing**: Securely pairs with host using PIN authentication
4. **Stream Setup**: Negotiates video/audio parameters and encryption
5. **Data Transfer**: Uses ENET channels for different data types (video, audio, control)
6. **Maintenance**: Handles keep-alives, error recovery, and reconnection

## Video Pipeline in moonlight-N3DS

### Hardware Decoding Path (New 3DS with MVD)
1. **Receive H.264 NAL units** via ENET from host PC
2. **Buffer NAL units** in linear memory allocated via linearAlloc
3. **Process through MVD**: 
   - Initialize MVD with `mvdstdInit()` specifying H.264 input, BGR565 output
   - Configure output dimensions and buffers
   - Submit decode units via `mvdstdProcessVideoFrame()`
   - Render frames with `mvdstdRenderVideoFrame()`
4. **Convert to RGB565**: MVD outputs BGR565 which matches 3DS framebuffer format
5. **Display**: Renderer writes pixels directly to framebuffer

### Software Decoding Fallback
1. **Use FFmpeg/libav** for H.264 decoding
2. **Convert to appropriate format** for 3DS display
3. **Handle via standard video path** in `src/video/n3ds_video.cpp`

### Renderer Architecture
- **N3dsRendererBase**: Abstract base class for all renderers
- **Specialized Renderers**:
  - `N3dsRendererTop`: Renders to top screen only
  - `N3dsRendererBottom`: Renders to bottom screen only
  - `N3dsRendererDualScreenStretch`: Stretches across both screens
  - `N3dsRendererDualScreenMirror`: Mirrors content on both screens
  - `N3dsRendererDualScreenMagnify`: Magnifies content with touch navigation

### Key Video Functions
- `n3ds_init()`: Sets up MVD hardware decoder
- `n3ds_decode()`: Processes video frames through MVD
- `n3ds_submit_decode_unit()`: Main entry point for video data from libgamestream
- `renderer->write_px_to_framebuffer()`: Outputs decoded pixels to display

## Audio Handling

### Audio Path
1. **Receive audio packets** via ENET from host PC
2. **Process through audio queue** (RtpAudioQueue from moonlight-common-c)
3. **Decode audio** using appropriate codec (typically Opus or AAC)
4. **Output to 3DS audio hardware** via citro3d audio system

### Implementation Details
- **Audio Callbacks**: Platform-specific audio renderer callbacks
- **Local Audio Option**: Can route audio to host PC instead of 3DS speakers
- **Audio Synchronization**: Timestamps used to sync audio with video
- **Buffer Management**: Careful handling to prevent audio glitches

### Key Audio Files
- `src/audio/n3ds_audio.cpp`: 3DS-specific audio implementation
- `src/RtpAudioQueue.c`: Audio packet jitter buffer and decoding
- `audio.h`: Audio interface definitions

## Input Mapping

### Input Flow
1. **Capture input** from 3DS hardware (circle pad, D-pad, touchscreen, buttons)
2. **Process through input handlers** (various TouchHandler implementations)
3. **Map to PC game inputs** (mouse, keyboard, gamepad)
4. **Transmit via control stream** to host PC using GameStream protocol

### Input Handlers
- **AbsoluteTouchHandler**: For stylus/touchscreen absolute positioning
- **GamepadTouchHandler**: Maps circle pad to analog stick
- **KeyboardTouchHandler**: On-screen keyboard for text input
- **MagnifyTouchHandler**: For zoom/pan in dual-screen magnify mode
- **MouseTouchHandler**: Maps touchscreen to mouse movement
- **N3dsTouchscreenInput**: Main touchscreen input coordinator

### Configuration Options
- Button remapping (swap face buttons, swap triggers/shoulders)
- Trigger usage (use ZL/ZR as mouse buttons)
- Motion controls (utilize 3DS gyroscope)
- Local audio toggle
- View-only mode (disable input transmission)

## Build System and Dependencies

### Build System
- **Primary**: Custom Makefile using devkitARM toolchain for 3DS development
- **Alternative**: Dockerfile for consistent build environment
- **Targets**: 
  - `.cia` file for installation via FBI
  - `.3dsx` file for homebrew launcher execution

### Makefile Structure
- **Source Organization**: Well-organized source directories
- **Include Paths**: Comprehensive list of third-party and project includes
- **Library Linking**: Extensive list of required libraries
- **Graphics Processing**: Custom rules for 3DS texture and shader processing
- **Banner Generation**: Tools for creating CIA banner and icon

### Key Dependencies
1. **devkitARM**: Nintendo 3DS development toolchain
2. **ctrulib**: 3DS system library
3. **citro2d/citro3d**: 2D/3D rendering libraries for 3DS
4. **libjpeg/libpng**: Image loading libraries
5. **zlib/bzip2**: Compression libraries
6. **freetype**: Font rendering
7. **opus**: Audio codec
8. **expat**: XML parsing
9. **curl**: HTTP client
10. **openssl/mbedtls**: Cryptographic libraries
11. **enet**: Networking library
12. **h264bitstream**: H.264 parsing
13. **libuuid**: UUID generation
14. **reedsolomon**: Error correction

### Build Process
1. **Setup**: Install devkitARM and required libraries
2. **Configuration**: Set DEVKITARM environment variable
3. **Compilation**: `make` command compiles all sources
4. **Packaging**: 
   - Creates .elf executable
   - Generates .cia installable image with bannertool and makerom
   - Optionally creates .3dsx homebrew executable

### Docker Build
- **Purpose**: Provides consistent build environment
- **Includes**: All cross-compilation tools and dependencies
- **Usage**: `docker build` and `docker run` followed by `make`

## Licensing and Dependencies

### Licensing
- **Moonlight-N3DS**: GNU General Public License v3.0
- **moonlight-common-c**: GNU General Public License v2.0
- **ENET**: MIT License
- **mbedTLS**: Apache License 2.0
- **h264bitstream**: BSD 2-Clause License
- **libuuid**: BSD 3-Clause License
- **reedsolomon**: Public Domain/MIT
- **Third-party 3DS libraries**: Various permissive licenses

### Dependency Summary
| Library | Purpose | License |
|---------|---------|---------|
| ENET | Reliable UDP transport | MIT |
| mbedTLS | Cryptography | Apache 2.0 |
| h264bitstream | H.264 parsing | BSD 2-Clause |
| libuuid | UUID generation | BSD 3-Clause |
| reedsolomon | Error correction | Public Domain/MIT |
| citro2d/citro3d | 3DS rendering | zlib/libpng |
| freetype | Font rendering | GPLv2+ or FTL |
| opus | Audio codec | BSD |
| expat | XML parsing | MIT |
| curl | HTTP transfer | MIT |
| OpenSSL | Cryptography (fallback) | OpenSSL/LGPL |

## Memory Constraints Handling

### Memory Management Strategies
1. **Linear Memory Allocation**: Uses `linearAlloc()` for video buffers to ensure contiguous physical memory
2. **Buffer Sizing**: Carefully calculates required buffer sizes for MVD hardware
3. **Memory Pooling**: Reuses buffers where possible to minimize allocation/freeing
4. **Static Allocation**: Uses static buffers for SOC (Socket) communication
5. **Memory Limits**: 
   - SOC buffers: 0x100000 (1MB) total for socket operations
   - Video buffers: Sized to match display capabilities (400x240 top, 320x240 bottom)
   - NAL unit buffer: Dynamically sized based on incoming packet sizes

### Specific Memory Areas
- **SOC_buffer**: 1MB aligned buffer for socket communications
- **nal_unit_buffer**: Dynamic buffer for H.264 NAL units with padding
- **rgb_img_buffer**: Video frame buffer matching 3DS texture format
- **Various small buffers**: For audio, input, and control data

## Architecture Summary

The moonlight-N3DS port represents a sophisticated adaptation of the Moonlight game streaming client to the constrained hardware of the Nintendo 3DS. Key achievements include:

1. **Hardware Acceleration**: Successful integration of the 3DS MVD hardware decoder for efficient video playback on New 3DS models
2. **Platform Adaptation**: Comprehensive abstraction layer allowing Moonlight to run on 3DS-specific hardware
3. **Resource Management**: Careful handling of limited memory and CPU resources
4. **Feature Completeness**: Full implementation of GameStream protocol features including pairing, encryption, input handling, and multiple display modes
5. **Build System**: Robust cross-platform build system supporting both traditional and Docker-based compilation

The architecture demonstrates how a complex streaming protocol can be adapted to run on severely constrained hardware while maintaining compatibility with the broader Moonlight ecosystem.