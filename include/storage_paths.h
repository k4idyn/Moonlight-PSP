/*
 * storage_paths.h - Central writable Memory Stick paths for PSP Moonlight
 *
 * Runtime data lives under SAVEDATA so app settings, logs, caches, and pairing
 * material do not clutter ms0:/ or the PSP/GAME homebrew install directory.
 */

#ifndef STORAGE_PATHS_H
#define STORAGE_PATHS_H

#include <pspiofilemgr.h>

#define MOONLIGHT_SAVE_DIR                 "ms0:/PSP/SAVEDATA/Moonlight"
#define MOONLIGHT_SAVE_CACHE_DIR           MOONLIGHT_SAVE_DIR "/cache"
#define MOONLIGHT_SAVE_TLS_PIN_DIR         MOONLIGHT_SAVE_DIR "/tls_pins"
#define MOONLIGHT_SAVE_LOG_PATH            MOONLIGHT_SAVE_DIR "/moonlight.log"
#define MOONLIGHT_SAVE_DEBUG_LOG_PATH      MOONLIGHT_SAVE_DIR "/moonlight_debug.log"
#define MOONLIGHT_SAVE_APPLIST_DUMP_PATH   MOONLIGHT_SAVE_DIR "/applist_dump.xml"
#define MOONLIGHT_SAVE_RAW_DUMP_PATH       MOONLIGHT_SAVE_DIR "/raw_dump.h264"
#define MOONLIGHT_SAVE_IDR_DUMP_PATH       MOONLIGHT_SAVE_DIR "/idr_dump.h264"
#define MOONLIGHT_SAVE_SAFETY_TEMP_PATH    MOONLIGHT_SAVE_DIR "/__temp_stream"

static inline void moonlight_storage_ensure_data_dir(void)
{
    sceIoMkdir("ms0:/PSP", 0777);
    sceIoMkdir("ms0:/PSP/SAVEDATA", 0777);
    sceIoMkdir(MOONLIGHT_SAVE_DIR, 0777);
}

static inline void moonlight_storage_ensure_cache_dir(void)
{
    moonlight_storage_ensure_data_dir();
    sceIoMkdir(MOONLIGHT_SAVE_CACHE_DIR, 0777);
}

static inline void moonlight_storage_ensure_tls_pin_dir(void)
{
    moonlight_storage_ensure_data_dir();
    sceIoMkdir(MOONLIGHT_SAVE_TLS_PIN_DIR, 0777);
}

#endif /* STORAGE_PATHS_H */
