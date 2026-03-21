# PSP Media Engine H.264 Decoding Research

## Executive Summary
The PSP Media Engine (ME) is a dedicated hardware block designed for multimedia decoding, including H.264/AVC video. This research compiles findings about its capabilities, access methods for homebrew developers, performance characteristics, and existing projects that utilize it.

## 1. PSP Media Engine (ME) Capabilities for H.264 Decoding

### Overview
- The Media Engine is a second MIPS32 R4000-based CPU core in the PSP SoC
- Contains dedicated hardware for multimedia decoding including H.264/AVC
- Works in conjunction with specialized hardware like the Virtual Mobile Engine (VME) and AVC blocks
- Not directly accessible to licensed developers; Sony runs proprietary code on it for decoding
- Features 2MB of embedded DRAM (eDRAM) for local processing to avoid stalling other components

### Supported Profiles and Levels
Based on community findings and encoding guides:
- **Baseline Profile**: Supported (required for CABAC-free decoding)
- **Main Profile**: Supported (confirmed by PSP v3.30 firmware update)
- **High Profile**: Limited evidence; likely not fully supported due to CABAC requirements
- **Levels**: 
  - Level 3.0 confirmed working for 720x480 resolution
  - Level 3.1 mentioned in encoding discussions
  - Level 4.0+ likely not supported due to memory bandwidth constraints

### Resolution and Frame Rate Limits
- Native PSP screen resolution: 480x272
- Supported resolutions per PSP v3.30 firmware: 720x480, 352x480, 480x272
- Frame rates: Up to 30fps confirmed for standard definition content
- Reference frames: Limited to low numbers (PSP struggles with 4+ reference frames in software decoding)

## 2. Accessing ME from Homebrew

### Methods and Libraries
- **Official Frameworks**: Sony provides frameworks like 'libmpeg' and 'libmp3' that implement complete applications optimized for the ME
- **Homebrew Access Projects**:
  - `psp-media-engine-custom-core`: Library project to map PSP's Media Engine core functions for homebrew use
  - Community efforts to reverse engineer and expose ME functions through kernel/syscall interfaces
  - `mebooter`: Contains RPC system for controlling codecs and VME
- **Syscall/Kernel Access**: 
  - Functions available in specific kernel areas
  - Requires mapping native kernel/core functions to make them available to homebrew
  - Projects like LibPspExploit aim to ease creation of OFW-compatible homebrew with kernel access

### Technical Approach
- Homebrew developers typically cannot run arbitrary code on the ME due to lack of kernel functions
- Instead, they interface with existing Sony-provided codecs through exposed APIs
- Some projects attempt to load custom code onto the ME using mebooter or similar techniques
- Direct ME programming is challenging due to limited documentation and proprietary nature

## 3. Existing Homebrew Projects Using ME for Video Decoding

### Notable Projects
- **PSP Media Center**: Aims for H.264 video support using Sony's libraries from flash
- **PMFplay H.264 Decoder**: Codec that enables PMF playback through default media players
- **PMPMod**: AVC media player for PSP
- **Leap Homebrew Engine**: Includes video decoding capabilities
- **PPSSPP**: While primarily an emulator, its internals documentation provides insights into ME implementation

### Community Efforts
- Forum discussions on ps2dev.org regarding H.264 support using Sony's flash libraries
- GitHub repositories like `mcidclan/psp-media-engine-custom-core` providing access libraries
- Homebrew video converters targeting PSP-specific H.264 requirements

## 4. Input Format Requirements

### NAL Unit Streaming and Packetization
- Input format: H.264 NAL unit streams (Annex B format common)
- Required NAL units: SPS (Sequence Parameter Set), PPS (Picture Parameter Set), IDR frames
- PSP-specific atoms: Some reports indicate PSP expects certain atoms in MP4 containers
- Packetization: Likely expects standard H.264 byte stream or packetized formats
- Homebrew tools: ffmpeg with `-bsf:v h264_mp4toannexb` for converting to Annex B format

### Specific Requirements
- Baseline/Main profile compliance
- Limited reference frames (typically 1-3)
- Specific resolution constraints (480x272 native, 720x480 max with firmware updates)
- Properly formatted SPS/PPS with PSP-compatible parameters

## 5. Output Format and VRAM Access

### Output Formats
- Primary output: YUV420 planar format (yuv420p)
- Some evidence of RGB output capabilities through post-processing
- Virtual Mobile Engine (VME) likely handles color space conversion
- Output typically delivered to main memory or VRAM for display

