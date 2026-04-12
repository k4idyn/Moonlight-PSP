/*
 * game_list_parser.h - Game List Parser and Icon Downloader for PSP Moonlight
 *
 * Fetches the game list from Sunshine/NVIDIA GameStream server via HTTP.
 * Parses the XML response to extract game ID, title, and box art URL.
 * Downloads game icons (144x80 PNG) to local cache directory.
 *
 * Cache location: ms0:/PSP/GAME/Moonlight/cache/
 */

#ifndef GAME_LIST_PARSER_H
#define GAME_LIST_PARSER_H

#include <psptypes.h>

#ifdef __cplusplus
extern "C" {
#endif

/*--------------------------------------------------------------------------
 * Constants
 *--------------------------------------------------------------------------*/

#define MAX_GAMES               100     /* Maximum number of games in list */
#define MAX_TITLE_LENGTH        128     /* Maximum game title length */
#define MAX_URL_LENGTH          512     /* Maximum URL length */
#define MAX_HOST_IP_LENGTH      64      /* Maximum host IP address length */

/* Icon dimensions */
#define ICON_WIDTH              100
#define ICON_HEIGHT             150
#define ICON_BUFFER_WIDTH       128
#define ICON_BUFFER_HEIGHT      256
#define ICON_PIXEL_COUNT        (ICON_BUFFER_WIDTH * ICON_BUFFER_HEIGHT)
#define ICON_DATA_SIZE          (ICON_PIXEL_COUNT * 2)  /* RGB565 */

/* Cache directory path on PSP memory stick */
#define CACHE_DIR               "ms0:/PSP/GAME/Moonlight/cache/"
#define CACHE_DIR_LENGTH        48

/* HTTP buffer size for receiving data (512 KB) */
#define HTTP_RECV_BUFFER_SIZE   524288

/*--------------------------------------------------------------------------
 * Game Info Structure
 *--------------------------------------------------------------------------*/

typedef struct {
    int id;                             /* Game ID from server */
    char title[MAX_TITLE_LENGTH];       /* Game display name */
    char boxArtUrl[MAX_URL_LENGTH];     /* Box art image URL */
    unsigned short* iconData;           /* Pointer to RGB565 icon data */
    int iconLoaded;                     /* 1 if icon is loaded, 0 otherwise */
} GameInfo;

/*--------------------------------------------------------------------------
 * Game List Structure
 *--------------------------------------------------------------------------*/

typedef struct {
    GameInfo games[MAX_GAMES];          /* Array of game info */
    int count;                          /* Number of games in list */
    char hostIp[MAX_HOST_IP_LENGTH];    /* Host IP address */
} GameList;

/*--------------------------------------------------------------------------
 * Download Status
 *--------------------------------------------------------------------------*/

typedef enum {
    DOWNLOAD_STATUS_IDLE = 0,           /* No download in progress */
    DOWNLOAD_STATUS_DOWNLOADING,        /* Download in progress */
    DOWNLOAD_STATUS_COMPLETE,           /* Download complete */
    DOWNLOAD_STATUS_ERROR               /* Download error */
} DownloadStatus;

/*--------------------------------------------------------------------------
 * Public API
 *--------------------------------------------------------------------------*/

/**
 * game_list_init - Initialize the game list parser
 *
 * @gameList: Pointer to GameList structure to initialize
 * @hostIp:   IP address of the Sunshine/GameStream host
 *
 * Initializes the game list structure and sets the host IP.
 */
void game_list_init(GameList *gameList, const char *hostIp);

/**
 * game_list_fetch - Fetch the game list from the host server
 *
 * @gameList: Pointer to GameList structure to populate
 *
 * Makes an HTTP GET request to https://[HOST_IP]:47884/applist
 * and parses the XML response to extract game information.
 *
 * Returns: 0 on success, negative error code on failure
 */
int game_list_fetch(GameList *gameList);

/**
 * game_list_parse_xml - Parse XML response to extract game info
 *
 * @gameList: Pointer to GameList structure to populate
 * @xmlData:  Null-terminated XML string to parse
 * @xmlSize:  Size of the XML data in bytes
 *
 * Parses the XML response from the server and extracts:
 * - Game ID
 * - Game title
 * - Box art URL
 *
 * Returns: Number of games parsed, or negative on error
 */
int game_list_parse_xml(GameList *gameList, const char *xmlData, int xmlSize);

/**
 * game_list_download_icons - Download all game icons in background
 *
 * @gameList: Pointer to GameList with games to download icons for
 *
 * Downloads 144x80 PNG icons for each game to the cache directory.
 * Icons are saved as ms0:/PSP/GAME/Moonlight/cache/[GAME_ID].png
 *
 * Returns: 0 on success, negative on error
 */
int game_list_download_icons(GameList *gameList);

/**
 * game_list_download_icon - Download a single game icon
 *
 * @game:    Pointer to GameInfo to download icon for
 * @host_ip: Sunshine host IP address for the HTTPS download
 *
 * Downloads the box art PNG from the game's boxArtUrl via mTLS.
 * Decodes with libpng, scales to ICON_WIDTH x ICON_HEIGHT, converts to RGB565.
 * Saves to cache directory and stores pointer in game->iconData.
 *
 * Returns: 0 on success, negative on error
 */
int game_list_download_icon(GameInfo *game, const char *host_ip);

/**
 * game_list_load_cached_icon - Load icon from cache if available
 *
 * @game: Pointer to GameInfo to load icon for
 *
 * Checks if icon exists in cache directory.
 * If found, loads the RGBA8888 data into game->iconData.
 *
 * Returns: 1 if icon was loaded from cache, 0 if not found
 */
int game_list_load_cached_icon(GameInfo *game);

/**
 * game_list_save_icon_to_cache - Save icon data to cache file
 *
 * @game: Pointer to GameInfo with icon data to save
 *
 * Saves the RGBA8888 icon data to cache directory.
 * Filename: ms0:/PSP/GAME/Moonlight/cache/[GAME_ID].raw
 *
 * Returns: 0 on success, negative on error
 */
int game_list_save_icon_to_cache(const GameInfo *game);

/**
 * game_list_get_icon_path - Get the cache file path for a game icon
 *
 * @game:     Pointer to GameInfo
 * @pathBuf:  Buffer to store the path (must be at least 256 bytes)
 *
 * Generates the full path to the cached icon file.
 */
void game_list_get_icon_path(const GameInfo *game, char *pathBuf);

/**
 * game_list_cleanup - Free all allocated resources
 *
 * @gameList: Pointer to GameList to clean up
 *
 * Frees all allocated icon data and resets the game list.
 */
void game_list_cleanup(GameList *gameList);

/**
 * game_list_get_default_icon - Get pointer to default "Internal Game" icon
 *
 * Returns: Pointer to static RGB565 icon data for default icon
 */
unsigned short* game_list_get_default_icon(void);

/**
 * game_list_get_game_by_index - Get game info by index
 *
 * @gameList: Pointer to GameList
 * @index:    Index of the game (0 to count-1)
 *
 * Returns: Pointer to GameInfo, or NULL if index is out of bounds
 */
GameInfo* game_list_get_game_by_index(GameList *gameList, int index);

/**
 * game_list_get_game_by_id - Get game info by ID
 *
 * @gameList: Pointer to GameList
 * @id:       Game ID to search for
 *
 * Returns: Pointer to GameInfo, or NULL if not found
 */
GameInfo* game_list_get_game_by_id(GameList *gameList, int id);

#ifdef __cplusplus
}
#endif

#endif /* GAME_LIST_PARSER_H */