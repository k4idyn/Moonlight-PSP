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
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <png.h>

#include "game_list_parser.h"
#include "client_identity.h"
#include "icon_cache.h"
#include "fallback_icons.h"

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

/* Statically bounded parser scratch. The PSP-1000 release profile forbids
 * first-party heap use before streaming, so app metadata parsing uses one
 * fixed receive buffer and one fixed per-entry scratch buffer. */
#define MAX_GAME_ENTRY_XML     4096
#define STATIC_ICON_SLOTS      16
#define PNG_DOWNLOAD_BUF_SIZE  (512 * 1024)
#define PNG_ARENA_SIZE         (192 * 1024)
#define PNG_MAX_ROW_BYTES      (4096)
static char s_http_recv_buf[HTTP_RECV_BUFFER_SIZE] __attribute__((aligned(64)));
static char s_entry_buf[MAX_GAME_ENTRY_XML];
static unsigned short s_icon_pool[STATIC_ICON_SLOTS][ICON_PIXEL_COUNT] __attribute__((aligned(64)));
static int s_icon_pool_used = 0;
static unsigned char s_png_download_buf[PNG_DOWNLOAD_BUF_SIZE] __attribute__((aligned(64)));
static unsigned char s_png_arena[PNG_ARENA_SIZE] __attribute__((aligned(64)));
static unsigned char s_png_row_buf[PNG_MAX_ROW_BYTES] __attribute__((aligned(64)));
static unsigned int s_png_arena_pos = 0;

/*--------------------------------------------------------------------------
 * Default "Internal Game" Icon Data (RGB565, 100x150)
 * A simple gray placeholder icon with "Internal Game" text concept
 *--------------------------------------------------------------------------*/

static unsigned short g_default_icon[ICON_PIXEL_COUNT];

/* Flag to track if default icon has been initialized */
static int g_default_icon_initialized = 0;

static void reset_icon_pool(void)
{
    s_icon_pool_used = 0;
}

static unsigned short *alloc_icon_slot(void)
{
    unsigned short *slot;
    if (s_icon_pool_used >= STATIC_ICON_SLOTS)
        return NULL;
    slot = s_icon_pool[s_icon_pool_used++];
    memset(slot, 0, ICON_DATA_SIZE);
    return slot;
}

static void release_last_icon_slot(unsigned short *slot)
{
    if (s_icon_pool_used > 0 && slot == s_icon_pool[s_icon_pool_used - 1])
        s_icon_pool_used--;
}

static void copy_compact_icon_to_padded(const unsigned short *src, unsigned short *dst)
{
    int y;
    memset(dst, 0, ICON_DATA_SIZE);
    for (y = 0; y < ICON_HEIGHT; y++) {
        memcpy(dst + y * ICON_BUFFER_WIDTH,
               src + y * ICON_WIDTH,
               ICON_WIDTH * sizeof(unsigned short));
    }
}

static int assign_fallback_icon(GameInfo *game)
{
    const unsigned short *compact;
    unsigned short *slot;

    if (!game)
        return 0;

    compact = fallback_icon_for_title(game->title);
    if (!compact)
        return 0;

    slot = alloc_icon_slot();
    if (!slot)
        return 0;

    copy_compact_icon_to_padded(compact, slot);
    game->iconData = slot;
    game->iconLoaded = 1;
    return 1;
}

static void ensure_cache_dir(void)
{
    sceIoMkdir("ms0:/PSP", 0777);
    sceIoMkdir("ms0:/PSP/GAME", 0777);
    sceIoMkdir("ms0:/PSP/GAME/Moonlight", 0777);
    sceIoMkdir("ms0:/PSP/GAME/Moonlight/cache", 0777);
}

typedef struct {
    const unsigned char *data;
    png_size_t pos;
    png_size_t size;
} PNGReadState;

static png_voidp PNGAPI png_static_alloc(png_structp png_ptr, png_alloc_size_t size)
{
    unsigned int aligned;
    (void)png_ptr;

    if (size == 0 || size > PNG_ARENA_SIZE)
        return NULL;

    aligned = (s_png_arena_pos + 15u) & ~15u;
    if (aligned + (unsigned int)size > PNG_ARENA_SIZE)
        return NULL;

    s_png_arena_pos = aligned + (unsigned int)size;
    return (png_voidp)(s_png_arena + aligned);
}

