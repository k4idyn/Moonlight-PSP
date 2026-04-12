/*
 * game_list_parser.c - Game List Parser and Icon Downloader for PSP Moonlight
 *
 * Implements XML parsing for Sunshine/GameStream game list API.
 * Downloads game icons to local cache for fast display.
 */

#include <pspkernel.h>
#include <pspdebug.h>
#include <pspiofilemgr.h>
#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <png.h>

#include "game_list_parser.h"
#include "client_identity.h"
#include "fallback_icons.h"
#include "icon_cache.h"

/* GU owns VRAM during game grid; pspDebugScreenPrintf causes visible
 * corruption (debug overlay draws into GU framebuffer region).
 * Redirect all debug output to the same log file used by network_connect.c */
#include "diag_log.h"
#define gl_log(fmt, ...) diag_log_write("GL", fmt, ##__VA_ARGS__)
#define pspDebugScreenPrintf gl_log

/* Unique ID must match CLIENT_UNIQUE_ID in network_connect.c */
#define PSP_UNIQUE_ID client_identity_get_uid()

/* https_launch_get is in network_connect.c — used for /applist XML */
extern int https_launch_get(const char *host, int port, const char *path,
                             char *response, int response_size);

/* Binary-safe variant for PNG downloads (uses memcpy, returns body byte count) */
extern int https_launch_get_binary(const char *host, int port, const char *path,
                                    char *resp, int resp_size);

/* PNG download buffer — reused across calls to reduce heap fragmentation */
#define PNG_DOWNLOAD_BUF_SIZE  (512 * 1024)   /* 512 KB max box art */

/*--------------------------------------------------------------------------
 * Default "Internal Game" Icon Data (RGB565, 100x150)
 * A simple gray placeholder icon with "Internal Game" text concept
 *--------------------------------------------------------------------------*/

static unsigned short g_default_icon[ICON_PIXEL_COUNT];

/* Flag to track if default icon has been initialized */
static int g_default_icon_initialized = 0;

/**
 * Initialize the default icon with a gray gradient placeholder
 */
static void init_default_icon(void)
{
    if (g_default_icon_initialized)
        return;
    
    for (int y = 0; y < ICON_HEIGHT; y++) {
        for (int x = 0; x < ICON_WIDTH; x++) {
            /* Create a gradient from dark gray to lighter gray */
            int idx = y * ICON_BUFFER_WIDTH + x;
            unsigned char gray = 0x30 + (y * 0x20 / ICON_HEIGHT);
            unsigned short gray16 = (gray >> 3) & 0x1F;
            g_default_icon[idx] = (gray16 << 11) | ((gray16 << 1) << 5) | gray16;
        }
    }
    
    /* Draw a simple border */
    unsigned short border_col = (0x0C << 11) | (0x18 << 5) | 0x0C;
    for (int x = 0; x < ICON_WIDTH; x++) {
        g_default_icon[x] = border_col;  /* Top border */
        g_default_icon[(ICON_HEIGHT - 1) * ICON_BUFFER_WIDTH + x] = border_col;  /* Bottom border */
    }
    for (int y = 0; y < ICON_HEIGHT; y++) {
        g_default_icon[y * ICON_BUFFER_WIDTH] = border_col;  /* Left border */
        g_default_icon[y * ICON_BUFFER_WIDTH + ICON_WIDTH - 1] = border_col;  /* Right border */
    }
    
    g_default_icon_initialized = 1;
}

/*--------------------------------------------------------------------------
 * Simple String Utilities (avoid heavy library dependencies)
 *--------------------------------------------------------------------------*/

/**
 * Skip whitespace characters in a string
 */
static const char* skip_whitespace(const char *str)
{
    while (*str && isspace((unsigned char)*str))
        str++;
    return str;
}

/**
 * Find a substring in a string (case-sensitive)
 */
static const char* find_substring(const char *haystack, const char *needle)
{
    if (!haystack || !needle || !*needle)
        return NULL;
    
    size_t needle_len = strlen(needle);
    while (*haystack) {
        if (strncmp(haystack, needle, needle_len) == 0)
            return haystack;
        haystack++;
    }
    return NULL;
}

