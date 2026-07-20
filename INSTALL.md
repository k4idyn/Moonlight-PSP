# Installation Guide - PSP Moonlight v1.4.0

## Prerequisites

- PSP with custom firmware installed. ARK-4 on 6.60/6.61 is the supported target.
- Memory Stick or internal storage on PSP Go.
- 2.4 GHz Wi-Fi network. The PSP does not support 5 GHz Wi-Fi.
- Host PC running [Sunshine](https://github.com/LizardByte/Sunshine) current stable release.

---

## Step 1: Install Custom Firmware

If your PSP already has custom firmware, skip to Step 2.

1. Download the latest ARK-4 release from [github.com/PSP-Archive/ARK-4](https://github.com/PSP-Archive/ARK-4/releases).
2. Follow the official ARK-4 instructions for your exact PSP model and firmware.
3. Prefer reversible install paths unless you explicitly understand flash-write risks on your device.
4. Verify CFW is active from the XMB system information screen.

---

## Step 2: Install PSP Moonlight

### Download

Download both files from the [latest release](https://github.com/k4idyn/Moonlight-PSP/releases/latest):

- `EBOOT.PBP` - the main application
- `moonlight_me_helper.prx` - Media Engine helper module

### Copy to Memory Stick

1. Connect your PSP by USB, or insert the Memory Stick into a card reader.
2. On the Memory Stick, open `PSP/GAME/`.
3. Create a folder named `Moonlight`.
4. Copy both release files into it:

```text
ms0:/
`-- PSP/
    `-- GAME/
        `-- Moonlight/
            |-- EBOOT.PBP
            `-- moonlight_me_helper.prx
```

5. Safely eject the Memory Stick or disconnect USB.

Both files must be in the same folder. The app loads `moonlight_me_helper.prx` at startup to initialize Media Engine conversion.

Runtime settings, pairing identity, logs, and icon cache are stored separately under `ms0:/PSP/SAVEDATA/Moonlight/`.

### Updating From v1.2 or Older

For a clean upgrade, remove stale old-build Moonlight clutter before copying the new files. Do not delete the Memory Stick root itself; only remove old Moonlight files and folders if they exist.

1. Delete old root-level Moonlight folders/files such as:
   - `ms0:/moonlight/`
   - `ms0:/moonlight.log`
   - `ms0:/moonlight_debug.log`
   - `ms0:/diag.log`
   - `ms0:/net.log`
   - `ms0:/applist_dump.xml`
   - `ms0:/raw_dump.h264`
   - `ms0:/idr_dump.h264`
2. Delete the old install folder: `ms0:/PSP/GAME/Moonlight/`.
3. Recreate `ms0:/PSP/GAME/Moonlight/` and copy the v1.4 `EBOOT.PBP` and `moonlight_me_helper.prx` into it.
4. Pair again in Sunshine. v1.4 stores the client identity under `ms0:/PSP/SAVEDATA/Moonlight/`.

---

## Step 3: Configure Sunshine

PSP Moonlight streams from Sunshine. Configure the host for low-latency H.264 output that the PSP can decode reliably.

### Recommended Host Settings

| Setting | Value | Why |
|---|---|---|
| Video Codec | H.264 | Only codec supported by this PSP client |
| Encoder Profile | Baseline | Lowest PSP decode cost |
| Entropy Coding | CAVLC / CABAC | CAVLC is recommended for lower CPU overhead, but CABAC is fully supported and optimized in v1.4.0 (ideal for AMD hosts) |
| Rate Control | Low latency / bandwidth limited | Reduces burst pressure on PSP Wi-Fi |
| FEC | 35 percent starting point | Adds recovery data for lossy 802.11b links |

### Recommended PSP Presets

| PSP Preset | Stream | Bitrate | Packet Size | Audio |
|---|---|---:|---:|---|
| Performance | 300x170 @ 30 fps | 384 kbps | 1056 bytes | Disabled |
| Balanced | 360x204 @ 20 fps | 480 kbps | 1200 bytes | Enabled |
| Quality | 480x272 @ 10 fps | 576 kbps | 1200 bytes | Enabled |

Start with Performance on PSP-1000 or weak Wi-Fi. Use Balanced when input, audio, and playback stay stable. Use Quality when you specifically want native 480x272 output.

### Choosing Entropy Coding (CAVLC vs CABAC)

In v1.4.0, client-side CABAC decoding has been fully optimized. However, CAVLC remains recommended if configurable, as it consumes slightly less PSP CPU power. For AMD host encoders (which often force CABAC and ignore CAVLC requests), CABAC can be used directly without performance penalties.

To configure CAVLC in Sunshine's web UI:

1. Open Sunshine in a browser, usually `https://localhost:47990` or `https://<your-PC-IP>:47990`.
2. Open Configuration.
3. Go to the encoder section for your active backend.
4. Set the H.264 entropy/coder option to CAVLC.
5. Save and restart Sunshine if Sunshine asks you to restart.

If editing `sunshine.conf` directly, use the key that matches your encoder:

```ini
# AMD AMF
amd_coder = cavlc

# NVIDIA NVENC
nvenc_h264_cavlc = enabled

# Intel QSV
qsv_coder = cavlc
```

### Host Encoder Notes

Hardware encoders such as NVENC, AMF, and QSV are fine. The PSP cares about the output bitstream, not which host encoder produced it. Keep H.264 Baseline + CAVLC and avoid settings that create large bitrate spikes.

---

## Step 4: First Launch

1. On the PSP, open Game -> Memory Stick.
2. Launch PSP Moonlight.
3. Choose your PSP preset in Settings. Performance is the safest starting point.
4. Continue to Wi-Fi setup. If the PSP is already connected, the network dialog is skipped.
5. Select your Sunshine host, or add the IP manually if discovery misses it.
6. If the host is unpaired, the PSP shows a 4-digit PIN.
7. Enter the PIN in Sunshine's Add Device / PIN page.
8. Once pairing completes, the game library loads with available applications.
9. Select a game or Desktop to start streaming.

---

## Troubleshooting

### No hosts found

- Verify the PSP and PC are on the same LAN subnet.
- Verify Sunshine is running and port `47989` is reachable.
- Try Triangle from the host list to enter the host IP manually.
- Try Square to rescan.

### Pairing fails or times out

- Enter the PIN within the time shown by Sunshine.
- Verify Sunshine's HTTPS port `47984` is not blocked by the firewall.
- Delete any stale PSP device entry in Sunshine and try pairing again.
- If the PSP is already shown as paired but the app list fails, unpair and pair again.

### Video does not start or stays black

- Verify `moonlight_me_helper.prx` is in the same folder as `EBOOT.PBP`.
- Set Sunshine to H.264 Baseline + CAVLC.
- Start with the Performance preset.
- Move closer to the access point or reduce bitrate if the HUD shows packet loss.

### Low FPS or stuttering

- Use Performance first: 300x170 @ 30 fps, 384 kbps, 1056-byte packets.
- Try lowering bitrate one step if Wi-Fi loss is high.
- Avoid congested 2.4 GHz channels.
- Disable host settings that allow large VBV or bitrate bursts.

### No audio in Performance

This is expected. Performance disables local PSP audio decode and playback to spend less PSP work on audio. Switch Audio to Enabled in settings, or use Balanced/Quality, if you need sound.

### Application crashes on launch

- Confirm custom firmware is active.
- Confirm both release files are copied into the same XMB folder.
- Try a clean install folder if old config files were copied from a much older build.

---

## Uninstalling

Delete `ms0:/PSP/GAME/Moonlight/`.

Moonlight stores runtime data here:

- `ms0:/PSP/SAVEDATA/Moonlight/config.ini` - settings
- `ms0:/PSP/SAVEDATA/Moonlight/map.cfg` - button mapper settings
- `ms0:/PSP/SAVEDATA/Moonlight/cache/` - icon cache
- `ms0:/PSP/SAVEDATA/Moonlight/client.crt` - pairing client certificate
- `ms0:/PSP/SAVEDATA/Moonlight/client.key` - pairing private key
- `ms0:/PSP/SAVEDATA/Moonlight/client.uid` - stable client ID
- `ms0:/PSP/SAVEDATA/Moonlight/tls_pins/` - HTTPS host certificate pins
- `ms0:/PSP/SAVEDATA/Moonlight/moonlight.log` - diagnostics log in debug builds

Delete `ms0:/PSP/SAVEDATA/Moonlight/` too for a clean removal.