static void PNGAPI png_static_release(png_structp png_ptr, png_voidp ptr)
{
    (void)png_ptr;
    (void)ptr;
}

static void png_mem_read(png_structp png, png_bytep buf, png_size_t len)
{
    PNGReadState *st = (PNGReadState *)png_get_io_ptr(png);
    if (!st || st->pos + len > st->size) {
        png_error(png, "png eof");
        return;
    }
    memcpy(buf, st->data + st->pos, len);
    st->pos += len;
}

static int decode_png_to_icon(const unsigned char *png_data, int png_len,
                              unsigned short *icon_out)
{
    png_structp png = NULL;
    png_infop info = NULL;
    PNGReadState st;
    int src_w, src_h;
    int color_type, bit_depth;
    png_size_t rowbytes;
    int sy, next_dy;

    if (!png_data || png_len <= 8 || !icon_out)
        return -1;
    if (png_sig_cmp((png_bytep)png_data, 0, 8) != 0)
        return -1;

    memset(icon_out, 0, ICON_DATA_SIZE);
    s_png_arena_pos = 0;

    png = png_create_read_struct_2(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL,
                                   NULL, png_static_alloc, png_static_release);
    if (!png)
        return -1;

    info = png_create_info_struct(png);
    if (!info) {
        png_destroy_read_struct(&png, NULL, NULL);
        return -1;
    }

    if (setjmp(png_jmpbuf(png))) {
        png_destroy_read_struct(&png, &info, NULL);
        return -1;
    }

    st.data = png_data;
    st.pos = 0;
    st.size = (png_size_t)png_len;
    png_set_read_fn(png, &st, png_mem_read);

    png_read_info(png, info);
    src_w = (int)png_get_image_width(png, info);
    src_h = (int)png_get_image_height(png, info);
    color_type = png_get_color_type(png, info);
    bit_depth = png_get_bit_depth(png, info);

    if (src_w <= 0 || src_h <= 0)
        png_error(png, "bad png size");

    if (bit_depth == 16) png_set_strip_16(png);
    if (color_type == PNG_COLOR_TYPE_PALETTE) png_set_palette_to_rgb(png);
    if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8) png_set_expand_gray_1_2_4_to_8(png);
    if (png_get_valid(png, info, PNG_INFO_tRNS)) png_set_tRNS_to_alpha(png);
    if (color_type == PNG_COLOR_TYPE_RGB ||
        color_type == PNG_COLOR_TYPE_GRAY ||
        color_type == PNG_COLOR_TYPE_PALETTE) png_set_filler(png, 0xFF, PNG_FILLER_AFTER);
    if (color_type == PNG_COLOR_TYPE_GRAY ||
        color_type == PNG_COLOR_TYPE_GRAY_ALPHA) png_set_gray_to_rgb(png);

    png_read_update_info(png, info);
    rowbytes = png_get_rowbytes(png, info);
    if (rowbytes > PNG_MAX_ROW_BYTES)
        png_error(png, "png row too wide");

    next_dy = 0;
    for (sy = 0; sy < src_h; sy++) {
        png_read_row(png, s_png_row_buf, NULL);
        while (next_dy < ICON_HEIGHT && (next_dy * src_h / ICON_HEIGHT) == sy) {
            int dx;
            for (dx = 0; dx < ICON_WIDTH; dx++) {
                int sx = dx * src_w / ICON_WIDTH;
                const unsigned char *px = s_png_row_buf + sx * 4;
                unsigned short r5 = (px[0] >> 3) & 0x1F;
                unsigned short g6 = (px[1] >> 2) & 0x3F;
                unsigned short b5 = (px[2] >> 3) & 0x1F;
                icon_out[next_dy * ICON_BUFFER_WIDTH + dx] =
                    (unsigned short)((b5 << 11) | (g6 << 5) | r5);
            }
            next_dy++;
        }
    }

    png_read_end(png, NULL);
    png_destroy_read_struct(&png, &info, NULL);
    sceKernelDcacheWritebackInvalidateRange(icon_out, ICON_DATA_SIZE);
    return 0;
}

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
    reset_icon_pool();
    
    /* Initialize default icon */
    init_default_icon();
}

