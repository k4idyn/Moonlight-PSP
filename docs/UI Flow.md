# UI Flow Reference

This document describes the implemented UI flow of PSP Moonlight v1.0.0.

All screen coordinates are in native PSP resolution: 480×272.

---

## Screen Flow Overview

```
Startup
  └─▶ Settings Menu (settings_menu.c)
        └─▶ WiFi Setup (netconf_ui.c) [skipped if already connected]
              └─▶ Host Discovery (host_discovery.c)
                    ├─▶ Back (○) → Settings Menu
                    ├─▶ Pairing Screen (pairing.c)
                    │     └─▶ Host Discovery (on success)
                    └─▶ Game Library (game_grid_ui.cpp)
                          ├─▶ Back (○) → Host Discovery (cached, instant)
                          └─▶ Connecting Screen (stream_connect_ui.c)
                                └─▶ Active Stream
                                      └─▶ Host Discovery (on quit, cached)
```

---

## Screen: WiFi Setup

**Source file:** `src/netconf_ui.c`

On startup, the app first shows the Settings Menu. After exiting settings, it checks `sceNetApctlGetState()`. If WiFi is already connected (state 4), the netconf dialog is skipped entirely. Otherwise, the app invokes `sceUtilityNetconfInitStart` to open the PSP's native Network Configuration dialog. This is a firmware-level UI — the app suspends its own render loop while it is active.

The app polls `sceNetApctlGetState()` in a loop. Once state reaches `PSP_NET_APCTL_STATE_IP_ACQUIRED`, the dialog is dismissed and the app transitions to Host Discovery. If the user cancels or the connection times out (60 seconds), a non-fatal error is displayed and the app exits cleanly.

No custom UI is drawn during this screen — it uses the PSP firmware's built-in WiFi selection UI.

**What triggers the next screen:** IP address acquired.

---

## Screen: Host Discovery

**Source file:** `src/host_discovery.c`

Displays a smooth-scrolling list of known hosts. Each entry is a rounded-rectangle card showing hostname + IP on the left and Online/Offline status + Paired/Unpaired label on the right. The selected card grows 2% (focus pop) and gains an extra shadow offset for hover lift. Paired/Unpaired is only shown for online hosts.

The list is populated by:

1. Manual hosts loaded from `config.ini` on startup (with paired status from config)
2. mDNS multicast discovery (`_nvstream._tcp.local.`) — near-instant LAN discovery
3. HTTP probes on port 47989 against known IPs (3s connect + 2s recv timeout per host)

Auto-selection is disabled. The user navigates with D-pad Up/Down.

**Controls:**

| Button | Action |
|--------|--------|
| Cross | Select host (→ Pairing or Game Library depending on paired status) |
| Circle | Back to Settings Menu |
| Square | Rescan LAN (mDNS + HTTP re-probe). Long-hold: full /24 subnet scan |
| Triangle | Add host manually via PSP OSK (numeric/IP input) |
| Select | Send Wake-on-LAN to the selected offline host (if MAC is stored) |

**Manual host entry:** Pressing Triangle opens `sceUtilityOskInitStart` in numeric/IP mode. The entered IP is validated and saved to `config.ini`.

**Wake-on-LAN:** If the selected host has a saved MAC address in `config.ini`, a WOL magic packet is broadcast to the LAN. A brief "WOL Sent" toast is shown. If no MAC is stored, the prompt shows "MAC unknown — WOL unavailable".

**MAC address capture:** On first successful RTSP connection to a host, an ARP request is sent to resolve the host IP → MAC. The result is stored in `config.ini` under the host's section. A toast "MAC saved for WOL" is shown on first capture.

**Host context menu (Square hold):**
- **Wake on LAN** — same as Select button
- **Remove Host** — removes IP and MAC from `config.ini`, refreshes list
- **Unpair** — sends HTTPS GET to `/unpair?uniqueid=[CLIENT_ID]`, deletes local client certificate on success, displays "Unpaired from [hostname]" toast. If host is unreachable, offers "Forget Locally" which only deletes the local cert files.

**What triggers the next screen:** Cross on an unpaired host → Pairing. Cross on a paired host → Game Library.

---

## Screen: Pairing

**Source file:** `src/pairing.c`

Displays a 4-digit PIN centered on screen in large text. Below the PIN: "Enter this PIN in Sunshine → Add Device."

The pairing protocol is a 5-step Moonlight challenge-response flow using mbedTLS:

1. Send salt + client certificate → receive server certificate
2. Send AES-encrypted client challenge → receive challenge response (PIN verified here; wrong PIN returns `paired=0`)
3. Send server challenge response
4. Send client pairing secret (RSA-signed with private key)
5. HTTPS confirm (optional for Sunshine — step 4 is sufficient)

A timer polls the `isPaired` flag. On success, the screen transitions to Host Discovery (host now appears as paired/online). On timeout or failure, an error message is displayed and the user is returned to Host Discovery.

**What triggers the next screen:** Successful pairing → Host Discovery.

---

## Screen: Game Library

**Source file:** `src/game_grid_ui.cpp`, `src/icon_cache.c`, `src/game_list_parser.c`

Displays a scrollable grid of games fetched from the host's `/applist` endpoint (XML, port 47884). Each tile shows a box art icon (144×80, cached to `ms0:/PSP/GAME/Moonlight/cache/`) and the game title below it. Icons are downloaded in the background using `lodepng` for PNG decoding and converted to RGB565 for display.

Grid layout: 3 columns × N rows. `ROWS_VISIBLE` rows are shown at a time. Scroll offset tracks which row is at the top of the visible area.

**Controls:**

