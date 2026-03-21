Complete the PSP Moonlight client by implementing the missing "glue" between the `libgamestream` network callbacks and the hardware-specific decoder/input modules. Currently, the NAL units and Opus packets are dropped because the [dr_submit_decode_unit](file:///c:/Users/beelink/Desktop/moonlight3/psp_moonlight/src/modules/network_receiver.c#138-139) and [ar_decode_and_play_sample](file:///c:/Users/beelink/Desktop/moonlight3/psp_moonlight/src/modules/network_receiver.c#144-145) callbacks in [network_receiver.c](file:///c:/Users/beelink/Desktop/moonlight3/psp_moonlight/src/modules/network_receiver.c) are empty stubs. Input mapping is also not being transmitted back to the host.

We will also build an autonomous debugging loop using AutoHotkey (AHK) and PPSSPP to continuously monitor, diagnose, and refine the application. The app will include graceful error handling (network drops, decoder errors) with meaningful onscreen messages, and follow UI paradigms inspired by Moonlight N3DS.

This plan will connect these pieces, enabling video rendering, audio playback, and controller inputs.

## Proposed Changes

### Core Integration
#### [MODIFY] src/main.c
- **Global Exports / Setters**: Ensure `g_video_decoder` and `g_audio_decoder` are accessible to [network_receiver.c](file:///c:/Users/beelink/Desktop/moonlight3/psp_moonlight/src/modules/network_receiver.c).
- **Input Loop**: Within the `STATE_STREAMING` block, fetch the `InputState` from [input_mapper](file:///c:/Users/beelink/Desktop/moonlight3/psp_moonlight/src/modules/input_mapper.c#44-57) and call a new export in [network_receiver.c](file:///c:/Users/beelink/Desktop/moonlight3/psp_moonlight/src/modules/network_receiver.c) to transmit `LiSendControllerEvent()` to the host.

#### [MODIFY] src/modules/network_receiver.c
- **Video Callback**: Update [dr_submit_decode_unit](file:///c:/Users/beelink/Desktop/moonlight3/psp_moonlight/src/modules/network_receiver.c#138-139) to pass `decodeUnit->buffer` of size `decodeUnit->fullLength` to [video_decoder_submit_frame](file:///c:/Users/beelink/Desktop/moonlight3/psp_moonlight/src/modules/video_decoder.c#112-128).
- **Audio Callback**: Update [ar_decode_and_play_sample](file:///c:/Users/beelink/Desktop/moonlight3/psp_moonlight/src/modules/network_receiver.c#144-145) to pass `sampleData` to [audio_decoder_submit_packet](file:///c:/Users/beelink/Desktop/moonlight3/psp_moonlight/src/modules/audio_decoder.c#130-154).
- **Input Function**: Add a new function `network_receiver_send_input(InputState *state)` that constructs and fires `LiSendControllerEvent()` (and potentially `LiSendControllerMoveEvent()` if analog logic demands it).

#### [MODIFY] src/modules/network_receiver.h
- Expose setter functions for the decoders (e.g., `network_receiver_set_video_decoder`) and the input transmission function.

#### [MODIFY] src/modules/audio_decoder.c
- Fine-tune Opus decoder settings as necessary if the frame sizes don't perfectly match the default 960 sample 20ms chunk that `libgamestream` pushes. Ensure thread safety if called directly from the network reception thread.

#### [MODIFY] src/modules/video_decoder.c
- Implement resilient error handling during `sceMpegAvcDecode` failures. Provide mechanisms to return error state to the main UI loop to display onscreen messages.

#### [NEW] auto_test.ahk
- Create an AHK script that:
  - Completely kills any existing [PPSSPPWindows64.exe](file:///c:/Users/beelink/Desktop/moonlight3/PPSSPP_x64/PPSSPPWindows64.exe) and `AutoHotkey.exe` processes before starting.
  - Compiles the latest codebase using `make -f Makefile.psp`.
  - On build success, launches [PPSSPPWindows64.exe](file:///c:/Users/beelink/Desktop/moonlight3/PPSSPP_x64/PPSSPPWindows64.exe) with the newly built [EBOOT.PBP](file:///C:/Users/beelink/Desktop/moonlight3/psp_moonlight/EBOOT.PBP).
  - Automatically handles any PPSSPP popup windows (like network permission or crash dialogs).
  - Monitors the PPSSPP debug logs or output to detect crashes or connection drops (timeout errors/decoder errors).
  - Automatically loops this process (or exits on success/crash to allow for log review).

## Verification Plan

### Automated Tests/Manual Verification (PPSSPP Only)
- **Primary Objective**: Utilize PPSSPP **exclusively** to confirm the app boots, connects to a local Moonlight host, successfully streams an app with audio/video, and responds to inputs without crashing.
- **Error Handling Validation**: Ensure that if the connection drops, times out, or the decoder fails (e.g. `sceMpegAvcDecode` returns an error), the app gracefully returns to the main menu (or a dedicated error screen) with a meaningful onscreen message (e.g., "Connection Lost: Timeout" or "Decoder Error: X").
- **Hardware constraints**: No real PSP hardware testing will be considered until the PPSSPP implementation runs 100% perfectly with UI, video, audio, inputs, and network optimization.