int game_list_fetch(GameList *gameList)
{
    char url[512];
    char *recv_buf = s_http_recv_buf;
    int ret;
    
    pspDebugScreenPrintf("game_list_fetch: fetching from %s\n", gameList->hostIp);
    
    memset(recv_buf, 0, HTTP_RECV_BUFFER_SIZE);
    
    /* Build URL for applist endpoint — HTTPS port 47984, requires mTLS.
     * https_launch_get is defined in network_connect.c and handles client cert. */
    snprintf(url, sizeof(url), "/applist?uniqueid=%s&uuid=%s",
             client_identity_get_uid(), client_identity_get_uuid());
    
    /* Use the mbedTLS HTTPS client (defined in network_connect.c).
     * Retry up to 3 times on TLS/HTTP failure — the PSP's TLS stack
     * occasionally fails the first handshake (-0x7280 = SSL_CONN_EOF). */
    extern int https_launch_get(const char *host, int port, const char *path,
                                char *response, int response_size);
    {
        int attempts;
        for (attempts = 0; attempts < 3; attempts++) {
            if (attempts > 0) {
                pspDebugScreenPrintf("game_list_fetch: retry %d/3...\n", attempts + 1);
                sceKernelDelayThread(2000 * 1000); /* 2s between retries */
            }
            ret = https_launch_get(gameList->hostIp, 47984, url, recv_buf, HTTP_RECV_BUFFER_SIZE - 1);
            if (ret >= 0 && strlen(recv_buf) > 0)
                break; /* success */
            pspDebugScreenPrintf("game_list_fetch: attempt %d failed (%d)\n", attempts + 1, ret);
        }
        if (ret < 0 || strlen(recv_buf) == 0) {
            pspDebugScreenPrintf("game_list_fetch: HTTP request failed after %d attempts (%d)\n", attempts, ret);
            return (ret < 0) ? ret : -1;
        }
    }

    /* https_launch_get returns 0 on success; derive actual length from buffer */
    {
        int xml_len = (int)strlen(recv_buf);
        pspDebugScreenPrintf("game_list_fetch: received %d bytes\n", xml_len);

        /* DUMP THE EXACT RAW XML DIRECTLY TO THE LOG SO WE CAN PROVE WHAT APOLLO SENT */
#ifndef RETAIL_BUILD
        SceUID dump_fd = sceIoOpen("ms0:/applist_dump.xml", PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
        if (dump_fd >= 0) {
            sceIoWrite(dump_fd, recv_buf, xml_len);
            sceIoClose(dump_fd);
        }
#endif

        ret = game_list_parse_xml(gameList, recv_buf, xml_len);
    }
    
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
        
        /* Extract entry to bounded static scratch. Oversized game entries are
         * truncated rather than heap-allocated; ID/title/app asset paths are
         * expected near the top of Sunshine's app nodes. */
        int entry_len = entry_end - entry_start + close_tag_len;  /* Include closing tag */
        if (entry_len >= MAX_GAME_ENTRY_XML) {
            pspDebugScreenPrintf("  Game entry too large (%d), truncating to %d bytes\n",
                                 entry_len, MAX_GAME_ENTRY_XML - 1);
            entry_len = MAX_GAME_ENTRY_XML - 1;
        }
        
        memcpy(s_entry_buf, entry_start, entry_len);
        s_entry_buf[entry_len] = '\0';
        
        /* Parse this game entry */
        if (parse_game_entry(s_entry_buf, &gameList->games[games_parsed]) == 0) {
            pspDebugScreenPrintf("  Game %d: ID=%d, Title=%s\n",
                                 games_parsed,
                                 gameList->games[games_parsed].id,
                                 gameList->games[games_parsed].title);
            games_parsed++;
        }

        /* Move past this entry */
        pos = entry_end + close_tag_len;
    }
    
    gameList->count = games_parsed;
    
    pspDebugScreenPrintf("game_list_parse_xml: parsed %d games\n", games_parsed);
    return games_parsed;
}