/**
 * Extract content between XML tags
 * Returns pointer to content, or NULL if not found
 */
static const char* extract_xml_content(const char *xml, const char *tag,
                                        char *buffer, int buffer_size)
{
    char open_tag[64];
    char close_tag[64];
    
    /* Build open and close tags */
    snprintf(open_tag, sizeof(open_tag), "<%s>", tag);
    snprintf(close_tag, sizeof(close_tag), "</%s>", tag);
    
    /* Find open tag */
    const char *start = find_substring(xml, open_tag);
    if (!start)
        return NULL;
    
    start += strlen(open_tag);
    start = skip_whitespace(start);
    
    /* Find close tag */
    const char *end = find_substring(start, close_tag);
    if (!end)
        return NULL;
    
    /* Copy content to buffer */
    int len = end - start;
    if (len >= buffer_size)
        len = buffer_size - 1;
    
    strncpy(buffer, start, len);
    buffer[len] = '\0';
    
    return buffer;
}

/**
 * Extract attribute value from XML element
 * Returns pointer to value, or NULL if not found
 */
static const char* extract_xml_attribute(const char *element, const char *attr_name,
                                          char *buffer, int buffer_size)
{
    char attr_pattern[64];
    
    /* Build attribute pattern */
    snprintf(attr_pattern, sizeof(attr_pattern), "%s=\"", attr_name);
    
    /* Find attribute */
    const char *start = find_substring(element, attr_pattern);
    if (!start)
        return NULL;
    
    start += strlen(attr_pattern);
    start = skip_whitespace(start);
    
    /* Find closing quote */
    const char *end = strchr(start, '"');
    if (!end)
        return NULL;
    
    /* Copy value to buffer */
    int len = end - start;
    if (len >= buffer_size)
        len = buffer_size - 1;
    
    strncpy(buffer, start, len);
    buffer[len] = '\0';
    
    return buffer;
}

/*--------------------------------------------------------------------------
 * XML Parser Implementation
 *--------------------------------------------------------------------------*/

/**
 * Parse a single game entry from XML
 */
static int parse_game_entry(const char *entry_xml, GameInfo *game)
{
    char buffer[512];
    
    /* Extract ID */
    if (extract_xml_content(entry_xml, "ID", buffer, sizeof(buffer))) {
        game->id = atoi(buffer);
    } else {
        /* Try extracting from attribute */
        if (extract_xml_attribute(entry_xml, "id", buffer, sizeof(buffer))) {
            game->id = atoi(buffer);
        } else {
            pspDebugScreenPrintf("parse_game_entry: no ID found\n");
            return -1;
        }
    }
    
    /* Extract Title — Sunshine uses <AppTitle> */
    if (extract_xml_content(entry_xml, "AppTitle", buffer, sizeof(buffer))) {
        strncpy(game->title, buffer, MAX_TITLE_LENGTH - 1);
        game->title[MAX_TITLE_LENGTH - 1] = '\0';
    } else if (extract_xml_content(entry_xml, "Title", buffer, sizeof(buffer))) {
        strncpy(game->title, buffer, MAX_TITLE_LENGTH - 1);
        game->title[MAX_TITLE_LENGTH - 1] = '\0';
    } else {
        snprintf(game->title, MAX_TITLE_LENGTH, "Game %d", game->id);
    }
    
    /* Extract BoxArtURL */
    if (extract_xml_content(entry_xml, "BoxArtUrl", buffer, sizeof(buffer))) {
        strncpy(game->boxArtUrl, buffer, MAX_URL_LENGTH - 1);
        game->boxArtUrl[MAX_URL_LENGTH - 1] = '\0';
    } else if (extract_xml_content(entry_xml, "BoxArtURL", buffer, sizeof(buffer))) {
        strncpy(game->boxArtUrl, buffer, MAX_URL_LENGTH - 1);
        game->boxArtUrl[MAX_URL_LENGTH - 1] = '\0';
    } else if (extract_xml_content(entry_xml, "boxart", buffer, sizeof(buffer))) {
        strncpy(game->boxArtUrl, buffer, MAX_URL_LENGTH - 1);
        game->boxArtUrl[MAX_URL_LENGTH - 1] = '\0';
    } else {
        game->boxArtUrl[0] = '\0';
    }
    
    /* Initialize icon state */
    game->iconData = NULL;
    game->iconLoaded = 0;
    
    return 0;
}

