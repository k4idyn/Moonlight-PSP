# Safety Buffer

The Safety Buffer is a circular RAM cache for the last 2 seconds of H.264 NAL units. When the RTP reassembler detects a sequence gap (packet loss), it transitions to rewind state rather than crashing the RTSP connection. The HUD shows a brief rewind/pause icon while the network catches up.

## Architecture

```
Network Packets → RTP Reassembly → Safety Buffer → Decoder → Display
                                      ↓
                              Packet Loss Detection
                                      ↓
                              Rewind/Pause Icon
```

## Key Features

1. **Circular Buffer in RAM**: Stores the last 2 seconds of H.264 NAL units
2. **Memory Stick Fallback**: If RAM is full, automatically falls back to `ms0:/PSP/SAVEDATA/Moonlight/__temp_stream`
3. **Async Writer Thread**: File I/O runs in background without blocking decode
4. **Keyframe Detection**: Identifies IDR frames for safe rewind points
5. **State Machine**: Tracks buffering, idle, rewind, and paused states

## How It Works

### Normal Operation
1. NAL units are decoded and immediately stored in the safety buffer
2. Buffer maintains a 2-second sliding window of video data
3. Old entries are automatically evicted as new ones arrive

### Packet Loss Detection
1. When `rtp_reassembly.c` detects a sequence gap (missing packets)
2. Safety buffer transitions to `SAFETY_BUFFER_REWIND` state
3. Orange rewind/pause icon appears on screen
4. System waits ~2 seconds for network to stabilize

### Recovery
1. After timeout, icon disappears
2. System searches buffer for last keyframe (IDR frame)
3. Playback resumes from keyframe position
4. Buffer transitions back to `SAFETY_BUFFER_BUFFERING` state

## File Structure

```
Moonlight PSP/
├── safety_buffer.h       # Header with API definitions
├── safety_buffer.c       # Implementation
├── decoder_cpu.c         # Modified to store NALs before decode
├── rtp_reassembly.c      # Modified to detect packet loss
├── hud.c                 # Modified to show rewind icon
├── main.c                # Modified to initialize safety buffer
└── stream_session.c      # Modified to shutdown safety buffer
```

## API Reference

### Initialization
```c
int safety_buffer_init(void);
```
- Allocates 500 KB RAM pool for 2 seconds of video
- Opens fallback file on Memory Stick if RAM allocation fails
- Spawns async writer thread

### Storing NAL Units
```c
void safety_buffer_store_nal(u8 *nal_data, u32 nal_len, u64 pts, u8 is_keyframe);
```
- Called by `decode_nal()` before decoding
- Automatically manages 2-second circular window
- Detects keyframes (NAL type 5) for rewind points

### Packet Loss Handling
```c
void safety_buffer_handle_packet_loss(void);
```
- Called by `rtp_reassembly.c` when sequence gap detected
- Triggers rewind icon display
- Increments rewind counter for statistics

### Rewind Operation
```c
u8* safety_buffer_rewind(u32 *out_len, u64 *out_pts);
```
- Searches buffer for last keyframe
- Returns pointer to keyframe data
- Resets buffer position to keyframe

### Statistics
```c
void safety_buffer_get_stats(u32 *out_total_nals, u32 *out_total_bytes, 
                             u32 *out_rewind_count);
```
- Tracks total NALs buffered
- Tracks total bytes buffered
- Tracks number of rewinds performed

## Configuration Constants

| Constant | Value | Description |
|----------|-------|-------------|
| `SAFETY_BUFFER_DURATION_MS` | 2000 | Target buffer duration (2 seconds) |
| `SAFETY_BUFFER_SIZE_BYTES` | 512000 | RAM pool size (500 KB) |
| `SAFETY_BUFFER_MAX_NAL_SIZE` | 262144 | Maximum NAL size (256 KB) |
| `SAFETY_BUFFER_SLOTS` | 128 | Number of circular buffer slots |
| `SAFETY_BUFFER_FALLBACK_PATH` | "ms0:/PSP/SAVEDATA/Moonlight/__temp_stream" | Fallback file path |

## State Machine

```
SAFETY_BUFFER_IDLE
       ↓
SAFETY_BUFFER_BUFFERING (initial state)
       ↓
SAFETY_BUFFER_REWIND (packet loss detected)
       ↓
SAFETY_BUFFER_BUFFERING (after rewind)
```

## Memory Usage

- **RAM Pool**: 500 KB (configurable)
- **Slot Array**: 128 × 16 bytes = 2 KB
- **Total RAM**: ~502 KB

## Performance Impact

- **CPU Overhead**: Minimal (runs in async thread)
- **Latency**: <1ms per NAL unit stored
- **Disk I/O**: Only when RAM is full (rare)

## Integration Points

### decoder_cpu.c
```c
// Added to decode_nal():
u8 nal_type = nal_data[0] & 0x1F;
g_is_keyframe = (nal_type == 5); // IDR frame
safety_buffer_store_nal(nal_data, nal_len, g_current_pts, g_is_keyframe);
```

### rtp_reassembly.c
```c
// Added to handle_sequence_gap():
safety_buffer_handle_packet_loss();
```

### hud.c
```c
// Added to hud_render():
if (hud_should_show_rewind_icon()) {
    draw_rewind_icon(FRAME_WIDTH / 2 - 24, FRAME_HEIGHT / 2 - 24);
}
```

## Troubleshooting

### Buffer Not Working
1. Check if RAM allocation succeeded (see debug output)
2. Verify Memory Stick is inserted if using fallback
3. Check for file write errors in debug output

### Rewind Icon Not Appearing
1. Verify `hud_init()` was called
2. Check if `hud_should_show_rewind_icon()` returns true
3. Verify GU rendering is active

### Performance Issues
1. Reduce `SAFETY_BUFFER_SIZE_BYTES` to free RAM
2. Increase `SAFETY_BUFFER_DURATION_MS` for more buffering
3. Monitor rewind count for excessive packet loss

## Known Limitations

- RAM-only mode is the common path. Memory Stick fallback (`ms0:/PSP/SAVEDATA/Moonlight/__temp_stream`) is only used when RAM allocation fails, which is uncommon on PSP-2000/3000.
- Rewind resumes from the last IDR frame. At low bitrates or high frame rates, the keyframe interval may be several seconds, causing a visible seek on recovery.
- The rewind icon duration is 2 seconds regardless of actual recovery time. Persistent packet loss will retrigger it repeatedly.
