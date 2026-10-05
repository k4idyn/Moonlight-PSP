# Streaming and controls

| Preset | Video | Bitrate | Packet size | PSP audio |
|---|---|---|---|---|
| Quality | 480x272,15fps | 576kbps | 1200bytes | On |
| Balanced (default) | 360x204,20fps | 480kbps | 1200bytes | On |
| Performance | 300x170,30fps | 384kbps | 1056bytes | Off |

Balanced is the recommended default for visual detail, smoothness and audio. New installations select it automatically; upgrades retain saved settings. Performance is the 30 fps preset; Quality is the native-resolution 15 fps preset. Fast-moving details and small desktop text can remain coarse.

Select Xbox mode for virtual gamepad controls or Browser mode for mouse and keyboard navigation. Change assignments in the mapping menu and use its analog direction hints.

## Browser controls

| PSP input | Host action |
|---|---|
| Analog nub | Move the mouse |
| L / R | Left / right mouse button; hold to drag |
| L + Up / Down | Scroll the mouse wheel |
| D-pad | Arrow keys |
| Cross / Circle | Enter / Escape |
| Triangle / Square | Tab / Space |
| Start / Select | Enter / Escape |
| R + Triangle | Enter shortcut |
| R + Up | Open the PSP HUD |
| Circle in the HUD | Close the HUD |
| Start + Select | Leave the stream |

HUD controls are consumed locally until their held buttons are released.

The hardware decoder reads the emitted PPS to select CAVLC/Baseline or CABAC/Main before decoding an IDR. After changing Apollo's encoder configuration, restart Apollo and check that the intended mode is loaded before reconnecting.

Keep host display geometry stable while streaming. Exit the stream through the client's controls before closing the application.
