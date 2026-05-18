# Game List Parser for PSP Moonlight

## Overview

This module implements a game list parser and icon downloader for the PSP Moonlight streaming client. It fetches the game list from a Sunshine/GameStream server and downloads game icons for display in the game grid UI.

## Features

- **XML Parser**: Simple string-based XML parser to extract game information
- **HTTP/HTTPS Client**: Uses the project TLS client for `/applist` and binary icon asset downloads
- **Icon Downloader**: Downloads Sunshine PNG box art through the normal `BoxArtUrl` or `/appasset` path
- **Static PNG Decode**: Decodes through fixed libpng buffers and a fixed arena; no first-party heap allocation
- **Fallback Support**: Uses embedded fallback art first, then the generated default texture when icons are missing
- **Cache System**: Stores downloaded icons in `ms0:/PSP/GAME/Moonlight/cache/`

## Files

- `game_list_parser.h` - Header file with structures and function declarations
- `game_list_parser.c` - Implementation of XML parser and icon downloader
- `game_grid_ui.cpp` - Updated UI to integrate with the parser

## API Usage

### Initialize and Fetch Game List

```c
GameList gameList;
game_list_init(&gameList, "192.168.1.100");

// Fetch game list from server
int ret = game_list_fetch(&gameList);
if (ret == 0) {
    // Download icons
    game_list_download_icons(&gameList);
}
```

### Access Games

```c
// Get game by index
GameInfo* game = game_list_get_game_by_index(&gameList, 0);
if (game) {
    printf("Game: %s (ID: %d)\n", game->title, game->id);
}

// Get game by ID
GameInfo* game = game_list_get_game_by_id(&gameList, 12345);
```

### Cleanup

```c
game_list_cleanup(&gameList);
```

## XML Format Supported

The parser supports multiple XML formats from Sunshine/GameStream:

```xml
<!-- Format 1: Sunshine -->
<applist>
    <App>
        <ID>12345</ID>
        <AppName>Game Name</AppName>
        <BoxArtUrl>http://example.com/icon.png</BoxArtUrl>
    </App>
</applist>

<!-- Format 2: GameStream -->
<games>
    <Game>
        <ID>12345</ID>
        <Title>Game Name</Title>
        <BoxArtURL>http://example.com/icon.png</BoxArtURL>
    </Game>
</games>
```

## Cache Structure

Icons are decoded from PNG via libpng and converted directly to RGB565 for display. Runtime icons are stored in padded `128x256` RGB565 texture slots; the visible art area is `100x150`. Each file is stored as raw RGB565:

```
ms0:/PSP/GAME/Moonlight/cache/
|-- 12345.raw    (Game ID 12345, 128x256x2 bytes = 65,536 bytes)
|-- 67890.raw
`-- ...
```

Current raw cache files are 65,536 bytes each (`128x256x2`). An index file (`cache/index.ini`) tracks each entry's source BoxArtURL, last fetch date, and a CRC32 of the URL. On each `/applist` fetch, URLs are compared against the index. If the URL changed or the cache entry is older than `max_cache_age_days` (default 7), the icon is re-downloaded. Cached icons are loaded first; stale or missing entries are downloaded synchronously while the game grid is being populated.

## Default Icon

When an icon cannot be downloaded or is missing, the system first tries embedded title-specific fallback art. If no fallback matches, it uses a generated default icon:
- Gray gradient background
- Simple border
- Current format: RGB565 texture in the same padded icon layout

## Integration with Game Grid UI

The `game_grid_ui.cpp` fetches the game list via `game_list_parser.c` and renders a scrollable 3-column grid. Scroll offset is tracked in rows; D-pad navigates within and across rows. The grid supports any number of titles - see `docs/UI_FLOW.md` for scrolling behavior.

## Configuration

The host IP is resolved from the selected host in the Host Discovery screen and passed into the game list fetch at session start. No hardcoded IP is needed.

## Limitations

- Maximum 64 game list entries are tracked in static metadata and the icon cache index.
- Up to 16 icon texture slots are resident in RAM during the game grid.
- PNG downloads are capped by a fixed 512 KB receive buffer.
- PNG decode uses a fixed 192 KB arena and rejects rows wider than 4096 bytes.
- Icon downloads use the binary-safe HTTPS path so embedded NUL bytes in PNG bodies are preserved.

## Build Integration

The `Makefile` has been updated to include `game_list_parser.o` in the build:

```makefile
OBJS = main.o network_connect.o network_me.o decoder_cpu.o \
       display_gpu.o input.o rtp_reassembly.o host_discovery.o pairing_pin_ui.o \
       settings_menu.o hud.o stream_session.o pairing.o game_list_parser.o
```

## Dependencies

- PSPSDK with `psphttp.h`
- Standard C libraries (stdio.h, stdlib.h, string.h)
- PSP file I/O (`pspiofilemgr.h`)
