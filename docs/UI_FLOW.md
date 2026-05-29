# UI Flow Reference

This document describes the implemented UI flow of PSP Moonlight v1.3.0.

All screen coordinates are in native PSP resolution: 480x272.

---

## Screen Flow Overview

```text
Startup
  -> Settings Menu (settings_menu.c)
      -> Wi-Fi Setup (netconf_ui.c, skipped if already connected)
          -> Host Discovery (host_discovery.c)
              -> Pairing Screen when needed
              -> Game Library (game_grid_ui.cpp)
                  -> Connecting Screen (stream_connect_ui.c)
                      -> Active Stream
                          -> Game Library or Host Discovery on quit
```

---

## Screen: Wi-Fi Setup

**Source file:** `src/netconf_ui.c`

On startup, the app first shows the Settings Menu. After exiting settings, it checks `sceNetApctlGetState()`. If Wi-Fi is already connected, the native network dialog is skipped. Otherwise, the app invokes the PSP firmware network configuration dialog.

The app polls `sceNetApctlGetState()` until an IP address is acquired. If the user cancels or the connection times out, a non-fatal error is displayed and the app exits cleanly.

---

## Screen: Host Discovery

**Source file:** `src/host_discovery.c`

Displays a smooth-scrolling list of known hosts. Each entry is a rounded card showing hostname, IP, online/offline status, and paired status when available.

The list is populated by:

1. Manual hosts loaded from `config.ini`.
2. mDNS multicast discovery for `_nvstream._tcp.local.`.
3. HTTP probes on port `47989` against known IPs.

| Button | Action |
|---|---|
| Cross | Select host |
| Circle | Back to Settings Menu |
| Square | Rescan LAN |
| Triangle | Add host manually by IP |
| Select | Send Wake-on-LAN when a MAC address is saved |

The host context menu can wake, remove, or unpair a saved host.

---

## Screen: Pairing

**Source file:** `src/network_connect.c`, `src/pairing_pin_ui.cpp`

Displays a 4-digit PIN centered on screen. Enter the PIN in Sunshine's Add Device / PIN page.

The pairing flow uses the Moonlight challenge-response protocol:

1. Send salt and client certificate, then receive the server certificate.
2. Send AES-encrypted client challenge.
3. Send server challenge response.
4. Send the signed client pairing secret.
5. Confirm pairing over authenticated HTTPS using the new client certificate.

On success, the host becomes paired and the app proceeds to the game library. Failed attempts are cleaned up so the next pairing attempt starts from a clean state.

---

## Screen: Game Library

**Source file:** `src/game_grid_ui.cpp`, `src/icon_cache.c`, `src/game_list_parser.c`

Displays a scrollable grid of games fetched from the host `/applist` endpoint. Each tile shows box art when available, with raw RGB565 cache files stored on the Memory Stick.

| Button | Action |
|---|---|
| D-pad | Navigate grid |
| Cross | Launch selected game |
| Start | Launch selected game |
| Circle | Back to Host Discovery |
| Square | Refresh game library |

The game-library loading and launch boundary primes both display buffers to avoid flipping back to stale host-list or loading frames during slow network work.

---

## Screen: Settings Menu

**Source file:** `src/settings_menu.c`

A vertical list of configurable settings. D-pad Up/Down navigates rows. D-pad Left/Right changes the selected value. Cross edits rows that open a sub-screen or OSK.

| Setting | Options | Default |
|---|---|---|
| Preset | Performance, Balanced, Quality, Custom | Performance |
| Resolution | 300x170, 360x204, 480x272, Custom | 300x170 |
| FPS | 10, 15, 20, 30, 60, Custom | 30 |
| Audio | Enabled, Disabled | Disabled in Performance |
| Control Mode | Xbox, Browser | Xbox |
| Button Map | Opens mapper | Default v2 map |
| Theme | 10 built-in themes | Ocean Depths |
| Bitrate | 192-2560 kbps presets | 384 kbps |
| Packet Size | 512-1392 byte presets | 1056 bytes |

### Presets

| Preset | Stream | Bitrate | Packet Size | Audio |
|---|---|---:|---:|---|
| Performance | 300x170 @ 30 fps | 384 kbps | 1056 | Disabled |
| Balanced | 360x204 @ 20 fps | 480 kbps | 1200 | Enabled |
| Quality | 480x272 @ 10 fps | 576 kbps | 1200 | Enabled |

The Preset row applies the matching resolution, FPS, bitrate, packet size, and audio default. The Resolution row can then be changed independently without reapplying the preset ladder. Built-in sizes use the PSP LCD aspect ratio so the renderer fills 480x272 without fixed black bars.

Audio Disabled is a client-side low-work mode. It skips local Opus decode and playback on the PSP while keeping the session compatible with Sunshine.

Values are persisted to `config.ini` on exit.

---

## Screen: Connecting to Stream

**Source file:** `src/stream_connect_ui.c`

Shown while the stream session is being established. A progress bar animates while the app requests the stream, negotiates RTSP, and starts playback.

If a phase fails, the connecting screen exits with an error modal and returns to the game library.

---

## Screen: Active Stream / HUD Overlay

**Source file:** `src/hud.c`

The active stream fills the 480x272 display with the decoded RGBA frame. The HUD is hidden by default.

The HUD shows stream timing, FPS, loss/FEC, CPU/GPU/ME/RAM telemetry, bandwidth, host, and battery information. Telemetry is updated once per second in diagnostics builds.

When the HUD is not visible, stream input is forwarded to the host. App-owned combos such as HUD toggle and stream exit are consumed locally and are not sent as host input.

---

## Screen: Exit Dialog

**Source file:** `src/exit_dialog.c`

A confirmation modal is shown when exiting from menu screens. During an active stream, the stream HUD handles quit actions.

Selecting Yes shuts down active threads, network sockets, and the ME worker before exiting.

---

## Error Handling

Major errors are logged to `ms0:/PSP/SAVEDATA/Moonlight/moonlight_debug.log`. Fatal stream errors display an error modal, then cleanly return to the host or game screen after acknowledgement.

---

## Button Mapper

**Source file:** `src/button_mapping_ui.cpp`, `src/input.c`, `map.cfg`

The PSP lacks LT/RT triggers and a right analog stick. The default v2 mapping keeps unmodified PSP controls close to an Xbox pad, then uses L as the combo modifier:

| Combo | Virtual Input |
|---|---|
| L + Triangle | Right stick up |
| L + Cross | Right stick down |
| L + Square | Right stick left |
| L + Circle | Right stick right |
| L + D-pad Left | LT |
| L + D-pad Right | RT |
| L + D-pad Down | L3 |
| L + D-pad Up | R3 |

The mapper can remap the modifier and each virtual action. It can also switch right-stick control from mapped buttons to L + analog nub. Old mapping files are migrated when loaded.
