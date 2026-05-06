# Installation Guide — PSP Moonlight v1.1.0

## Prerequisites

- **PSP** with ARK-4 custom firmware installed
- **Memory Stick** (or internal storage on PSP Go)
- **Wi-Fi network** (2.4 GHz — the PSP does not support 5 GHz)
- **Host PC** running [Sunshine](https://github.com/LizardByte/Sunshine) v0.20+

---

## Step 1: Install ARK-4 Custom Firmware (if not already installed)

If your PSP already has custom firmware, skip to Step 2.

ARK-4 is the current supported CFW target for this project.

1. Download the latest ARK-4 release from [github.com/PSP-Archive/ARK-4](https://github.com/PSP-Archive/ARK-4/releases).
2. Follow the official ARK-4 installation instructions for your exact PSP model/firmware.
3. Prefer reversible install paths unless you explicitly understand flash-write risks on your device.
4. Verify CFW is working: you should see the version string on the XMB (e.g., `6.61 ARK-4`).

---

## Step 2: Install PSP Moonlight

### Download

Download both files from the [latest release](https://github.com/k4idyn/Moonlight-PSP/releases/latest):

- `EBOOT.PBP` — the main application
- `moonlight_me_helper.prx` — kernel-mode Media Engine helper module

### Copy to Memory Stick

1. Connect your PSP via USB cable, or insert the Memory Stick into a card reader.
2. On the Memory Stick, navigate to `PSP/GAME/`.
3. Create a folder called `Moonlight`.
4. Copy both files into it:

```
ms0:/
└── PSP/
    └── GAME/
        └── Moonlight/
            ├── EBOOT.PBP
            └── moonlight_me_helper.prx
```

5. Safely eject the Memory Stick / disconnect USB.

> **Both files must be in the same folder.** The application loads `moonlight_me_helper.prx` at startup to initialize the Media Engine coprocessor. Without it, video decode will not work.

---

## Step 3: Configure Sunshine on Your Host PC

PSP Moonlight streams from a Sunshine server. Configure Sunshine for optimal PSP compatibility:

### Recommended Stream Settings

| Setting | Value | Why |
|---|---|---|
| **Video Codec** | H.264 | Only codec the PSP can decode |
| **Encoder Profile** | Baseline | PSP OpenH264 build supports Baseline only |
| **Entropy Coding** | CAVLC | **CAVLC strongly recommended.** CABAC can be unstable on PSP workloads and may cause stalls or heavy jitter. See "Changing Entropy Coding" below. |
| **Resolution** | 480×272 | Native PSP LCD resolution |
| **Frame Rate** | 15 fps | Optimal for PSP-1000 (17.9 fps peak) |
| **Bitrate** | 384 kbps | WiFi-safe default for 802.11b |

### Changing Entropy Coding (CABAC -> CAVLC)

Your streaming server (Sunshine, etc.) may default to **CABAC** entropy coding. The PSP decode pipeline is significantly more stable on **CAVLC**, while CABAC can trigger freezes, stutter, or rubber-banding under real Wi-Fi conditions. **Use CAVLC for best reliability.**

**How to change it in Sunshine's Web UI:**

1. Open your Sunshine Web UI in a browser (usually `https://localhost:47990` or `https://<your-PC-IP>:47990`).
2. Log in with your Sunshine credentials.
3. Go to **Configuration** (the gear icon or "Configuration" tab).
4. Scroll down to the **Encoder** section.
5. Look for the H.264 entropy/coder setting for your active encoder backend.
6. Change it from **CABAC** to **CAVLC**.
7. Click **Save** and restart Sunshine.

**If you prefer editing the config file directly:**

Open `sunshine.conf` (usually in `C:\Users\<you>\Applications\Files\Apollo\config\sunshine.conf` on Windows, or `~/.config/sunshine/sunshine.conf` on Linux) and add or change:

```ini
# AMD AMF
amd_coder = cavlc

# NVIDIA NVENC
nvenc_h264_cavlc = enabled

# Intel QSV
qsv_coder = cavlc
```

> **Why?** CABAC is more decode-intensive. On PSP, CAVLC is the practical reliability-first choice across host GPU vendors.

### UPnP for Hotspot / Remote Sessions

PSP Moonlight can request temporary UPnP IGD UDP mappings for RTP/RTCP ports during session setup.

Use this when streaming over:

- Mobile hotspot networks
- Public/WAN routes
- NAT environments that need explicit inbound UDP mapping

Checklist:

1. Enable UPnP on your router/hotspot (if supported).
2. Ensure Sunshine is running and reachable on the selected route.
3. Start with conservative stream settings (Baseline + CAVLC + moderate bitrate).

If UPnP is unavailable or blocked by network policy, LAN streaming may still work normally, but remote NAT traversal can fail depending on gateway behavior.

### Sunshine Configuration

In Sunshine's web UI (`https://localhost:47990`), or in `sunshine.conf`:

```ini
encoder = software
min_fps_factor = 1
```

> **Note:** Hardware encoders (NVENC, AMF, QSV) should work fine — the PSP only cares about the output bitstream format, not how it was encoded. Just ensure H.264 Baseline + CAVLC output.

---

## Step 4: First Launch

1. On the PSP, go to **Game → Memory Stick** on the XMB.
2. Launch **PSP Moonlight**.
3. The app will automatically connect to your Wi-Fi access point.
4. Moonlight scans for Sunshine hosts on the local network.
5. Select your host from the list.
6. A 4-digit **pairing PIN** is displayed on the PSP screen — enter this PIN in the Sunshine web UI (`https://localhost:47990/pin`).
7. Once paired, the game library loads with box art icons.
8. Select a game to start streaming.

---

## Troubleshooting

### "No hosts found"
- Verify your PSP and PC are on the same LAN subnet.
- Verify Sunshine is running and port **47989** is accessible.
- Try entering the host IP manually via the on-screen keyboard.

### Pairing fails or times out
- The PIN must be entered within ~60 seconds.
- Verify Sunshine's HTTPS port **47984** is not blocked by your firewall.
- Try pairing again — the PSP generates a new PIN each attempt.

### Video doesn't start / black screen
- Verify `moonlight_me_helper.prx` is in the same folder as `EBOOT.PBP`.
- Set Sunshine to **H.264 Baseline, CAVLC** (not Main/High profile). CABAC is technically supported but causes severe stuttering ~75% of the time — always use CAVLC.
- Reduce bitrate to **384 kbps** if on a weak Wi-Fi signal.

### Low FPS or stuttering
- Move the PSP closer to your Wi-Fi router (802.11b has limited range).
- Reduce resolution to **368×208** if you need higher FPS.
- Reduce bitrate to **384 kbps** if packet loss is high (check the HUD stats).

### Application crashes on launch
- Ensure ARK-4 is installed correctly for your firmware/model and currently active.
- The PRX requires kernel-mode plugin access — some older CFW versions restrict this.

---

## Uninstalling

Delete the `ms0:/PSP/GAME/Moonlight/` folder. Moonlight also creates:
- `ms0:/PSP/GAME/Moonlight/config.ini` — your settings
- `ms0:/PSP/GAME/Moonlight/cache/` — icon cache
- `ms0:/PSP/GAME/Moonlight/certs/` — pairing certificates

Delete these too for a clean removal.