int game_list_download_icons(GameList *gameList)
{
    int i;
    int success_count = 0;
    
    int cache_count = 0;
    int download_count = 0;
    int fallback_count = 0;
    int default_count = 0;

    pspDebugScreenPrintf("game_list_download_icons: loading %d icons\n",
                         gameList->count);
    
    ensure_cache_dir();

    /* v1.0 PSP-1000 policy: no first-party heap allocation. Existing raw cache
     * art is loaded first; stale/missing icons are downloaded and decoded through
     * fixed PNG buffers; embedded icons remain the offline fallback. */
    for (i = 0; i < gameList->count; i++) {
        GameInfo *game = &gameList->games[i];
        int cache_stale = icon_cache_needs_update(game->id, game->boxArtUrl);
        if (!cache_stale && game_list_load_cached_icon(game)) {
            pspDebugScreenPrintf("  [%d/%d] Loaded from cache: %s\n",
                                 i + 1, gameList->count, game->title);
            cache_count++;
        } else if (game_list_download_icon(game, gameList->hostIp) == 0) {
            icon_cache_record(game->id, game->boxArtUrl);
            pspDebugScreenPrintf("  [%d/%d] Downloaded: %s\n",
                                 i + 1, gameList->count, game->title);
            download_count++;
        } else if (assign_fallback_icon(game)) {
            pspDebugScreenPrintf("  [%d/%d] Fallback: %s\n",
                                 i + 1, gameList->count, game->title);
            fallback_count++;
        } else {
            game->iconData = g_default_icon;
            game->iconLoaded = 1;
            pspDebugScreenPrintf("  [%d/%d] Default icon: %s\n",
                                 i + 1, gameList->count, game->title);
            default_count++;
        }
        success_count++;
    }
    
    pspDebugScreenPrintf("game_list_download_icons: %d/%d icons assigned (cache=%d downloaded=%d fallback=%d default=%d pool=%d/%d)\n",
                         success_count, gameList->count, cache_count, download_count,
                         fallback_count, default_count, s_icon_pool_used, STATIC_ICON_SLOTS);
    return 0;
}

int game_list_download_icon(GameInfo *game, const char *host_ip)
{
    int png_len;
    unsigned short *slot;

    if (!game) return -1;
    if (!host_ip || !host_ip[0])
        return -1;

    if (!game->boxArtUrl[0]) {
        if (game->id == 0)
            return -1;
        snprintf(game->boxArtUrl, MAX_URL_LENGTH,
                 "/appasset?uniqueid=%s&appid=%d&AssetType=2&AssetIdx=0",
                 PSP_UNIQUE_ID, game->id);
    }

    pspDebugScreenPrintf("[ICON] downloading appid=%d url=%s\n",
                         game->id, game->boxArtUrl);
    png_len = https_launch_get_binary(host_ip, 47984, game->boxArtUrl,
                                      (char *)s_png_download_buf,
                                      PNG_DOWNLOAD_BUF_SIZE);
    if (png_len <= 0) {
        pspDebugScreenPrintf("[ICON] download failed appid=%d ret=%d\n", game->id, png_len);
        return -1;
    }

    slot = alloc_icon_slot();
    if (!slot)
        return -1;

    if (decode_png_to_icon(s_png_download_buf, png_len, slot) < 0) {
        release_last_icon_slot(slot);
        pspDebugScreenPrintf("[ICON] decode failed appid=%d len=%d\n", game->id, png_len);
        return -1;
    }

    game->iconData = slot;
    game->iconLoaded = 1;
    game_list_save_icon_to_cache(game);
    return 0;
}

int game_list_load_cached_icon(GameInfo *game)
{
    char path[256];
    unsigned short *slot;
    SceUID fd;
    int ret;

    if (!game)
        return 0;

    game_list_get_icon_path(game, path);
    fd = sceIoOpen(path, PSP_O_RDONLY, 0);
    if (fd < 0)
        return 0;

    slot = alloc_icon_slot();
    if (!slot) {
        sceIoClose(fd);
        return 0;
    }

    ret = sceIoRead(fd, slot, ICON_DATA_SIZE);
    sceIoClose(fd);
    if (ret != ICON_DATA_SIZE) {
        release_last_icon_slot(slot);
        return 0;
    }

    sceKernelDcacheWritebackInvalidateRange(slot, ICON_DATA_SIZE);
    game->iconData = slot;
    game->iconLoaded = 1;
    return 1;
}

int game_list_save_icon_to_cache(const GameInfo *game)
{
    char path[256];
    SceUID fd;
    int ret;
    
    if (!game->iconData || !game->iconLoaded || game->iconData == g_default_icon)
        return -1;
    
    game_list_get_icon_path(game, path);
    
    ensure_cache_dir();
    
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
