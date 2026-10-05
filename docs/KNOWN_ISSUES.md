# Known Issues

_PSP Moonlight v1.5.0_

## Starting Moonlight

Keep `EBOOT.PBP` and `moonlight_me_helper.prx` from the same package in the same folder. Moonlight loads the helper when it starts.

If initialization fails, the error screen shows the step and hexadecimal error code. The last fatal error is saved to `ms0:/PSP/SAVEDATA/Moonlight/error.log` for bug reports. Release the launch button, then press a button to leave the error screen.

## Wireless streaming

The PSP supports 2.4 GHz Wi-Fi only. Crowded channels, weak signal and burst loss can cause stutter or recovery pauses. Keep the PSP close to the access point and use a clear channel.

## Video quality and frame rate

Quality prioritizes native 480x272 detail at 15 fps. Balanced uses 360x204 at 20 fps. Performance prioritizes 30 fps at 300x170. Fast motion and small desktop text can appear soft or blocky at these low bitrates.

Configure the host for H.264 Baseline/CAVLC or Main/CABAC. The hardware decoder selects its mode from the stream. Some host encoder settings take effect only after restarting the host streaming service.

## Audio

Performance disables local PSP audio to prioritize video and input responsiveness. Quality and Balanced enable audio. Bursty audio packets can cause brief gaps, repeated samples or delay.

## Display changes

Some host configurations use the physical desktop when a virtual display is unavailable. Changing desktop resolution during a stream can interrupt video. Restore a stable display mode and reconnect.

## Connection recovery

If a stream cannot start or a decoder error prevents reconnection, leave Moonlight and launch it again. Keep the application and helper from the same package together.

## Hotspot and remote sessions

UPnP port mapping depends on gateway support. It may be unavailable behind carrier-grade NAT or when the gateway disables UPnP.

## Unsupported formats

- H.265 and AV1 decoding.
- Resolutions above the PSP display's intended operating range.
- Desktop-class image detail at modern streaming bitrates.
