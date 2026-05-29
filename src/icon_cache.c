/*
 * icon_cache.c - Box art icon cache invalidation and storage policy
 *
 * Maintains a small INI-like index file alongside cached icons.
 * Each entry: app_id, url_hash (CRC32 of BoxArtURL), fetch_timestamp.
 * On a fresh /applist, compares url_hash to detect changed art.
 */

#include "icon_cache.h"
#include "game_list_parser.h"

#include <pspkernel.h>
#include <pspiofilemgr.h>
#include <psprtc.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/*--------------------------------------------------------------------------
 * Simple CRC32 (for URL content key)
 *--------------------------------------------------------------------------*/
static unsigned int crc32_compute(const char *data, int len)
{
    unsigned int crc = 0xFFFFFFFF;
    int i, j;
    for (i = 0; i < len; i++) {
        crc ^= (unsigned char)data[i];
        for (j = 0; j < 8; j++) {
            if (crc & 1)
                crc = (crc >> 1) ^ 0xEDB88320;
            else
                crc >>= 1;
        }
    }
    return crc ^ 0xFFFFFFFF;
}

/*--------------------------------------------------------------------------
 * Cache index entry (in-memory)
 *--------------------------------------------------------------------------*/
#define MAX_CACHE_ENTRIES   MAX_GAMES

typedef struct {
    int          app_id;
    unsigned int url_hash;
    unsigned int fetch_day;   /* days since epoch for age check */
} CacheEntry;

static CacheEntry s_entries[MAX_CACHE_ENTRIES];
static int        s_count = 0;
static int        s_loaded = 0;

/*--------------------------------------------------------------------------
 * Helper: current day count (approximate, for age comparison)
 *--------------------------------------------------------------------------*/
static unsigned int get_current_day(void)
{
    u64 tick;
    sceRtcGetCurrentTick(&tick);
    /* PSP ticks are in microseconds; rough day = tick / (1000000*86400) */
    return (unsigned int)(tick / 86400000000ULL);
}

/*--------------------------------------------------------------------------
 * Load index from disk
 *--------------------------------------------------------------------------*/
static void load_index(void)
{
    char buf[4096];
    SceUID fd;
    int n;

    if (s_loaded) return;
    s_loaded = 1;
    s_count = 0;

    fd = sceIoOpen(ICON_CACHE_INDEX_PATH, PSP_O_RDONLY, 0);
    if (fd < 0) return;

    n = sceIoRead(fd, buf, sizeof(buf) - 1);
    sceIoClose(fd);
    if (n <= 0) return;
    buf[n] = '\0';

    /* Parse lines: "app_id,url_hash,fetch_day\n" */
    char *line = buf;
    while (*line && s_count < MAX_CACHE_ENTRIES) {
        int id;
        unsigned int hash, day;
        if (sscanf(line, "%d,%u,%u", &id, &hash, &day) == 3) {
            s_entries[s_count].app_id   = id;
            s_entries[s_count].url_hash = hash;
            s_entries[s_count].fetch_day = day;
            s_count++;
        }
        /* Advance to next line */
        char *nl = strchr(line, '\n');
        if (!nl) break;
        line = nl + 1;
    }
}

/*--------------------------------------------------------------------------
 * Save index to disk
 *--------------------------------------------------------------------------*/
static void save_index(void)
{
    char buf[4096];
    int  pos = 0;
    int  i;
    SceUID fd;

    for (i = 0; i < s_count && pos < (int)sizeof(buf) - 40; i++) {
        pos += snprintf(buf + pos, sizeof(buf) - pos, "%d,%u,%u\n",
                        s_entries[i].app_id,
                        s_entries[i].url_hash,
                        s_entries[i].fetch_day);
    }

    moonlight_storage_ensure_cache_dir();

    fd = sceIoOpen(ICON_CACHE_INDEX_PATH,
                   PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd >= 0) {
        sceIoWrite(fd, buf, pos);
        sceIoClose(fd);
    }
}

/*--------------------------------------------------------------------------
 * Find entry by app_id
 *--------------------------------------------------------------------------*/
static int find_entry(int app_id)
{
    int i;
    for (i = 0; i < s_count; i++) {
        if (s_entries[i].app_id == app_id) return i;
    }
    return -1;
}

/*--------------------------------------------------------------------------
 * Public API
 *--------------------------------------------------------------------------*/

int icon_cache_needs_update(int app_id, const char *box_art_url)
{
    unsigned int url_hash;
    unsigned int now_day;
    int idx;

    load_index();

    if (!box_art_url || !box_art_url[0]) {
        return 0;  /* No URL — nothing to update */
    }

    url_hash = crc32_compute(box_art_url, (int)strlen(box_art_url));
    now_day  = get_current_day();

    idx = find_entry(app_id);
    if (idx < 0) {
        return 1;  /* Not in cache — need download */
    }

    /* Check URL hash mismatch (art changed on server) */
    if (s_entries[idx].url_hash != url_hash) {
        return 1;
    }

    /* Check max age */
    if (now_day > s_entries[idx].fetch_day + ICON_CACHE_MAX_AGE_DAYS) {
        return 1;
    }

    return 0;  /* Cache is valid */
}

void icon_cache_record(int app_id, const char *box_art_url)
{
    unsigned int url_hash;
    unsigned int now_day;
    int idx;

    load_index();

    url_hash = box_art_url ? crc32_compute(box_art_url, (int)strlen(box_art_url)) : 0;
    now_day  = get_current_day();

    idx = find_entry(app_id);
    if (idx >= 0) {
        s_entries[idx].url_hash  = url_hash;
        s_entries[idx].fetch_day = now_day;
    } else if (s_count < MAX_CACHE_ENTRIES) {
        s_entries[s_count].app_id    = app_id;
        s_entries[s_count].url_hash  = url_hash;
        s_entries[s_count].fetch_day = now_day;
        s_count++;
    }

    save_index();
}

int icon_cache_clear_all(void)
{
    char path[128];
    int i;

    load_index();

    /* Delete cached icon files */
    for (i = 0; i < s_count; i++) {
        snprintf(path, sizeof(path),
                 MOONLIGHT_SAVE_CACHE_DIR "/%d.raw", s_entries[i].app_id);
        sceIoRemove(path);
    }

    /* Delete index file */
    sceIoRemove(ICON_CACHE_INDEX_PATH);

    s_count = 0;
    return 0;
}
