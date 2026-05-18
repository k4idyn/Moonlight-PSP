# RTP Packet Reassembly

This module handles RTP packet reassembly for the PSP Moonlight video stream. It converts raw UDP payloads into complete H.264 NAL units for the OpenH264 decoder. Supports single NAL units and FU-A fragmented NAL units per RFC 6184.

## Overview

The module converts raw UDP payloads (12-byte RTP header + payload) into complete H.264 Annex-B NAL units ready for `oh264_pipeline_decode_frame()`.

### Supported NAL Unit Types

- **Single NAL Units (Types 1-23)**: Complete NAL units contained in a single RTP packet
- **FU-A Fragmented NAL Units (Type 28)**: NAL units split across multiple RTP packets

### Key Features

- **Lock-free operation**: Works with the existing PacketRingBuffer design
- **Sequence tracking**: Detects packet loss and reordering
- **Static buffer**: Uses a fixed 65536-byte assembly buffer
- **Single-threaded**: Designed for the PSP's main CPU

## Files

- `rtp_reassembly.c` — Main implementation
- `rtp_reassembly.h` — Public API header
- `rtp_fec.c` — Reed-Solomon FEC repair (called before reassembly)

## API Usage

### Basic Usage

```c
#include "rtp_reassembly.h"

// Initialize the decoder (resets RTP reassembly state)
decoder_init(&g_shared.frame_buffer);

// Process packets from the ring buffer
while (1) {
    // Check if there are packets to process
    if (g_shared.packet_ring.tail != g_shared.packet_ring.head) {
        decoder_process_packet(&g_shared.packet_ring, &g_shared.frame_buffer);
    }
    // Sleep or do other work...
}
```

### Processing Flow

1. **Packet Reception**: The ME network thread writes UDP packets to the ring buffer
2. **Packet Reading**: The ME decoder thread reads packets from the ring buffer tail
3. **RTP Parsing**: The 12-byte RTP header is parsed to extract the sequence number
4. **NAL Type Detection**: The first payload byte determines if this is a single NAL or FU-A fragment
5. **Reassembly**: Single NAL units and FU-A fragments are accumulated into the 64 KB static assembly buffer.
6. **Annex-B formatting**: Reassembled NAL units are prefixed with the 4-byte Annex-B start code (`00 00 00 01`) before being passed to the decoder.
7. **Decoding**: Complete NAL units are passed to `oh264_pipeline_decode_frame()` on the main CPU.

## RTP Packet Format

### RTP Header (12 bytes)

```
Byte 0:  [version:2][P:1][X:1][CC:4]
Byte 1:  [M:1][PT:7]
Byte 2-3: [sequence:16]  (network byte order)
Byte 4-7: [timestamp:32] (network byte order)
Byte 8-11: [ssrc:32]
```

### Single NAL Unit Format

```
[RTP Header (12 bytes)] [NAL Unit Data]
```

The first byte of the NAL unit contains:
- Bit 7: forbidden_zero_bit (should be 0)
- Bits 6-5: nal_ref_idc (priority)
- Bits 4-0: nal_unit_type (1-23 for single NAL units)

### FU-A Fragmented Format

```
[RTP Header (12 bytes)] [FU Indicator (1 byte)] [FU Header (1 byte)] [Fragment Data]
```

**FU Indicator**:
- Bits 7-5: Same as NAL header (forbidden_zero_bit, nal_ref_idc)
- Bits 4-0: Always 28 (FU-A type)

**FU Header**:
- Bit 7: Start bit (1 = first fragment)
- Bit 6: End bit (1 = last fragment)
- Bit 5: Reserved (must be 0)
- Bits 4-0: Original NAL unit type

## Sequence Number Handling

The module tracks RTP sequence numbers to detect:
- **Packet Loss**: Gaps in sequence numbers
- **Reordering**: Out-of-order packet delivery

When a sequence gap is detected:
1. Any partially reassembled NAL unit is discarded
2. The reassembly state is reset
3. Processing continues with the current packet

This ensures that only complete, correctly ordered NAL units are passed to the decoder.

## Implementation Details

### Static Variables

```c
static u8 nal_buffer[65536];      // Assembly buffer
static u32 nal_buffer_pos;        // Current position in buffer
static u16 expected_sequence;     // Next expected sequence number
static u8 reassembling;           // Flag: currently reassembling FU-A NAL
static u8 reassembled_nal_type;   // Original NAL type being reassembled
```

### Memory Usage

- **Assembly Buffer**: 65536 bytes (static)
- **State Variables**: ~10 bytes
- **Total**: ~64 KB

This fits comfortably within the PSP's RAM constraints.

## Integration with Existing Code

The module integrates seamlessly with the existing PSP Moonlight codebase:

1. **SharedState**: Uses the existing `PacketRingBuffer` structure
2. **Decoder Interface**: Calls `decode_nal()` when complete NAL units are ready
3. **Threading Model**: Works with the lock-free ring buffer design

### Integration

The reassembler is driven by `sw_decoder_thread.c`, which reads from the 1024-slot packet ring buffer and calls `rtp_reassembly_process_packet()`. When a complete NAL unit is ready, the callback calls `oh264_pipeline_decode_frame()` on the main CPU. FEC repair (`rtp_fec.c`) is applied to each RTP frame group before passing to reassembly.

## Error Handling

The module handles various error conditions gracefully:

- **Invalid RTP packets**: Discarded silently
- **Sequence gaps**: Partial NAL units are discarded; the decoder state is reset to wait for the next IDR frame to prevent visual artifacts and hardware hangs (`0x80628002`).
- **Buffer overflow**: Fragments that would exceed the buffer are discarded
- **Out-of-order packets**: Older packets are discarded

## Performance Considerations

- **Zero-copy**: Single NAL units are passed directly without copying
- **Minimal overhead**: FU-A reassembly uses efficient `memcpy()` operations
- **No locks**: Thread-safe operation without mutexes
- **Static allocation**: No dynamic memory allocation

## Testing

To test the module:

1. Send RTP packets with single NAL units (types 1-23)
2. Send RTP packets with FU-A fragmented NAL units (type 28)
3. Introduce packet loss and verify sequence gap detection
4. Send out-of-order packets and verify they are handled correctly

## Known Limitations

- STAP-A (type 24) and FU-B (type 29) are not supported — Sunshine does not send these in practice.
- The 64 KB assembly buffer is static. NAL units larger than 64 KB are dropped with a logged error.
- Sequence number tracking wraps at 65535; rollover handling is implemented but not stress-tested.

## References

- RFC 6184: RTP Payload Format for H.264 Video
- RFC 3550: RTP: A Transport Protocol for Real-Time Applications
- H.264/AVC Standard (ITU-T H.264)