### Getting Frames to VRAM
- ME decodes to local eDRAM
- Results transferred to main memory via memory controller
- Graphics Core (GE) accesses decoded frames from main memory/VRAM
- Likely involves DMA transfers or memory-mapped interfaces
- Homebrew access typically through Sony-provided libraries that handle memory management

## 6. Performance Characteristics

### Latency and Throughput
- Hardware decoding is significantly faster than software decoding on the main CPU
- Enables real-time playback of video that would be impossible via software decoding
- Low latency suitable for interactive applications
- Throughput sufficient for DVD-quality video (approx. 1.5-3 Mbps for typical PSP content)

### Power Consumption
- Hardware decoding consumes significantly less power than software decoding
- Critical for battery life during video playback
- ME can be powered down when not in use
- More efficient than running H.264 decode on main Allegrex CPU

### Comparison with Software Decoding
- Software H.264 decoding on PSP's main CPU is impractical for real-time playback
- Hardware acceleration essential for any reasonable video playback experience
- Main CPU lacks sufficient power for High Profile or complex Baseline/Main streams
- ME provides dedicated resources, avoiding contention with game/application logic

## 7. Documentation and Reverse Engineering Efforts

### Official Documentation
- Official Sony PSP SDK documentation is limited and not publicly available
- Some information leaked through official SDKs and documentation
- Sony keeps ME details proprietary; no public register-level documentation

### Reverse Engineering Resources
- **PSP Developer Wiki (psdevwiki.com)**: Primary source of community knowledge
  - Media Engine page: Details on ME architecture and known functions
  - Tachyon documentation: Information on ME's role in audio/video decoding
- **PPSSPP Emulator**: Open-source PSP emulator with documented ME implementation
  - Provides insights into how ME communicates with main system
  - Shows how decoded results are delivered back to the main CPU
- **Homebrew Forum Discussions**: 
  - ps2dev.org forums contain valuable information on ME access
  - Doom9 forums discuss PSP-specific H.264 encoding requirements
  - Reddit communities (r/hardware, r/psp) share reverse engineering findings
- **Specific Projects**:
  - `psp-media-engine-custom-core`: Active reverse engineering effort
  - Various homebrew media players that interface with ME codecs
  - PSP Media Center project attempting to leverage Sony's flash libraries

### Key Findings from Reverse Engineering
- ME contains a second MIPS core running at similar speed to main CPU
- Dedicated hardware blocks for specific codecs (H.264, MPEG-2, etc.)
- Communication occurs through mailbox/interrupt mechanisms
- Firmware updates have added capabilities (e.g., PSP v3.30 adding Main Profile support)
- ME firmware is updatable through system updates

## 8. Comparison: Hardware vs Software Decoding on PSP

### Why Hardware is Essential
1. **Processing Power**: Main CPU (~333 MHz MIPS) insufficient for real-time H.264 decode
2. **Power Efficiency**: Hardware decoding uses fraction of power compared to software
3. **Real-time Performance**: Hardware meets real-time constraints; software does not
4. **Resource Isolation**: ME decoding doesn't compete with game logic for CPU cycles
5. **Memory Bandwidth**: Dedicated eDRAM reduces contention with main memory subsystem

### Software Decoding Limitations
- Attempts at software H.264 decode on PSP show poor performance
- Limited to very low resolutions and frame rates
- High CPU usage leaves little room for other processing
- Not practical for entertainment video applications

## Conclusion

The PSP Media Engine provides essential hardware acceleration for H.264 video decoding, making video playback practical on the device. While access for homebrew developers is challenging due to Sony's proprietary nature, the community has made significant progress in reverse engineering and creating interfaces to leverage ME capabilities. Key limitations include profile/level restrictions (primarily Baseline and Main profiles up to level 3.1) and resolution constraints, but within these bounds, the ME delivers efficient, real-time video decoding performance that would be impossible to achieve through software means on the PSP's main CPU.

For homebrew developers seeking to use ME for H.264 decoding:
1. Target Baseline profile, level 3.0 or lower
2. Use resolutions up to 720x480 (with appropriate firmware) or 480x272 (native)
3. Interface through existing libraries like libmpeg or community ME access projects
4. Expect YUV420 output requiring conversion for display
5. Leverage community reverse engineering efforts for detailed ME access methods

## References
- Wikipedia: PlayStation Portable hardware
- PS2Dev forums (ps2dev.org)
- PSP Developer Wiki (psdevwiki.com)
- PPSSPP emulator documentation
- Various homebrew projects and forum discussions
- Video encoding guides specific to PSP limitations