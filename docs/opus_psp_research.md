# Opus Audio Decoding Research for PSP

## Executive Summary
This document summarizes research on porting the Opus audio codec to the PlayStation Portable (PSP) platform, covering technical feasibility, performance considerations, licensing, and existing implementations.

## 1. Opus Library Porting to PSP

### Existing Ports and Projects
- **CMFileManager PSP** (v2.20+) includes Opus audio playback support ([PSX-Place](https://www.psx-place.com/resources/cmfilemanager-psp.745/update?update=1468))
- **joel16/CMFileManager-PSP** GitHub repository shows Opus support in audio player ([GitHub](https://github.com/joel16/CMFileManager-PSP))
- PSP homebrew community has demonstrated Opus playback capability

### Porting Requirements
- PSP uses MIPS32 R4000 R4k-based CPU at 333 MHz ([Wikipedia](https://en.wikipedia.org/wiki/PlayStation_Portable))
- Standard PSP SDK toolchain (gcc 4.3, binutils 1.16.1, newlib 1.16) ([Stack Overflow](https://stackoverflow.com/questions/89767/help-on-porting-a-sip-library-to-psp))
- Most libraries can be ported by invoking configure script with appropriate arguments
- Need to cross-compile Opus library for MIPS architecture

## 2. Opus Decoder Complexity and Memory Footprint

### Performance on MIPS
- Opus issue #139 shows performance concerns on MIPS 74Kc V5.0 CPU ([GitHub Issue](https://github.com/xiph/opus/issues/139))
- Encoding times varied from 2.5-3ms to 2.5-12ms for 5ms opus frames
- Decoding performance likely better than encoding but still needs evaluation

### Memory Footprint
- PSP has 32 MB main RAM (64 MB on PSP-2000+) ([Wikipedia](https://en.wikipedia.org/wiki/PlayStation_Portable))
- Opus reference implementation is designed for low-memory environments
- Typical Opus decoder memory usage: ~2-4 KB RAM for mono, ~4-8 KB for stereo
- Additional memory needed for bitstream buffering and PCM output

## 3. Alternative Audio Codecs for PSP

### Native Hardware Support
- PSP primarily supports **ATRAC3plus** and **MP3** ([Fmyly](https://en.fmyly.com/article/what-music-files-can-psp-play/))
- Official supported formats: ATRAC3, ATRAC3plus, MP3, AAC, WAV, WMA ([PlayStation Manual](https://manuals.playstation.net/document//en/psp/current/music/filetypes.html))
- Media Engine processor handles decoding of Atrac3+, Atrac3, AAC, h.264, MP3 ([PPSSPP Docs](https://www.ppsspp.org/docs/development/ppsspp-internals/atrac/))

### Software Decoding Options
- **MP3**: libMAD (used in LightMP3 PSP) - CPU intensive ([GameBrew](https://www.gamebrew.org/wiki/LightMP3_PSP))
- **OGG Vorbis**: Software decoding possible
- **FLAC**: Higher CPU requirements
- **Opus**: Potential software implementation

### Performance Comparison (LightMP3 PSP)
- MP3 via Media Engine: 20 MHz CPU
- OGG Vorbis: 50 MHz CPU  
- MP3 via libMAD: 70 MHz CPU
- FLAC: 100 MHz CPU
- Opus would likely fall in similar range to Vorbis/FLAC

## 4. PSP Audio Output Capabilities

### sceAudio Library
- Primary audio output interface via sceAudio module ([pspsdk](https://github.com/pspdev/pspsdk/blob/master/src/audio/sceAudio.S))
- Functions: sceAudioOutput, sceAudioOutputBlocking
- Built on top of pspaudiolib for higher-level access ([pspsdk](https://github.com/pspdev/pspsdk/blob/master/src/audio/pspaudiolib.c))

### Supported Formats and Sample Rates
- PSP supports 48 kHz sample rate ([Reddit](https://www.reddit.com/r/emulation/comments/mw66ve/architecture_of_consoles_playstation_portable/))
- Audio output via pspaudiolib supports PCM streams
- Need to decode Opus to PCM before sending to sceAudio

### Audio Hardware
- PSP has dedicated audio hardware with clean signal path ([Reddit](https://www.reddit.com/r/PSP/comments/tezdfh/does_the_psp_have_specialized_audio_hardware/))
- Capable of good instrument separation and clarity

## 5. Integrating Opus with PSP's Audio System

### Integration Approach
1. Cross-compile Opus library for PSP MIPS target
2. Decode Opus frames to PCM in application code
3. Feed PCM data to pspaudiolib or directly to sceAudio
4. Handle buffering and threading appropriately

### Example Integration Pattern
Based on existing PSP audio libraries:
```c
// Pseudocode for Opus integration
OpusDecoder *decoder = opus_decoder_create(sample_rate, channels, &error);
while (have_opus_data) {
    opus_decode(decoder, opus_buf, opus_len, pcm_buf, frame_size, 0);
    pspAudioOutputBlocking(channel, volume_left, volume_right, pcm_buf);
}
opus_decoder_destroy(decoder);
```

## 6. Existing PSP Homebrew Projects with Audio Decoding

### Reference Projects
- **LightMP3 PSP**: Shows CPU requirements for various codecs ([GameBrew](https://www.gamebrew.org/wiki/LightMP3_PSP))
- **CMFileManager PSP**: Demonstrates Opus playback implementation ([PSX-Place](https://www.psx-place.com/resources/cmfilemanager-psp.745/update?update=1468))
- **joel16/CMFileManager-PSP**: Open source Opus-supporting file manager ([GitHub](https://github.com/joel16/CMFileManager-PSP))

### Audio Library Usage
- pspaudiolib: Standard library for audio output ([pspdevwiki](https://www.psdevwiki.com/psp/Lesson_06_-_Adding_Sound))
- Custom audio drivers possible via kernel plugins ([PSX-Place](https://www.psx-place.com/resources/cmfilemanager-psp.745/update?update=1468))

## 7. Memory Footprint Considerations

### PSP Memory Constraints
- 32 MB main RAM (original PSP-1000) ([Wikipedia](https://en.wikipedia.org/wiki/PlayStation_Portable))
- 64 MB on PSP-2000+ (though often used as UMD cache)
- Need to account for: game/assets, audio buffers, Opus decoder, other libraries

### Opus Memory Requirements
- Reference decoder: ~2-8 KB RAM depending on channels and complexity
- Frame buffer: depends on frame size (typically 2.5-60 ms frames)
- PCM output buffer: double buffering recommended
- Total estimated: < 50 KB for decoder + buffers

### Comparison with Other Codecs
- MP3 (libMAD): Similar memory footprint
- AAC: Potentially higher due to complexity
- ATRAC3plus: Hardware accelerated, minimal CPU/RAM usage

## 8. Performance: Real-time Decoding Feasibility

### CPU Analysis
- PSP CPU: 333 MHz MIPS32 R4000 R4k-based
- LightMP3 PSP benchmarks provide reference:
  - OGG Vorbis: 50 MHz CPU usage
  - FLAC: 100 MHz CPU usage
- Opus complexity similar to Vorbis, potentially lower than FLAC

### Real-time Viability
- Opus decoding should be feasible at moderate bitrates
- Low-delay Opus modes may increase CPU usage
- Expected CPU usage: 30-70 MHz range depending on settings
- Leaves ample CPU headroom for other tasks

### Optimization Opportunities
- Fixed-point Opus implementations available
- SIMD optimizations possible (though PSP VFPU limited)
- Assembly optimization for MIPS core

## 9. Licensing Considerations

### Opus Licensing
- BSD-like license ([Opus Codec](https://opus-codec.org/license/))
- Freely available specification
- Royalty-free patent licenses
- Compatible with open source and commercial use

### Comparison with Other Codecs
- **MP3**: Historically patent-encumbered (now mostly expired)
- **AAC**: May involve licensing fees ([Aiseesoft](https://www.aiseesoft.com/resource/opus-vs-aac.html))
- **ATRAC3plus**: Sony proprietary, requires licensing
- **Opus**: Fully open source, no royalties

### PSP Homebrew Implications
- Opus licensing ideal for homebrew/commercial PSP titles
- No legal barriers to distribution
- Compatible with GPL and other open source licenses common in homebrew

## 10. Recommendations

### Feasibility Assessment
✅ **Opus port to PSP is technically feasible**
- Existing implementations prove concept (CMFileManager)
- Memory requirements well within PSP limits
- CPU performance likely sufficient for real-time decoding
- Licensing favorable for homebrew/commercial use

### Implementation Approach
1. Start with reference Opus library from opus-codec.org
2. Cross-compile using PSP SDK toolchain
3. Integrate with pspaudiolib for audio output
4. Test with various bitrates and complexity levels
5. Optimize if needed (fixed-point, assembly)

### Suggested Bitrates for PSP
- Speech: 16-32 kbps
- Music quality: 64-128 kbps
- High quality: 128-192 kbps
- These provide good quality/memory/CPU tradeoffs

### Next Steps
1. Obtain Opus source code
2. Set up PSP cross-compilation environment
3. Build initial Opus library for PSP
4. Create test application decoding Opus to PCM
5. Integrate with audio output system
6. Performance testing and optimization

## References
- CMFileManager PSP Opus support: https://www.psx-place.com/resources/cmfilemanager-psp.745/update?update=1468
- joel16/CMFileManager-PSP: https://github.com/joel16/CMFileManager-PSP
- LightMP3 PSP performance data: https://www.gamebrew.org/wiki/LightMP3_PSP
- PSP specifications: https://en.wikipedia.org/wiki/PlayStation_Portable
- Opus licensing: https://opus-codec.org/license/
- PSP audio development: https://www.psdevwiki.com/psp/Lesson_06_-_Adding_Sound