/*--------------------------------------------------------------------------
 * Public API Implementation
 *--------------------------------------------------------------------------*/

void game_list_init(GameList *gameList, const char *hostIp)
{
    memset(gameList, 0, sizeof(GameList));
    strncpy(gameList->hostIp, hostIp, MAX_HOST_IP_LENGTH - 1);
    gameList->hostIp[MAX_HOST_IP_LENGTH - 1] = '\0';
    gameList->count = 0;
    
    /* Initialize default icon */
    init_default_icon();
}

int game_list_fetch(GameList *gameList)
{
    char url[512];
    char *recv_buf;
    int ret;
    
    pspDebugScreenPrintf("game_list_fetch: fetching from %s\n", gameList->hostIp);
    
    /* Allocate receive buffer */
    recv_buf = (char*)malloc(HTTP_RECV_BUFFER_SIZE);
    if (!recv_buf) {
        pspDebugScreenPrintf("game_list_fetch: failed to allocate buffer\n");
        return -1;
    }
    
    /* Build URL for applist endpoint — HTTPS port 47984, requires mTLS.
     * https_launch_get is defined in network_connect.c and handles client cert. */
    snprintf(url, sizeof(url), "/applist?uniqueid=%s&uuid=%s",
             client_identity_get_uid(), client_identity_get_uuid());
    
    /* Use the mbedTLS HTTPS client (defined in network_connect.c) */
    extern int https_launch_get(const char *host, int port, const char *path,
                                char *response, int response_size);
    ret = https_launch_get(gameList->hostIp, 47984, url, recv_buf, HTTP_RECV_BUFFER_SIZE - 1);
    if (ret < 0) {
        pspDebugScreenPrintf("game_list_fetch: HTTP request failed (%d)\n", ret);
        free(recv_buf);
        return ret;
    }

    /* https_launch_get returns 0 on success; derive actual length from buffer */
    {
        int xml_len = (int)strlen(recv_buf);
        pspDebugScreenPrintf("game_list_fetch: received %d bytes\n", xml_len);

        /* DUMP THE EXACT RAW XML DIRECTLY TO THE LOG SO WE CAN PROVE WHAT APOLLO SENT */
        SceUID dump_fd = sceIoOpen("ms0:/applist_dump.xml", PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
        if (dump_fd >= 0) {
            sceIoWrite(dump_fd, recv_buf, xml_len);
            sceIoClose(dump_fd);
        }

        ret = game_list_parse_xml(gameList, recv_buf, xml_len);
    }
    
    free(recv_buf);
    
    if (ret < 0) {
        pspDebugScreenPrintf("game_list_fetch: XML parsing failed (%d)\n", ret);
        return ret;
    }
    
    pspDebugScreenPrintf("game_list_fetch: found %d games\n", gameList->count);
    return 0;
}

int game_list_parse_xml(GameList *gameList, const char *xmlData, int xmlSize)
{
    const char *pos;
    const char *end;
    char *entry_buf;
    int games_parsed = 0;
    
    pspDebugScreenPrintf("game_list_parse_xml: parsing %d bytes\n", xmlSize);
    
    /* Find the root element - could be <applist>, <games>, or <root> */
    pos = find_substring(xmlData, "<applist>");
    if (!pos)
        pos = find_substring(xmlData, "<games>");
    if (!pos)
        pos = find_substring(xmlData, "<root>");
    if (!pos) {
        /* Try to find individual game entries directly */
        pos = xmlData;
    }
    
    /* Find end of root element */
    end = find_substring(xmlData, "</applist>");
    if (!end)
        end = find_substring(xmlData, "</games>");
    if (!end)
        end = find_substring(xmlData, "</root>");
    if (!end)
        end = xmlData + xmlSize;
    
    /* Parse each game entry */
    while (pos < end && games_parsed < MAX_GAMES) {
        const char *entry_start;
        const char *entry_end;
        
        /* Find next game entry */
        entry_start = find_substring(pos, "<Game>");
        if (!entry_start || entry_start >= end)
            entry_start = find_substring(pos, "<game>");
        if (!entry_start || entry_start >= end)
            entry_start = find_substring(pos, "<App>");
        if (!entry_start || entry_start >= end)
            entry_start = find_substring(pos, "<app>");
        
        if (!entry_start || entry_start >= end)
            break;
        
        /* Find end of this entry */
        int close_tag_len = 0;
        entry_end = find_substring(entry_start, "</Game>");
        if (entry_end) close_tag_len = 7;
        if (!entry_end) {
            entry_end = find_substring(entry_start, "</game>");
            if (entry_end) close_tag_len = 7;
        }
        if (!entry_end) {
            entry_end = find_substring(entry_start, "</App>");
            if (entry_end) close_tag_len = 6;
        }
        if (!entry_end) {
            entry_end = find_substring(entry_start, "</app>");
            if (entry_end) close_tag_len = 6;
        }
        
        if (!entry_end || entry_end >= end)
            break;
        
        /* Extract entry to buffer dynamically to accommodate massive Base64 nodes */
        int entry_len = entry_end - entry_start + close_tag_len;  /* Include closing tag */
        entry_buf = (char *)malloc(entry_len + 1);
        if (!entry_buf) break;
        
        memcpy(entry_buf, entry_start, entry_len);
        entry_buf[entry_len] = '\0';
        
        /* Parse this game entry */
        if (parse_game_entry(entry_buf, &gameList->games[games_parsed]) == 0) {
            pspDebugScreenPrintf("  Game %d: ID=%d, Title=%s\n",
                                 games_parsed,
                                 gameList->games[games_parsed].id,
                                 gameList->games[games_parsed].title);
            games_parsed++;
        }
        
        free(entry_buf);
        
        /* Move past this entry */
        pos = entry_end + close_tag_len;
    }
    
    gameList->count = games_parsed;
    
    pspDebugScreenPrintf("game_list_parse_xml: parsed %d games\n", games_parsed);
    return games_parsed;
}

static unsigned short* allocate_padded_fallback_icon(const unsigned short *tight_pixels)
{
    unsigned short *padded = (unsigned short *)memalign(16, ICON_DATA_SIZE);
    if (!padded) return NULL;
    memset(padded, 0, ICON_DATA_SIZE);
    
    for (int y = 0; y < ICON_HEIGHT; y++) {
        for (int x = 0; x < ICON_WIDTH; x++) {
            padded[y * ICON_BUFFER_WIDTH + x] = tight_pixels[y * ICON_WIDTH + x];
        }
    }
    
    sceKernelDcacheWritebackInvalidateRange(padded, ICON_DATA_SIZE);
    return padded;
}

int game_list_download_icons(GameList *gameList)
{
    int i;
    int success_count = 0;
    
    pspDebugScreenPrintf("game_list_download_icons: downloading %d icons\n",
                         gameList->count);
    
    /* Create cache directory if it doesn't exist */
    sceIoMkdir(CACHE_DIR, 0777);
    
    /* Download icons for each game */
    for (i = 0; i < gameList->count; i++) {
        GameInfo *game = &gameList->games[i];

        /* Check icon cache validity (URL hash + age) */
        int cache_stale = icon_cache_needs_update(game->id, game->boxArtUrl);
        
        /* Try to load from cache first (only if cache is still valid) */
        if (!cache_stale && game_list_load_cached_icon(game)) {
            pspDebugScreenPrintf("  [%d/%d] Loaded from cache: %s\n",
                                 i + 1, gameList->count, game->title);
            success_count++;
            continue;
        }
        
        /* Download icon from server */
        if (game_list_download_icon(game, gameList->hostIp) == 0) {
            pspDebugScreenPrintf("  [%d/%d] Downloaded: %s\n",
                                 i + 1, gameList->count, game->title);
            icon_cache_record(game->id, game->boxArtUrl);
            success_count++;
        } else {
            pspDebugScreenPrintf("  [%d/%d] Failed: %s (using fallback)\n",
                                 i + 1, gameList->count, game->title);
            /* Try per-title fallback icon first, then generic grey */
            {
                const unsigned short *fb = fallback_icon_for_title(game->title);
                if (fb) {
                    game->iconData = allocate_padded_fallback_icon(fb);
                }
                if (!game->iconData) {
                    game->iconData = g_default_icon;
                }
            }
            game->iconLoaded = 1;
        }
        
        /* Yield between downloads */
        sceKernelDelayThread(50000);
    }
    
    pspDebugScreenPrintf("game_list_download_icons: %d/%d successful\n",
                         success_count, gameList->count);
    return 0;
}

/* -------------------------------------------------------------------------
 * PNG memory-read helpers for libpng
 * ------------------------------------------------------------------------- */
typedef struct {
    const unsigned char *data;
    png_size_t           pos;
    png_size_t           size;
} PNGReadState;

static void png_mem_read(png_structp png, png_bytep buf, png_size_t len)
{
    PNGReadState *st = (PNGReadState *)png_get_io_ptr(png);
    if (st->pos + len > st->size) {
        png_error(png, "EOF");
        return;
    }
    memcpy(buf, st->data + st->pos, len);
    st->pos += len;
}

/* -------------------------------------------------------------------------
 * game_list_download_icon
 *
 * 1. Downloads PNG from Sunshine /appasset (mTLS via https_launch_get)
 * 2. Decodes with libpng into a temporary RGBA8888 buffer
 * 3. Nearest-neighbour scales to ICON_WIDTH x ICON_HEIGHT
 * 4. Saves raw ICON_DATA_SIZE bytes to cache
 * ------------------------------------------------------------------------- */
int game_list_download_icon(GameInfo *game, const char *host_ip)
{
    char *png_buf;
    int   png_len;
    png_structp  png  = NULL;
    png_infop    info = NULL;
    png_bytep   *rows = NULL;
    unsigned int *src = NULL;
    int src_w, src_h = 0;

    if (!game->boxArtUrl || game->boxArtUrl[0] == '\0' || !host_ip) {
        /* No BoxArtUrl in server XML — construct the standard Sunshine appasset URL.
         * Sunshine serves dynamic cover art for every app at this endpoint. */
        if (!host_ip || game->id == 0) {
            pspDebugScreenPrintf("[ICON] skip %d: no host or id\n", game->id);
            return -1;
        }
        snprintf(game->boxArtUrl, MAX_URL_LENGTH,
                 "/appasset?uniqueid=%s&appid=%d&AssetType=2&AssetIdx=0",
                 PSP_UNIQUE_ID, game->id);
        pspDebugScreenPrintf("[ICON] constructed URL: %s\n", game->boxArtUrl);
    }

    pspDebugScreenPrintf("[ICON] downloading appid=%d url=%s\n",
                         game->id, game->boxArtUrl);

    /* boxArtUrl is stored as the path portion only, e.g.
     * "/appasset?uniqueid=...&appid=123&AssetType=2&AssetIdx=0" */
    png_buf = (char *)malloc(PNG_DOWNLOAD_BUF_SIZE);
    if (!png_buf) return -1;

    /* Use binary-safe download: returns body byte count, not 0/-1.
     * Must use host_ip (not boxArtUrl) as the TLS host parameter. */
    png_len = https_launch_get_binary(host_ip, 47984,
                                      game->boxArtUrl,
                                      png_buf,
                                      PNG_DOWNLOAD_BUF_SIZE);
    pspDebugScreenPrintf("[ICON] download ret=%d appid=%d\n", png_len, game->id);
    if (png_len <= 0) {
        free(png_buf);
        return -1;
    }

    /* Validate PNG signature */
    if (png_sig_cmp((png_bytep)png_buf, 0, 8) != 0) {
        pspDebugScreenPrintf("[ICON] not a PNG (first bytes: %02x %02x %02x %02x)\n",
                             (unsigned char)png_buf[0], (unsigned char)png_buf[1],
                             (unsigned char)png_buf[2], (unsigned char)png_buf[3]);
        free(png_buf);
        return -1;
    }

    /* Set up libpng */
    png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!png) { free(png_buf); return -1; }

    info = png_create_info_struct(png);
    if (!info) { png_destroy_read_struct(&png, NULL, NULL); free(png_buf); return -1; }

    if (setjmp(png_jmpbuf(png))) {
        /* libpng error */
        if (rows) {
            for (int i = 0; i < src_h; i++) free(rows[i]);
            free(rows);
        }
        free(src);
        png_destroy_read_struct(&png, &info, NULL);
        free(png_buf);
        return -1;
    }

    /* Custom read from memory */
    PNGReadState st = { (const unsigned char *)png_buf, 0, (png_size_t)png_len };
    png_set_read_fn(png, &st, png_mem_read);

    png_read_info(png, info);

    src_w = (int)png_get_image_width(png, info);
    src_h = (int)png_get_image_height(png, info);
    int color_type = png_get_color_type(png, info);
    int bit_depth  = png_get_bit_depth(png, info);

    /* Normalise to 8-bit RGBA */
    if (bit_depth == 16)              png_set_strip_16(png);
    if (color_type == PNG_COLOR_TYPE_PALETTE) png_set_palette_to_rgb(png);
    if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8) png_set_expand_gray_1_2_4_to_8(png);
    if (png_get_valid(png, info, PNG_INFO_tRNS)) png_set_tRNS_to_alpha(png);
    if (color_type == PNG_COLOR_TYPE_RGB  ||
        color_type == PNG_COLOR_TYPE_GRAY ||
        color_type == PNG_COLOR_TYPE_PALETTE) png_set_filler(png, 0xFF, PNG_FILLER_AFTER);
    if (color_type == PNG_COLOR_TYPE_GRAY ||
        color_type == PNG_COLOR_TYPE_GRAY_ALPHA) png_set_gray_to_rgb(png);
    png_read_update_info(png, info);

    /* Allocate rows for the full-size source image */
    rows = (png_bytep *)malloc(sizeof(png_bytep) * src_h);
    if (!rows) { longjmp(png_jmpbuf(png), 1); }
    for (int i = 0; i < src_h; i++) {
        rows[i] = (png_byte *)malloc(png_get_rowbytes(png, info));
        if (!rows[i]) {
            for (int j = 0; j < i; j++) free(rows[j]);
            free(rows); rows = NULL;
            longjmp(png_jmpbuf(png), 1);
        }
    }
    png_read_image(png, rows);
    png_destroy_read_struct(&png, &info, NULL);
    free(png_buf);

    /* Scale decoded RGBA to ICON_WIDTH x ICON_HEIGHT using nearest-neighbour */
    /* MUST be 16-byte aligned for sceGuTexImage to prevent GE lockup on real hardware */
    game->iconData = (unsigned short *)memalign(16, ICON_DATA_SIZE);
    if (!game->iconData) {
        for (int i = 0; i < src_h; i++) free(rows[i]);
        free(rows);
        return -1;
    }
    memset(game->iconData, 0, ICON_DATA_SIZE);

    for (int dy = 0; dy < ICON_HEIGHT; dy++) {
        int sy = dy * src_h / ICON_HEIGHT;
        if (sy >= src_h) sy = src_h - 1;
        const unsigned char *row = rows[sy];
        for (int dx = 0; dx < ICON_WIDTH; dx++) {
            int sx = dx * src_w / ICON_WIDTH;
            if (sx >= src_w) sx = src_w - 1;
            const unsigned char *px = row + sx * 4;
            /* Convert 8-bit RGBA to 16-bit RGB565 */
            unsigned short r5 = (px[0] >> 3) & 0x1F;
            unsigned short g6 = (px[1] >> 2) & 0x3F;
            unsigned short b5 = (px[2] >> 3) & 0x1F;
            game->iconData[dy * ICON_BUFFER_WIDTH + dx] = (b5 << 11) | (g6 << 5) | r5;
        }
    }

    for (int i = 0; i < src_h; i++) free(rows[i]);
    free(rows);

    /* Flush D-cache after generating texture data */
    sceKernelDcacheWritebackInvalidateRange(game->iconData, ICON_DATA_SIZE);

    game->iconLoaded = 1;
    game_list_save_icon_to_cache(game);
    return 0;
}

