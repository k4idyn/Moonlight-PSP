/*
 * settings_menu.h - PSP Settings Menu System for Moonlight
 *
 * Provides a menu interface for configuring streaming settings:
 * - Preset tier (Quality native, Balanced, Performance, or custom)
 * - Resolution dimensions, independently adjustable after preset selection
 * - FPS (10/15/20/30/60/custom; 20fps is a gate, not a ceiling)
 * - Audio enable, bitrate, packet size, theme, and controls
 * - Control Mode (Digital/Analog)
 *
 * Controls:
 * - D-pad Up/Down: Navigate between settings
 * - D-pad Left/Right: Toggle values
 * - Analog stick: Mouse control
 * - L button: Mouse Button Left (MBL)
 * - R button: Mouse Button Right (MBR)
 */

#ifndef SETTINGS_MENU_H
#define SETTINGS_MENU_H

#include <psptypes.h>

#ifdef __cplusplus
extern "C" {
#endif

/*--------------------------------------------------------------------------
 * Setting Option Arrays
 * Defined once in settings_menu.c; declared here for read-only access.
 *--------------------------------------------------------------------------*/

/* Resolution presets — PSP-1000 hardware-tuned
 *
 *   [0] Quality      480x272 native resolution, 10fps tier
 *   [1] Balanced     360x204 exact PSP-panel aspect, 20fps tier
 *   [2] Performance  300x170 lowest even PSP-panel aspect above host floor, 30fps tier, audio off
 *   [3] Custom       user-defined via OSK (defaults to 480x272)
 *
 * The Custom slot is writable — OSK input updates it at runtime.
 * ALL code paths read width/height from these arrays (single source of truth).
 */
#define RESOLUTION_PRESET_COUNT 3
#define RESOLUTION_CUSTOM_INDEX 3
#define RESOLUTION_COUNT 4
extern int          RESOLUTION_WIDTHS[RESOLUTION_COUNT];
extern int          RESOLUTION_HEIGHTS[RESOLUTION_COUNT];
extern const char * RESOLUTION_LABELS[RESOLUTION_COUNT];
extern const int    RESOLUTION_OPTIMAL_FPS_IDX[RESOLUTION_COUNT];
extern const int    RESOLUTION_OPTIMAL_BITRATE[RESOLUTION_COUNT];
extern const int    RESOLUTION_OPTIMAL_PACKET_SIZE[RESOLUTION_COUNT];

/* Update the Custom slot's label after OSK entry. */
void resolution_update_custom(int width, int height);

/* FPS options — only values that evenly divide 60Hz (no judder)
 *   60Hz / 4 = 15fps,  60Hz / 3 = 20fps,
 *   60Hz / 2 = 30fps,  60Hz / 1 = 60fps
 */
extern const char * const FPS_OPTIONS[6];
extern const int    FPS_VALUES[6];
#define FPS_COUNT 6
#define FPS_CUSTOM_INDEX 5


/* Control mode options */
extern const char * const CONTROL_MODE_OPTIONS[2];
#define CONTROL_MODE_COUNT 2

/*--------------------------------------------------------------------------
 * Control Mode Enum
 *--------------------------------------------------------------------------*/
typedef enum {
    CONTROL_MODE_XBOX = 0,
    CONTROL_MODE_BROWSER = 1
} ControlMode;

/*--------------------------------------------------------------------------
 * Settings Config Structure
 *
 * This structure wraps the Moonlight STREAM_CONFIGURATION and adds
 * PSP-specific settings like control mode.
 *--------------------------------------------------------------------------*/
typedef struct {
    /* Moonlight stream configuration */
    int width;              /* Stream width  — supports super-native (e.g. 640) */
    int height;             /* Stream height — supports super-native (e.g. 360) */
    int fps;                /* Frame rate */
    int bitrate;            /* Bitrate in kbps */
    int packetSize;         /* Max packet size */
    int streamingRemotely;  /* Remote streaming flag */
    int audioConfiguration; /* Moonlight audio config; PSP defaults to mono */
    int supportedVideoFormats; /* Supported video codecs */
    int clientRefreshRateX100; /* Display refresh rate x 100 */
    int colorSpace;         /* Color space */
    int colorRange;         /* Color range */
    int encryptionFlags;    /* Encryption flags */
    char remoteInputAesKey[16]; /* AES key for input */
    char remoteInputAesIv[16];  /* AES IV for input */

    /* PSP-specific settings */
    ControlMode controlMode;    /* Digital or Analog control mode */
    int presetIndex;            /* Step-ladder preset index */
    int resolutionIndex;        /* Resolution row index; does not snap defaults */
    int fpsIndex;               /* Index into FPS_OPTIONS */

    /* Pairing persistence — up to 8 remembered paired hosts */
    char pairedHostIps[8][16];  /* IPs of successfully paired hosts */
    int  pairedHostCount;       /* Number of valid entries (0..8) */

    /* Network: local UDP bind address.
     * Leave empty (all zeros) for real PSP hardware — sockets bind INADDR_ANY.
     * For PPSSPP on the same host as the streaming server, set this to a
     * secondary IP alias on the host NIC (e.g. 10.0.0.100) so the server
     * sees a different source IP and routes video back correctly.
     * Add the alias with: netsh interface ipv4 add address "Ethernet" 10.0.0.100 255.255.255.0 */
    char localBindIp[16];       /* Source IP for UDP sockets, or "" = INADDR_ANY */
    int uiThemeIndex;           /* Selected UI accent color index */
    int cabacTestMode;          /* 1 = request Main profile + CABAC in SDP (test only) */
    int audioEnabled;           /* 1 = stream/decode audio, 0 = drain/drop RTP only */
    int disableEncryption;      /* 1 = disable AV encryption; keep RTSP/control compatibility */
} PspConfig;

/*--------------------------------------------------------------------------
 * Menu State
 *--------------------------------------------------------------------------*/
typedef struct {
    int currentSelection;   /* Currently selected menu item */
    int presetIndex;        /* Selected step-ladder preset */
    int resolutionIndex;    /* Selected resolution option; no default snap */
    int fpsIndex;           /* Selected FPS option */
    int customFpsValue;     /* The custom FPS value if custom is selected */
    int audioEnabled;       /* Local state for Audio option */
    int controlModeIndex;   /* Selected control mode option */
    int bitrate;            /* Bitrate in kbps */
    int packetSize;         /* RTP payload size in bytes */
    int uiThemeIndex;       /* Selected UI theme index */
    int needsRedraw;        /* Flag to indicate menu needs redraw */
} MenuState;

/*--------------------------------------------------------------------------
 * Public API
 *--------------------------------------------------------------------------*/

/*
 * settings_menu_init - Initialize the settings menu
 *
 * @config: Pointer to PspConfig to initialize with defaults
 *
 * Sets default values:
 * - Preset: Performance
 * - Resolution: 300x170
 * - FPS: 30
 * - Audio: Disabled
 * - Control Mode: Xbox
 * - Bitrate: 384 kbps
 * - Packet size: 1056 bytes
 */
void settings_menu_init(PspConfig *config);

/*
 * settings_menu_run - Run the settings menu loop
 *
 * @config: Pointer to PspConfig to update with user selections
 *
 * Displays the menu and handles D-pad navigation.
 * Returns when user presses START or CROSS to confirm.
 *
 * Navigation:
 * - UP/DOWN: Move between settings
 * - LEFT/RIGHT: Change values
 * - START/CROSS: Confirm and exit
 * - CIRCLE: Cancel (keeps current values)
 *
 * Returns: 0 on confirm, -1 on cancel
 */
int settings_menu_run(PspConfig *config);

/*
 * settings_menu_apply - Apply settings to Moonlight STREAM_CONFIGURATION
 *
 * @psp_config: Pointer to PspConfig with user selections
 * @stream_config: Pointer to Moonlight STREAM_CONFIGURATION to update
 *
 * Converts PSP config settings to Moonlight stream configuration.
 */
void settings_menu_apply(const PspConfig *psp_config, void *stream_config);

/*
 * settings_menu_draw - Draw the settings menu (can be called manually)
 *
 * @config: Pointer to PspConfig to display
 *
 * Renders the menu on the PSP debug screen.
 */
void settings_menu_draw(const PspConfig *config);

#ifdef __cplusplus
}
#endif

#endif /* SETTINGS_MENU_H */
