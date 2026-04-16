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

#ifdef __cplusplus
extern "C" {
#endif

/*--------------------------------------------------------------------------
 * Configuration File Path
 *--------------------------------------------------------------------------*/
#define CONFIG_FILE_PATH        "ms0:/moonlight/config.ini"
#define CONFIG_DIR_PATH         "ms0:/moonlight"
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
 *  - 500 kbps keeps IDR keyframes under ~15KB (~15 RTP packets),
 *    dramatically reducing burst packet loss vs 1000+ kbps (~48KB/~47 pkts).
 *  - 15 FPS halves bandwidth pressure vs 30 FPS, giving WiFi headroom.
 *  - 480x272 is the sceMpeg AVC hardware decoder maximum.
 *--------------------------------------------------------------------------*/
/*
 * PSP-1000 Mathematical Optimum (706 hardware samples, 14 test runs):
 *
 *   CPU:  Allegrex MIPS32 @ 333MHz, 16KB L1i+L1d, in-order single-issue
 *   WiFi: 802.11b 11Mbps theoretical, ~4.5Mbps sustained UDP
 *   ME:   YUV→RGBA in ~31µs (uncached DMA via 0x40000000)
 *   GE:   Bilinear upscale to 480×272 in ~2ms
 *
 *   Decode cost: 560 µs per 1000 pixels + 3031 µs fixed overhead (Run 101 measured)
 *   Bitrate:     500kbps flat (proven safe on 802.11b)
 *   VSync:       60Hz → valid fps: 15, 20, 30, 60
 *
 *   Quality:     368×208 @15fps @500kbps  → 45.9ms/frame (68.8% of 66.7ms budget)
 *   Balanced:    256×144 @30fps @500kbps  → 23.7ms/frame (71.0% of 33.3ms budget)
 *   Performance: 256×144 @20fps @500kbps  → 23.7ms/frame (47.3% of 50ms budget)
 *
 *   NOTE: 192x112 dropped — Sunshine rejects resolutions below ~256x144.
 *
 *   Bitrate: 500kbps flat (proven stable on 802.11b, IDR < 15KB).
 *   Higher bitrate scales decode cost ~linearly (804kbps → 637 µs/Kpx MEASURED).
 */
#define DEFAULT_WIDTH           368     /* Quality preset — best visual at 15fps */
#define DEFAULT_HEIGHT          208     /* ~16:9 (1.769), mod-16 aligned */
#define DEFAULT_FPS             15      /* 60Hz/4 — highest safe fps for 368x208 */
#define DEFAULT_BITRATE         384     /* 384kbps — WiFi-safe default */
#define MAX_BITRATE             2760    /* ~4.5Mbps WiFi - 25% FEC - 15% safety */
#define DEFAULT_PACKET_SIZE     1024
#define DEFAULT_CONTROL_MODE    CONTROL_MODE_XBOX

/*--------------------------------------------------------------------------
 * Public API
 *--------------------------------------------------------------------------*/

/*
 * loadConfig - Load configuration from config.ini
 *
 * @config: Pointer to PspConfig structure to populate
 *
 * Reads configuration from "ms0:/moonlight/config.ini" file.
 * If the file doesn't exist or is corrupted, initializes with defaults:
 * - 480x272 (PSP native, sceMpeg AVC hardware limit)
 * - 30 FPS
 * - 1500 kbps bitrate
 *
 * Returns: 0 on success, -1 on error (defaults applied)
 */
int loadConfig(PspConfig *config);

/*
 * saveConfig - Save configuration to config.ini
 *
 * @config: Pointer to PspConfig structure to save
 *
 * Writes current configuration to "ms0:/moonlight/config.ini".
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
 * - 480x272 (PSP native + sceMpeg AVC hardware decoder maximum)
 * - 15 FPS (stable on 802.11b)
 * - 500 kbps bitrate (small IDRs for reliable WiFi delivery)
 * - Analog control mode
 */
void configSetDefaults(PspConfig *config);

#ifdef __cplusplus
}
#endif

#endif /* CONFIG_H */