int game_list_load_cached_icon(GameInfo *game)
{
    char path[256];
    SceUID fd;
    int ret;
    
    game_list_get_icon_path(game, path);
    
    /* Try to open the cache file */
    fd = sceIoOpen(path, PSP_O_RDONLY, 0777);
    if (fd < 0) {
        return 0;  /* Not in cache */
    }
    
    /* Allocate icon data */
    /* Texture buffer MUST be 16-byte aligned for the Graphics Engine */
    game->iconData = (unsigned short *)memalign(16, ICON_DATA_SIZE);
    if (!game->iconData) {
        sceIoClose(fd);
        return 0;
    }
    memset(game->iconData, 0, ICON_DATA_SIZE);
    
    /* Read icon data */
    ret = sceIoRead(fd, game->iconData, ICON_DATA_SIZE);
    sceIoClose(fd);
    
    if (ret != ICON_DATA_SIZE) {
        free(game->iconData);
        game->iconData = NULL;
        return 0;
    }
    
    /* Ensure D-cache coherence: sceIoRead may DMA into the buffer,
     * leaving stale data in the CPU data cache.  Writeback+invalidate
     * so subsequent CPU reads (and GU DMA for textures) see correct data. */
    sceKernelDcacheWritebackInvalidateRange(game->iconData, ICON_DATA_SIZE);

    game->iconLoaded = 1;
    return 1;
}

