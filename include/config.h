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
 * Balanced is the recommended visual default: 360x204 at 20 fps,
 * 480 kbps, 1200-byte packets, with PSP audio enabled.
 * The tuned preset ladder remains unchanged:
 *   Quality:     480x272 at 15 fps, 576 kbps, p1200, audio on.
 *   Balanced:    360x204 at 20 fps, 480 kbps, p1200, audio on.
 *   Performance: 300x170 at 30 fps, 384 kbps, p1056, audio off.
 * All built-in sizes match the PSP LCD's 30:17 aspect ratio.
 *--------------------------------------------------------------------------*/
#define DEFAULT_PRESET_INDEX    1
#define DEFAULT_FPS_INDEX       2
#define DEFAULT_AUDIO_ENABLED   1
#define DEFAULT_WIDTH           360
#define DEFAULT_HEIGHT          204
#define DEFAULT_FPS             20
#define MIN_BITRATE             192
#define DEFAULT_BITRATE         480
#define MAX_BITRATE             2760
#define MIN_STREAM_PACKET_SIZE  512
#define MAX_STREAM_PACKET_SIZE  1392
#define DEFAULT_PACKET_SIZE     1200
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
 * - Balanced preset, 360x204
 * - 20 FPS
 * - 480 kbps bitrate
 * - 1200 byte packet size
 * - Audio enabled
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
 * - Balanced preset, 360x204
 * - 20 FPS
 * - 480 kbps bitrate
 * - 1200 byte packet size
 * - Audio enabled
 * - Xbox control mode
 */
void configSetDefaults(PspConfig *config);

#ifdef __cplusplus
}
#endif

#endif /* CONFIG_H */
