/*
 * config.h - Configuration persistence for PSP Moonlight
 *
 * Saves and loads streaming settings to/from config.ini on Memory Stick.
 * Uses PSP sceIo* functions for file I/O.
 */

#ifndef CONFIG_H
#define CONFIG_H

#include <psptypes.h>
#include "settings_menu.h"
#include "storage_paths.h"

#ifdef __cplusplus
extern "C" {
#endif

/*--------------------------------------------------------------------------
 * Configuration File Path
 *--------------------------------------------------------------------------*/
#define CONFIG_FILE_PATH        MOONLIGHT_SAVE_DIR "/config.ini"
#define CONFIG_DIR_PATH         MOONLIGHT_SAVE_DIR
#define MAX_MANUAL_HOSTS        8

typedef struct {
	char ip[16];
	char mac[18];
} ManualHostEntry;

/*--------------------------------------------------------------------------
 * Default Configuration Values
 *
 * PSP-1000 WiFi: Marvell 88W8686, 802.11b ONLY (11 Mbps theoretical,
 * ~2-3 Mbps real-world with high packet loss on bursts).
 *
 * Low defaults are CRITICAL for stability:
 *  - 300x170 is the lowest even PSP-panel-aspect stream above the host floor.
 *  - 30 FPS is the current practical Performance anchor after the
 *    29/30/31/32/33/34/35/36/37/40 fps sweep.
 *  - 384 kbps / 1056-byte packets is the current Performance anchor:
 *    adjacent bitrate and packet brackets did not improve PSP survival.
 *--------------------------------------------------------------------------*/
/*
 * PSP-1000 current hardware evidence:
 *
 *   CPU:  Allegrex MIPS32 @ 333MHz, 16KB L1i+L1d, in-order single-issue
 *   WiFi: 802.11b 11Mbps theoretical; burst loss is the practical limit
 *   ME:   YUV→RGBA in ~31µs (uncached DMA via 0x40000000)
 *   GE:   Bilinear upscale to 480×272 in ~2ms
 *
 *   300x170@30fps = 1,530,000 pixels/sec. The latest 2026-05-17
 *   Performance sweep has not promoted any preset to production readiness:
 *   - 30fps/384kbps/p1056/FEC35/min1/audio60 is the best current
 *     exact-aspect Performance packet bracket.
 *   - 31fps and 35fps remain high-side evidence points, not stable defaults.
 *   - 360x204@20fps/480kbps is the current Balanced exact-aspect candidate.
 *   - 480x272@10fps/576kbps is the required native Quality bracket anchor.
 *
 *   Defaults use the practical Performance anchor because it is the best
 *   repeatable low-work setting found so far on PSP-1000 hardware. Any future
 *   changes should be based on real PSP playback, input, audio, HUD, and log
 *   evidence rather than desktop-only testing.
 *   VSync:       60Hz -> valid fps: 10, 15, 20, 30, 60
 *
 *   Performance: 300x170 @30fps @384kbps p1056, audio and AV encryption disabled.
 *   Balanced:    360x204 @20fps @480kbps p1200, audio enabled, still stress-only.
 *   Quality:     480x272 @10fps @576kbps p1200, audio enabled, native requirement.
 *
 *   Audio Disabled is client-side low work: keep audio RTSP keepalive,
 *   drain/drop audio RTP, and skip Opus/SRC/playback. It is not zero-RTP
 *   without explicit server support.
 *
 *   NOTE: non-30:17 sizes are not presets because they can introduce bars,
 *   aspect distortion, or host-side padding. Sunshine also rejects very small
 *   streams, so Performance starts at 300x170 instead of 240x136.
 *
 *   Higher bitrate and packet sizes are not promoted from comments; they must
 *   prove better end-to-end behavior on real PSP hardware.
 */
#define DEFAULT_WIDTH           300
#define DEFAULT_HEIGHT          170
#define DEFAULT_FPS             30
#define MIN_BITRATE             192
#define DEFAULT_BITRATE         384
#define MAX_BITRATE             2760
#define MIN_STREAM_PACKET_SIZE  512
#define MAX_STREAM_PACKET_SIZE  1392
#define DEFAULT_PACKET_SIZE     1056
#define DEFAULT_CONTROL_MODE    CONTROL_MODE_XBOX

/*--------------------------------------------------------------------------
 * Public API
 *--------------------------------------------------------------------------*/

/*
 * loadConfig - Load configuration from config.ini
 *
 * @config: Pointer to PspConfig structure to populate
 *
 * Reads configuration from "ms0:/PSP/SAVEDATA/Moonlight/config.ini" file.
 * If the file doesn't exist or is corrupted, initializes with defaults:
 * - 300x170
 * - 30 FPS
 * - 384 kbps bitrate
 * - 1056 byte packet size
 *
 * Returns: 0 on success, -1 on error (defaults applied)
 */
int loadConfig(PspConfig *config);

/*
 * saveConfig - Save configuration to config.ini
 *
 * @config: Pointer to PspConfig structure to save
 *
 * Writes current configuration to "ms0:/PSP/SAVEDATA/Moonlight/config.ini".
 * Creates the directory and file if they don't exist.
 *
 * Returns: 0 on success, -1 on error
 */
int saveConfig(const PspConfig *config);

/*
 * config_get_manual_host_count - Return the number of saved manual hosts.
 */
int config_get_manual_host_count(void);

/*
 * config_get_manual_host - Copy one saved manual host entry.
 *
 * Returns 0 on success, -1 if index is out of range or out_entry is NULL.
 */
int config_get_manual_host(int index, ManualHostEntry *out_entry);

/*
 * config_add_manual_host - Add or update a manual host entry and persist it.
 *
 * If the IP already exists, only the MAC is updated when provided.
 * Returns 0 on success, -1 on failure.
 */
int config_add_manual_host(const char *ip, const char *mac);

/*
 * config_delete_manual_host - Remove a manual host entry by IP and persist.
 *
 * Returns 0 on success, -1 if the IP was not found.
 */
int config_delete_manual_host(const char *ip);

/*
 * config_is_host_paired - Check if a host IP is in the paired list.
 *
 * Returns 1 if paired, 0 if not.
 */
int config_is_host_paired(const PspConfig *config, const char *ip);

/*
 * config_add_paired_host - Add a host IP to the paired list (no duplicates).
 *
 * Most recently paired host is moved to slot 0.  If the list is full,
 * the oldest entry is evicted.  Calls saveConfig() to persist.
 *
 * Returns 0 on success, -1 on failure.
 */
int config_add_paired_host(PspConfig *config, const char *ip);

/*
 * configSetDefaults - Initialize config with default values
 *
 * @config: Pointer to PspConfig structure to initialize
 *
 * Sets defaults:
 * - 300x170
 * - 30 FPS
 * - 384 kbps bitrate
 * - 1056 byte packet size
 * - Xbox control mode
 */
void configSetDefaults(PspConfig *config);

#ifdef __cplusplus
}
#endif

#endif /* CONFIG_H */