| Button | Action |
|--------|--------|
| D-pad | Navigate grid |
| Cross | Launch selected game (→ Connecting screen) |
| Start | Launch selected game |
| Circle | Back to Host Discovery (cached, instant — no re-probe) |
| Square | Refresh game library |

**Scroll indicator:** Bottom-right corner shows "Row N/M".

**Empty state:** "No games found — check Sunshine App List" is displayed centered in the grid area if the game count is zero.

**Icon fallback:** If an icon download fails or returns a non-zero lodepng result, a default fallback icon from `fallback_icons.h` is used.

**Memory budgeting:** Icon cache is sized at startup based on available RAM. PSP-2000/3000 (≥48 MB free): up to 20 icons. PSP-1000 (<32 MB free): up to 8 icons, background prefetch disabled.

**What triggers the next screen:** Cross/Start on a game → Connecting. Triangle → Settings Menu.

---

## Screen: Settings Menu

**Source file:** `src/settings_menu.c`

A vertical list of 5 configurable settings. D-pad Up/Down navigates rows; D-pad Left/Right changes the selected value.

| Setting | Options | Default |
|---------|---------|---------|
| Resolution | 480×272 (PSP Native), 256×144 (Performance), Custom | 480×272 |
| FPS | 15, 20, 30, 60 | 15 |
| Control Mode | Xbox, Browser | Xbox |
| Theme | Ocean Depths, Sunset Blvd, Forest Canopy, Modern Minimal, Golden Hour, Arctic Frost, Desert Rose, Tech Innovate, Botanical, Midnight Gal. | Ocean Depths |
| Bitrate | Codec-aligned presets (64–2560 kbps) | 384 kbps |

**Resolution notes:**
- "480×272 (PSP Native)" streams at the PSP's native display resolution. This is the target for full-detail streaming.
- "256×144 (Performance)" reduces the decode budget significantly and is intended for unstable WiFi or PSP-1000 constraints.
- "Custom" opens the PSP OSK for manual width/height entry.

When resolution changes, FPS and bitrate are snapped to optimal values for that resolution automatically (`RESOLUTION_OPTIMAL_FPS_IDX`, `RESOLUTION_OPTIMAL_BITRATE` arrays in `settings_menu.c`).

**Bitrate** is derived from per-resolution sample data (706 hardware measurements). It is stored in the config struct and passed to the RTSP DESCRIBE negotiation phase.

Values are persisted to `config.ini` on exit.

**What triggers the next screen:** Circle → return to Game Library.

---

## Screen: Connecting to Stream

**Source file:** `src/stream_connect_ui.c`

Shown while the stream session is being established in the background. A progress bar animates smoothly at ~60 fps using a background VSyncThread. The main thread updates the target phase; the render thread interpolates toward it.

**Phases:**

| Phase | Progress | Label |
|-------|----------|-------|
| 0 | 25% | Requesting Stream... |
| 1 | 60% | Negotiating RTSP... |
| 2 | 90% | Starting Playback... |
| 3 | 100% | Stream Ready |

The game title is shown above the progress bar. If any phase fails, the connecting screen exits with an error modal and the user is returned to the Game Library.

**What triggers the next screen:** Phase 3 complete → Active Stream.

---

## Screen: Active Stream / HUD Overlay

**Source file:** `src/hud.c`

The active stream fills the entire 480×272 display with the decoded RGBA frame. The HUD is hidden by default.

**HUD toggle:** Home button or Note button toggles HUD visibility.

When visible, the HUD renders as a semi-transparent alpha-blended overlay in the top-right corner. It shows:
- Latency (ms)
- Frame loss (%)
- Brief timed icons (bottom-right): WiFi signal change icon (2 s), Rewind/Pause icon (2 s, shown on packet-loss safety buffer event)

**HUD menu items (D-pad Up/Down when HUD is visible):**

| Item | Action |
|------|--------|
| Stats | View latency/loss values |
| Pause | (sends pause signal to host) |
| Quit | Cross button → calls `end_stream_session()`, returns to Game Library |

**Input forwarding:** When HUD is not visible, all D-pad and button input is forwarded to the host as Moonlight Type-5 controller packets. Button combo mappings (from `map.cfg`) are applied before building the packet bitmask. Input is rate-limited to once per frame to avoid flooding the control socket.

**Power switch handling:** If the physical power switch is toggled to sleep, a Pause command is sent to the host and the current session token is cached. Wake-up attempts a quick reconnect via `sceNetInetConnect` with a 5-second timeout. Full resume is not yet reliable — see Known Issues.

**What triggers the next screen:** Quit from HUD → Game Library.

---

## Screen: Exit Dialog

**Source file:** `src/exit_dialog.c`

A confirmation modal: "Exit Moonlight?" with Yes/No options. Triggered by the Home button from Host Discovery or Game Library (not during active stream — Home during stream opens the HUD instead).

Selecting Yes cleanly shuts down all active threads, network sockets, and the ME worker before calling `sceKernelExitGame()`.

---

## Error Handling

All major errors are logged to `ms0:/moonlight_debug.log` via `diag_log.c`. If a fatal error occurs during streaming (network watchdog timeout, RTSP connection drop), a semi-transparent error modal is drawn over the current screen with the specific error string. The user presses Circle to acknowledge, which initiates a clean socket teardown and returns to Host Discovery.

---

## Button Mapper

**Source file:** `src/input.c`, `map.cfg`

The PSP lacks L2/R2 triggers and a right analog stick. The button mapper system allows combo mappings: L + D-pad direction or R + face button can trigger virtual L2, R2, or right stick inputs. Mappings are stored in `map.cfg` and loaded at startup. They are applied inside the `sceCtrlReadBufferPositive` loop before building Moonlight controller packets.