/*
 * icon_cache.h - Box art icon cache invalidation and management
 *
 * Stores icon metadata (source URL hash, fetch timestamp) alongside
 * cached RGB565 icon files on ms0:/PSP/SAVEDATA/Moonlight/cache/.
 * Validates freshness on each /applist fetch and re-downloads stale art.
 */

#ifndef ICON_CACHE_H
#define ICON_CACHE_H

#include "storage_paths.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Maximum age before forced refresh (days) */
#define ICON_CACHE_MAX_AGE_DAYS     7

/* Cache index file */
#define ICON_CACHE_INDEX_PATH       MOONLIGHT_SAVE_CACHE_DIR "/cache_index.ini"

/*
 * icon_cache_needs_update - Check if a cached icon is stale.
 *
 * @app_id:      Game/app ID
 * @box_art_url: Current BoxArtURL from the server
 *
 * Returns 1 if the icon should be re-downloaded, 0 if cache is valid.
 */
int icon_cache_needs_update(int app_id, const char *box_art_url);

/*
 * icon_cache_record - Record a successful icon download in the cache index.
 *
 * @app_id:      Game/app ID
 * @box_art_url: URL that was downloaded
 */
void icon_cache_record(int app_id, const char *box_art_url);

/*
 * icon_cache_clear_all - Delete all cached icons and the index file.
 *
 * Returns 0 on success, -1 on I/O error.
 */
int icon_cache_clear_all(void);

#ifdef __cplusplus
}
#endif

#endif /* ICON_CACHE_H */