int game_list_save_icon_to_cache(const GameInfo *game)
{
    char path[256];
    SceUID fd;
    int ret;
    
    if (!game->iconData || !game->iconLoaded)
        return -1;
    
    game_list_get_icon_path(game, path);
    
    /* Create cache directory if needed */
    sceIoMkdir(CACHE_DIR, 0777);
    
    /* Open file for writing */
    fd = sceIoOpen(path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd < 0) {
        pspDebugScreenPrintf("game_list_save_icon_to_cache: failed to open %s\n", path);
        return -1;
    }
    
    /* Write icon data */
    ret = sceIoWrite(fd, game->iconData, ICON_DATA_SIZE);
    sceIoClose(fd);
    
    if (ret != ICON_DATA_SIZE) {
        pspDebugScreenPrintf("game_list_save_icon_to_cache: write failed (%d)\n", ret);
        return -1;
    }
    
    return 0;
}

void game_list_get_icon_path(const GameInfo *game, char *pathBuf)
{
    snprintf(pathBuf, 256, "%s%d.raw", CACHE_DIR, game->id);
}

void game_list_cleanup(GameList *gameList)
{
    int i;
    
    for (i = 0; i < gameList->count; i++) {
        if (gameList->games[i].iconData && 
            gameList->games[i].iconData != g_default_icon) {
            free(gameList->games[i].iconData);
        }
        gameList->games[i].iconData = NULL;
        gameList->games[i].iconLoaded = 0;
    }
    
    gameList->count = 0;
}

unsigned short* game_list_get_default_icon(void)
{
    init_default_icon();
    return g_default_icon;
}

GameInfo* game_list_get_game_by_index(GameList *gameList, int index)
{
    if (index < 0 || index >= gameList->count)
        return NULL;
    
    return &gameList->games[index];
}

GameInfo* game_list_get_game_by_id(GameList *gameList, int id)
{
    int i;
    
    for (i = 0; i < gameList->count; i++) {
        if (gameList->games[i].id == id)
            return &gameList->games[i];
    }
    
    return NULL;
}