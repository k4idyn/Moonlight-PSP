/*
 * input.c - PSP button input polling and Moonlight controller transmission
 *
 * Polls the PSP controller at 60Hz using sceCtrlPeekBufferPositive,
 * maps PSP buttons and analog stick to Moonlight's controller format,
 * and sends NV_MULTI_CONTROLLER_PACKET (34 bytes) through the encrypted
 * ENet control stream (channel 0x10, type 0x0206).
 *
 * Button Mapper (PSP has no L2/R2 or right stick):
 *   Default: L + face-button diamond -> Right Stick Up/Down/Left/Right
 *   Optional: L + analog nub         -> Right Stick analog
 *   L + mapped button                -> virtual LT/RT/L3/R3
 */

#include <pspkernel.h>
#include <pspctrl.h>
#include <pspiofilemgr.h>
#include <psppower.h>
#include <string.h>
#include <stdio.h>

#include "shared.h"
#include "control_stream.h"
#include "diag_log.h"
#include "settings_menu.h"
#include "input.h"

/* ------------------------------------------------------------------ *
 * Moonlight controller button bit-flags (u16 bitmask)
 * ------------------------------------------------------------------ */
#define ML_A            0x1000
#define ML_B            0x2000
#define ML_X            0x4000
#define ML_Y            0x8000
#define ML_START        0x0010
#define ML_BACK         0x0020
#define ML_LB           0x0100
#define ML_RB           0x0200
#define ML_LS_CLK       0x0040
#define ML_RS_CLK       0x0080
#define ML_DPAD_UP      0x0001
#define ML_DPAD_DOWN    0x0002
#define ML_DPAD_LEFT    0x0004
#define ML_DPAD_RIGHT   0x0008

/* Moonlight NV_MULTI_CONTROLLER magic (little-endian 0x0000000C) */
#define MULTI_CONTROLLER_MAGIC  0x0000000C

/* Right-stick emulation magnitude when a combo fires */
#define RSTICK_MAGNITUDE    24000
#define RSTICK_ANALOG_DEADZONE  3200

/* Map config path on Memory Stick */
#define MAP_CFG_PATH        "ms0:/moonlight/map.cfg"

typedef struct {
    uint32_t l2_button;
    uint32_t r2_button;
    uint32_t rs_up_button;
    uint32_t rs_down_button;
    uint32_t rs_left_button;
    uint32_t rs_right_button;
    uint32_t l3_button;
    uint32_t r3_button;
} ButtonMappingV1;

/* ------------------------------------------------------------------ *
 * ButtonMapping - loaded from map.cfg
 *
 * Combo modifier is always L-trigger (PSP_CTRL_LTRIGGER).
 * When L is held, the mapped psp_button activates the action.
 * ------------------------------------------------------------------ */
/* ------------------------------------------------------------------ *
 * Browser-mode mouse packet constants (Gen5+ protocol)
 * ------------------------------------------------------------------ */
#define MOUSE_MOVE_REL_MAGIC_GEN5   0x00000007
#define MOUSE_BUTTON_DOWN_MAGIC     0x00000008
#define MOUSE_BUTTON_UP_MAGIC       0x00000009
#define MOUSE_BUTTON_LEFT           1
#define MOUSE_BUTTON_RIGHT          3
#define MOUSE_DEADZONE              18
#define MOUSE_BASE_SPEED            12
#define MOUSE_ACCEL_SPEED           16
#define MOUSE_AXIS_RANGE            (127 - MOUSE_DEADZONE)

/* ENet channel assignments per Moonlight protocol (moonlight-common-c) */
#define CTRL_CHANNEL_KEYBOARD     0x02
#define CTRL_CHANNEL_MOUSE        0x03
#define CTRL_CHANNEL_GAMEPAD0     0x10  /* CTRL_CHANNEL_GAMEPAD_BASE + 0 */

/* ── Phase 3 Protocol Constants ──────────────────────────────────── */

/* Keyboard event (Type 5) */
#define KEYBOARD_MAGIC            0x00000005
#define KEY_ACTION_DOWN           0x03
#define KEY_ACTION_UP             0x04

/* Controller Arrival (Type 0x37) */
#define CONTROLLER_ARRIVAL_MAGIC  0x00000037
#define CONTROLLER_TYPE_XBOX      1
/* Supported button flags: all standard Xbox buttons.
 * PSP can emulate most via button mapper. */
#define SUPPORTED_BUTTON_FLAGS    0x0000FFFF
/* Capabilities: we have analog triggers (via L+combo) */
#define CONTROLLER_CAP_ANALOG_TRIGGERS  0x01

/* Controller Battery (Type 0x40) */
#define CONTROLLER_BATTERY_MAGIC  0x00000040
#define BATTERY_STATE_UNKNOWN     0x00
#define BATTERY_STATE_NOT_PRESENT 0x01
#define BATTERY_STATE_DISCHARGING 0x02
#define BATTERY_STATE_CHARGING    0x04
#define BATTERY_STATE_FULL        0x08

/* Scroll Event (Type 0x09, Gen5) */
#define SCROLL_MAGIC_GEN5         0x00000009
/* High-res Scroll (Type 0x33) */
#define SCROLL_HIRES_MAGIC        0x00000033

/* Battery report interval: every 30 seconds */
#define BATTERY_REPORT_INTERVAL_US  (30 * 1000 * 1000)

/* ------------------------------------------------------------------ *
 * State tracking
 * ------------------------------------------------------------------ */
static int          g_initialized = 0;
static SceCtrlData  g_prev;      /* previous frame's controller state */
static int          g_prev_valid; /* 0 until first poll completes  */
ButtonMapping g_mapping;  /* loaded combo mappings */
static int          g_mapping_loaded = 0;

/* Access to global config for control mode */
extern PspConfig g_psp_config;
extern volatile unsigned int g_remote_analog_active;
extern volatile unsigned int g_remote_analog_lx;
extern volatile unsigned int g_remote_analog_ly;

/* Browser-mode button-press memory for edge detection */
static int g_mouse_l_down = 0;
static int g_mouse_r_down = 0;
static int g_last_battery_percent = 100;
static uint8_t g_last_battery_state = BATTERY_STATE_FULL;
static int g_suppress_combo_prev = 0;

/* Phase 3: Battery report timer */
static SceUInt32 g_last_battery_report_us = 0;

/* Phase 3: Keyboard OSK trigger state (R+Triangle edge detection) */
static int g_osk_trigger_prev = 0;

/* ------------------------------------------------------------------ *
 * map_cfg I/O
 * ------------------------------------------------------------------ */
static void set_default_mapping(void)
{
    memset(&g_mapping, 0, sizeof(g_mapping));
    g_mapping.version          = BUTTON_MAPPING_VERSION;
    g_mapping.modifier_button  = PSP_CTRL_LTRIGGER;
    g_mapping.right_stick_mode = RIGHT_STICK_MODE_BUTTONS;
    g_mapping.l2_button        = PSP_CTRL_LEFT;
    g_mapping.r2_button        = PSP_CTRL_RIGHT;
    g_mapping.rs_up_button     = PSP_CTRL_TRIANGLE;
    g_mapping.rs_down_button   = PSP_CTRL_CROSS;
    g_mapping.rs_left_button   = PSP_CTRL_SQUARE;
    g_mapping.rs_right_button  = PSP_CTRL_CIRCLE;
    g_mapping.l3_button        = PSP_CTRL_DOWN;
    g_mapping.r3_button        = PSP_CTRL_UP;
}

static int mapping_button_supported(uint32_t button)
{
    switch (button) {
        case 0:
        case PSP_CTRL_CROSS:
        case PSP_CTRL_CIRCLE:
        case PSP_CTRL_SQUARE:
        case PSP_CTRL_TRIANGLE:
        case PSP_CTRL_UP:
        case PSP_CTRL_DOWN:
        case PSP_CTRL_LEFT:
        case PSP_CTRL_RIGHT:
        case PSP_CTRL_LTRIGGER:
        case PSP_CTRL_RTRIGGER:
        case PSP_CTRL_START:
        case PSP_CTRL_SELECT:
            return 1;
        default:
            return 0;
    }
}

static int legacy_mapping_is_factory_default(const ButtonMappingV1 *m)
{
    return m &&
           m->l2_button       == PSP_CTRL_CROSS &&
           m->r2_button       == PSP_CTRL_SQUARE &&
           m->rs_up_button    == PSP_CTRL_UP &&
           m->rs_down_button  == PSP_CTRL_DOWN &&
           m->rs_left_button  == PSP_CTRL_LEFT &&
           m->rs_right_button == PSP_CTRL_RIGHT &&
           m->l3_button       == PSP_CTRL_TRIANGLE &&
           m->r3_button       == PSP_CTRL_CIRCLE;
}

static void sanitize_mapping_button(uint32_t *button, uint32_t fallback, const char *name)
{
    uint32_t replacement = fallback;
    if (!button)
        return;

    if (g_mapping.modifier_button != 0 && replacement == g_mapping.modifier_button)
        replacement = 0;

    if (!mapping_button_supported(*button)) {
        diag_log_write("INP", "Mapping %s invalid 0x%08X -> 0x%08X",
                       name, *button, replacement);
        *button = replacement;
    }

    if (g_mapping.modifier_button != 0 && *button == g_mapping.modifier_button) {
        diag_log_write("INP", "Mapping %s matched modifier 0x%08X -> 0x%08X",
                       name, *button, replacement);
        *button = replacement;
    }
}

static void sanitize_mapping(void)
{
    g_mapping.version = BUTTON_MAPPING_VERSION;

    if (!mapping_button_supported(g_mapping.modifier_button)) {
        diag_log_write("INP", "Mapping modifier invalid 0x%08X -> 0x%08X",
                       g_mapping.modifier_button, PSP_CTRL_LTRIGGER);
        g_mapping.modifier_button = PSP_CTRL_LTRIGGER;
    }

    if (g_mapping.right_stick_mode > RIGHT_STICK_MODE_ANALOG_NUB) {
        diag_log_write("INP", "Mapping right-stick mode invalid %u -> %u",
                       g_mapping.right_stick_mode, RIGHT_STICK_MODE_BUTTONS);
        g_mapping.right_stick_mode = RIGHT_STICK_MODE_BUTTONS;
    }

    sanitize_mapping_button(&g_mapping.l2_button,       PSP_CTRL_LEFT,     "LT");
    sanitize_mapping_button(&g_mapping.r2_button,       PSP_CTRL_RIGHT,    "RT");
    sanitize_mapping_button(&g_mapping.rs_up_button,    PSP_CTRL_TRIANGLE, "RS_UP");
    sanitize_mapping_button(&g_mapping.rs_down_button,  PSP_CTRL_CROSS,    "RS_DOWN");
    sanitize_mapping_button(&g_mapping.rs_left_button,  PSP_CTRL_SQUARE,   "RS_LEFT");
    sanitize_mapping_button(&g_mapping.rs_right_button, PSP_CTRL_CIRCLE,   "RS_RIGHT");
    sanitize_mapping_button(&g_mapping.l3_button,       PSP_CTRL_DOWN,     "L3");
    sanitize_mapping_button(&g_mapping.r3_button,       PSP_CTRL_UP,       "R3");
}

static void log_mapping(const char *source)
{
    diag_log_write("INP",
                   "Mapping %s: ver=%u mod=0x%08X rsMode=%u LT=0x%08X RT=0x%08X RSU=0x%08X RSD=0x%08X RSL=0x%08X RSR=0x%08X L3=0x%08X R3=0x%08X",
                   source ? source : "load",
                   g_mapping.version,
                   g_mapping.modifier_button,
                   g_mapping.right_stick_mode,
                   g_mapping.l2_button,
                   g_mapping.r2_button,
                   g_mapping.rs_up_button,
                   g_mapping.rs_down_button,
                   g_mapping.rs_left_button,
                   g_mapping.rs_right_button,
                   g_mapping.l3_button,
                   g_mapping.r3_button);
}

static void load_mapping(void)
{
    SceUID fd;
    int read_len;
    set_default_mapping();

    fd = sceIoOpen(MAP_CFG_PATH, PSP_O_RDONLY, 0);
    if (fd < 0) {
        log_mapping("defaults");
        g_mapping_loaded = 1;
        return;
    }
    if (fd < 0) return; /* no file — use defaults */

    /* File is a raw struct dump for simplicity */
    read_len = sceIoRead(fd, &g_mapping, sizeof(g_mapping));
    sceIoClose(fd);

    if (read_len == (int)sizeof(ButtonMappingV1)) {
        ButtonMappingV1 legacy;
        memcpy(&legacy, &g_mapping, sizeof(legacy));
        if (legacy_mapping_is_factory_default(&legacy)) {
            set_default_mapping();
            diag_log_write("INP", "Migrated legacy factory map.cfg to v2 defaults");
        } else {
            set_default_mapping();
            g_mapping.l2_button       = legacy.l2_button;
            g_mapping.r2_button       = legacy.r2_button;
            g_mapping.rs_up_button    = legacy.rs_up_button;
            g_mapping.rs_down_button  = legacy.rs_down_button;
            g_mapping.rs_left_button  = legacy.rs_left_button;
            g_mapping.rs_right_button = legacy.rs_right_button;
            g_mapping.l3_button       = legacy.l3_button;
            g_mapping.r3_button       = legacy.r3_button;
            diag_log_write("INP", "Loaded legacy custom map.cfg (%d bytes)", read_len);
        }
    } else if (read_len != (int)sizeof(g_mapping)) {
        diag_log_write("INP", "Ignoring malformed map.cfg size=%d expected=%u",
                       read_len, (unsigned int)sizeof(g_mapping));
        set_default_mapping();
    }

    sanitize_mapping();
    log_mapping("loaded");
    g_mapping_loaded = 1;
}

void input_save_mapping(void)
{
    SceUID fd;
    g_mapping.version = BUTTON_MAPPING_VERSION;
    sanitize_mapping();
    sceIoMkdir("ms0:/moonlight", 0777);
    fd = sceIoOpen(MAP_CFG_PATH,
                   PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd < 0) return;
    sceIoWrite(fd, &g_mapping, sizeof(g_mapping));
    sceIoClose(fd);
    g_mapping_loaded = 1;
}

void button_mapping_get(ButtonMapping *mapping) {
    if (!g_mapping_loaded) {
        load_mapping();
    }
    if (mapping) *mapping = g_mapping;
}

void button_mapping_set(const ButtonMapping *mapping) {
    if (mapping) {
        g_mapping = *mapping;
        input_save_mapping();
    }
}

/* ------------------------------------------------------------------ *
 * Analog stick mapping: PSP (0-255, center 128) → int16 (-32768..32767)
 *
 * Formula: out = (in - 128) * 258
 *   0   → -128*258 = -33024  ≈ -32768 (clamped)
 *   128 → 0
 *   255 →  127*258 =  32766  ≈  32767
 *
 * We clamp the result to stay within int16 range.
 * ------------------------------------------------------------------ */
static inline int16_t map_analog(uint8_t val)
{
    int32_t out = ((int32_t)val - 128) * 258;
    /* Cap at -32767 (not -32768) so negation for Y-axis inversion
     * cannot overflow int16_t.  Same approach as moonlight-qt. */
    if (out >  32767) out =  32767;
    if (out < -32767) out = -32767;
    return (int16_t)out;
}

static inline void put_le16(uint8_t *dst, uint16_t value)
{
    dst[0] = (uint8_t)(value & 0xFF);
    dst[1] = (uint8_t)((value >> 8) & 0xFF);
}

static inline void put_le32(uint8_t *dst, uint32_t value)
{
    dst[0] = (uint8_t)(value & 0xFF);
    dst[1] = (uint8_t)((value >> 8) & 0xFF);
    dst[2] = (uint8_t)((value >> 16) & 0xFF);
    dst[3] = (uint8_t)((value >> 24) & 0xFF);
}

static inline void put_be32(uint8_t *dst, uint32_t value)
{
    dst[0] = (uint8_t)((value >> 24) & 0xFF);
    dst[1] = (uint8_t)((value >> 16) & 0xFF);
    dst[2] = (uint8_t)((value >> 8) & 0xFF);
    dst[3] = (uint8_t)(value & 0xFF);
}

static inline int16_t zero_deadzone_axis(int16_t value)
{
    if (value > -RSTICK_ANALOG_DEADZONE && value < RSTICK_ANALOG_DEADZONE)
        return 0;
    return value;
}

static void consume_psp_button(uint32_t psp_button, uint16_t *button_flags)
{
    if (!button_flags)
        return;

    switch (psp_button) {
        case PSP_CTRL_CROSS:    *button_flags &= ~ML_A; break;
        case PSP_CTRL_CIRCLE:   *button_flags &= ~ML_B; break;
        case PSP_CTRL_SQUARE:   *button_flags &= ~ML_X; break;
        case PSP_CTRL_TRIANGLE: *button_flags &= ~ML_Y; break;
        case PSP_CTRL_START:    *button_flags &= ~ML_START; break;
        case PSP_CTRL_SELECT:   *button_flags &= ~ML_BACK; break;
        case PSP_CTRL_LTRIGGER: *button_flags &= ~ML_LB; break;
        case PSP_CTRL_RTRIGGER: *button_flags &= ~ML_RB; break;
        case PSP_CTRL_UP:       *button_flags &= ~ML_DPAD_UP; break;
        case PSP_CTRL_DOWN:     *button_flags &= ~ML_DPAD_DOWN; break;
        case PSP_CTRL_LEFT:     *button_flags &= ~ML_DPAD_LEFT; break;
        case PSP_CTRL_RIGHT:    *button_flags &= ~ML_DPAD_RIGHT; break;
        default: break;
    }
}

/* ------------------------------------------------------------------ *
 * Build the NV_MULTI_CONTROLLER_PACKET (34 bytes, mixed-endian)
 *
 * Wire layout per moonlight-common-c:
 *   [0..3]   u32  headerA     = LE32 magic 0x0000000C
 *   [4..5]   u16  headerB     = BE16 0x001A  (= 26, remaining size field)
 *   [6..7]   u16  controllerNumber = LE16 0 (first controller)
 *   [8..9]   u16  activeGamepadMask = LE16 0x0001
 *   [10..11] u16  midB        = BE16 0x0014  (= 20)
 *   [12..13] u16  buttonFlags = LE16
 *   [14]     u8   leftTrigger
 *   [15]     u8   rightTrigger
 *   [16..17] s16  leftStickX  (LE16)
 *   [18..19] s16  leftStickY  (LE16)
 *   [20..21] s16  rightStickX (LE16)
 *   [22..23] s16  rightStickY (LE16)
 *   [24..25] u16  tailA       = BE16 0x009C
 *   [26..27] u16  tailB       = BE16 0x0055
 *   [28..29] u16  tailC       = BE16 0x0000
 *   [30..31] u16  tailD       = BE16 0x0000
 *   [32..33] u16  tailE       = BE16 0x0000
 * ------------------------------------------------------------------ */
static void build_packet(uint8_t *buf, uint16_t buttons,
                         uint8_t lt, uint8_t rt,
                         int16_t lsx, int16_t lsy,
                         int16_t rsx, int16_t rsy)
{
    /* NV_INPUT_HEADER.size is BE32 and excludes the size field itself. */
    put_be32(&buf[0], 30);

    /* NV_INPUT_HEADER.magic is LE32 for Gen5+ multi-controller. */
    put_le32(&buf[4], MULTI_CONTROLLER_MAGIC);

    /* MC fields are LE16 in Gen5+/Gen7. */
    put_le16(&buf[8],  0x001A);          /* headerB */
    put_le16(&buf[10], 0x0000);          /* controllerNumber */
    put_le16(&buf[12], 0x0001);          /* activeGamepadMask */
    put_le16(&buf[14], 0x0014);          /* midB */
    put_le16(&buf[16], buttons);         /* buttonFlags */

    buf[18] = lt;
    buf[19] = rt;

    put_le16(&buf[20], (uint16_t)lsx);
    put_le16(&buf[22], (uint16_t)lsy);
    put_le16(&buf[24], (uint16_t)rsx);
    put_le16(&buf[26], (uint16_t)rsy);

    put_le16(&buf[28], 0x009C);          /* tailA */
    put_le16(&buf[30], 0x0000);          /* buttonFlags2 (Sunshine extension) */
    put_le16(&buf[32], 0x0055);          /* tailB */
}

static void apply_button_mapper_v2(uint32_t psp_buttons,
                                   int16_t *lsx, int16_t *lsy,
                                   uint16_t *button_flags,
                                   uint8_t  *lt, uint8_t  *rt,
                                   int16_t  *rsx, int16_t *rsy)
{
    uint32_t modifier = g_mapping.modifier_button;
    int combo_active = 0;

    *lt  = 0;
    *rt  = 0;
    *rsx = 0;
    *rsy = 0;

    if (modifier == 0 || !(psp_buttons & modifier))
        return;

    if (g_mapping.right_stick_mode == RIGHT_STICK_MODE_ANALOG_NUB) {
        int16_t ax = zero_deadzone_axis(*lsx);
        int16_t ay = zero_deadzone_axis(*lsy);
        if (ax != 0 || ay != 0) {
            *rsx = ax;
            *rsy = ay;
            *lsx = 0;
            *lsy = 0;
            combo_active = 1;
        }
    } else {
        if (g_mapping.rs_up_button && (psp_buttons & g_mapping.rs_up_button)) {
            *rsy = RSTICK_MAGNITUDE;
            consume_psp_button(g_mapping.rs_up_button, button_flags);
            combo_active = 1;
        }
        if (g_mapping.rs_down_button && (psp_buttons & g_mapping.rs_down_button)) {
            *rsy = -RSTICK_MAGNITUDE;
            consume_psp_button(g_mapping.rs_down_button, button_flags);
            combo_active = 1;
        }
        if (g_mapping.rs_left_button && (psp_buttons & g_mapping.rs_left_button)) {
            *rsx = -RSTICK_MAGNITUDE;
            consume_psp_button(g_mapping.rs_left_button, button_flags);
            combo_active = 1;
        }
        if (g_mapping.rs_right_button && (psp_buttons & g_mapping.rs_right_button)) {
            *rsx = RSTICK_MAGNITUDE;
            consume_psp_button(g_mapping.rs_right_button, button_flags);
            combo_active = 1;
        }
    }

    if (g_mapping.l2_button && (psp_buttons & g_mapping.l2_button)) {
        *lt = 0xFF;
        consume_psp_button(g_mapping.l2_button, button_flags);
        combo_active = 1;
    }

    if (g_mapping.r2_button && (psp_buttons & g_mapping.r2_button)) {
        *rt = 0xFF;
        consume_psp_button(g_mapping.r2_button, button_flags);
        combo_active = 1;
    }

    if (g_mapping.l3_button && (psp_buttons & g_mapping.l3_button)) {
        *button_flags |= ML_LS_CLK;
        consume_psp_button(g_mapping.l3_button, button_flags);
        combo_active = 1;
    }

    if (g_mapping.r3_button && (psp_buttons & g_mapping.r3_button)) {
        *button_flags |= ML_RS_CLK;
        consume_psp_button(g_mapping.r3_button, button_flags);
        combo_active = 1;
    }

    if (combo_active) {
        consume_psp_button(modifier, button_flags);
    }
}

static uint16_t translate_buttons(uint32_t psp_buttons)
{
    uint16_t flags = 0;

    if (psp_buttons & PSP_CTRL_CROSS)    flags |= ML_A;
    if (psp_buttons & PSP_CTRL_CIRCLE)   flags |= ML_B;
    if (psp_buttons & PSP_CTRL_SQUARE)   flags |= ML_X;
    if (psp_buttons & PSP_CTRL_TRIANGLE) flags |= ML_Y;
    if (psp_buttons & PSP_CTRL_START)    flags |= ML_START;
    if (psp_buttons & PSP_CTRL_SELECT)   flags |= ML_BACK;
    if (psp_buttons & PSP_CTRL_LTRIGGER) flags |= ML_LB;
    if (psp_buttons & PSP_CTRL_RTRIGGER) flags |= ML_RB;
    if (psp_buttons & PSP_CTRL_UP)       flags |= ML_DPAD_UP;
    if (psp_buttons & PSP_CTRL_DOWN)     flags |= ML_DPAD_DOWN;
    if (psp_buttons & PSP_CTRL_LEFT)     flags |= ML_DPAD_LEFT;
    if (psp_buttons & PSP_CTRL_RIGHT)    flags |= ML_DPAD_RIGHT;

    return flags;
}

/* ------------------------------------------------------------------ *
 * Check whether the controller state has changed since last frame
 *
 * Compares buttons + analog stick values. Returns 1 if changed.
 * ------------------------------------------------------------------ */
static int state_changed(const SceCtrlData *cur)
{
    if (!g_prev_valid)
        return 1;   /* first frame — always send */

    if (cur->Buttons != g_prev.Buttons)
        return 1;

    /* Analog stick: require >3 LSB change to filter jitter noise.
     * PSP sticks have ±1-2 LSB jitter at rest which was triggering
     * 60 sends/sec even when untouched, flooding the WiFi buffer. */
    {
        int dx = (int)cur->Lx - (int)g_prev.Lx;
        int dy = (int)cur->Ly - (int)g_prev.Ly;
        if (dx < 0) dx = -dx;
        if (dy < 0) dy = -dy;
        if (dx > 3 || dy > 3)
            return 1;
    }

    return 0;
}

/* ------------------------------------------------------------------ *
 * Browser mode: mouse move packet (12 bytes, Gen5+ protocol)
 *
 *   [0..3] BE32 size = 8 (remaining bytes after size field)
 *   [4..7] LE32 magic = 0x00000007 (MOUSE_MOVE_REL_MAGIC_GEN5)
 *   [8..9] BE16 deltaX
 *  [10..11] BE16 deltaY
 * ------------------------------------------------------------------ */
static void send_mouse_move(int16_t dx, int16_t dy)
{
    uint8_t pkt[12];
    put_be32(&pkt[0], 8);
    put_le32(&pkt[4], MOUSE_MOVE_REL_MAGIC_GEN5);
    /* deltaX/Y are big-endian per moonlight-common-c */
    pkt[8]  = (uint8_t)((dx >> 8) & 0xFF);
    pkt[9]  = (uint8_t)(dx & 0xFF);
    pkt[10] = (uint8_t)((dy >> 8) & 0xFF);
    pkt[11] = (uint8_t)(dy & 0xFF);
    int ret = control_stream_send_input(pkt, sizeof(pkt), CTRL_CHANNEL_MOUSE);
    if (ret <= 0) {
        diag_log_write("INP", "Mouse move FAILED: ret=%d dx=%d dy=%d", ret, dx, dy);
    } else {
        static unsigned int s_mouse_move_log_count = 0;
        if ((s_mouse_move_log_count++ & 31u) == 0u) {
            diag_log_write("INP", "Mouse move: dx=%d dy=%d ret=%d", dx, dy, ret);
        }
    }
}

static int browser_mouse_delta(int axis)
{
    int sign = 1;
    int mag = axis;
    if (mag < 0) {
        sign = -1;
        mag = -mag;
    }

    if (mag <= MOUSE_DEADZONE)
        return 0;

    mag -= MOUSE_DEADZONE;

    int delta = (mag * MOUSE_BASE_SPEED) / MOUSE_AXIS_RANGE;
    delta += (mag * mag * MOUSE_ACCEL_SPEED) / (MOUSE_AXIS_RANGE * MOUSE_AXIS_RANGE);
    if (delta < 1)
        delta = 1;

    return sign * delta;
}

/* ------------------------------------------------------------------ *
 * Browser mode: mouse button packet (9 bytes, Gen5+ protocol)
 *
 *   [0..3] BE32 size = 5
 *   [4..7] LE32 magic = 0x08 (down) or 0x09 (up)
 *   [8]    u8   button (1=left, 3=right)
 * ------------------------------------------------------------------ */
static void send_mouse_button(int pressed, uint8_t button)
{
    uint8_t pkt[9];
    put_be32(&pkt[0], 5);
    put_le32(&pkt[4], pressed ? MOUSE_BUTTON_DOWN_MAGIC : MOUSE_BUTTON_UP_MAGIC);
    pkt[8] = button;
    int ret = control_stream_send_input(pkt, sizeof(pkt), CTRL_CHANNEL_MOUSE);
    if (ret <= 0) {
        diag_log_write("INP", "Mouse button FAILED: ret=%d pressed=%d btn=%d", ret, pressed, button);
    } else {
        diag_log_write("INP", "Mouse button: pressed=%d btn=%d ret=%d", pressed, button, ret);
    }
}

/* ── Phase 3.1: Controller Arrival Event ────────────────────────── */
static void send_controller_arrival(void)
{
    uint8_t pkt[16];
    memset(pkt, 0, sizeof(pkt));
    put_be32(&pkt[0], 12);                              /* size: remaining bytes */
    put_le32(&pkt[4], CONTROLLER_ARRIVAL_MAGIC);         /* magic 0x37 */
    pkt[8] = 0;                                          /* controllerNumber = 0 */
    pkt[9] = CONTROLLER_TYPE_XBOX;                       /* controllerType = Xbox */
    put_le32(&pkt[10], SUPPORTED_BUTTON_FLAGS);          /* supportedButtonFlags */
    put_le16(&pkt[14], CONTROLLER_CAP_ANALOG_TRIGGERS);  /* capabilities */
#ifdef RETAIL_BUILD
    control_stream_send_input(pkt, sizeof(pkt), CTRL_CHANNEL_GAMEPAD0);
#else
    int ret = control_stream_send_input(pkt, sizeof(pkt), CTRL_CHANNEL_GAMEPAD0);
    diag_log_write("INP", "Controller arrival sent: type=Xbox caps=0x%04X ret=%d",
                   CONTROLLER_CAP_ANALOG_TRIGGERS, ret);
#endif
}

/* ── Phase 3.2: Controller Battery Event ────────────────────────── */
static void send_controller_battery(void)
{
    int raw_charging = scePowerIsBatteryCharging();
    int charging = (raw_charging > 0) ? 1 : 0;
    int battery_exists = scePowerIsBatteryExist();
    int raw_percent = scePowerGetBatteryLifePercent();
    int percent = raw_percent;
    uint8_t state;

    if (battery_exists == 0) {
        state = BATTERY_STATE_NOT_PRESENT;
        percent = 0;
    } else if (percent < 0 || percent > 100) {
        state = g_last_battery_state;
        percent = g_last_battery_percent;
    } else if (charging) {
        state = (percent >= 100) ? BATTERY_STATE_FULL : BATTERY_STATE_CHARGING;
    } else {
        state = BATTERY_STATE_DISCHARGING;
    }
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    if (state != BATTERY_STATE_NOT_PRESENT && state != BATTERY_STATE_UNKNOWN) {
        g_last_battery_state = state;
        g_last_battery_percent = percent;
    }

    uint8_t pkt[11];
    memset(pkt, 0, sizeof(pkt));
    put_be32(&pkt[0], 7);                           /* size: remaining bytes */
    put_le32(&pkt[4], CONTROLLER_BATTERY_MAGIC);     /* magic 0x40 */
    pkt[8] = 0;                                      /* controllerNumber = 0 */
    pkt[9] = state;                                  /* batteryState */
    pkt[10] = (uint8_t)percent;                      /* batteryPercentage */

#ifdef RETAIL_BUILD
    control_stream_send_input(pkt, sizeof(pkt), CTRL_CHANNEL_GAMEPAD0);
#else
    int ret = control_stream_send_input(pkt, sizeof(pkt), CTRL_CHANNEL_GAMEPAD0);
    diag_log_write("INP", "Battery report: state=%d pct=%d charging=%d rawCharging=%d rawPct=%d exists=%d ret=%d",
                   state, percent, charging, raw_charging, raw_percent, battery_exists, ret);
#endif
}

/* ── Phase 3.3: Keyboard Event Sender ───────────────────────────── */
void input_send_keyboard_event(uint8_t key_action, uint16_t vk_code, uint8_t modifiers)
{
    uint8_t pkt[17];
    memset(pkt, 0, sizeof(pkt));
    put_be32(&pkt[0], 13);                   /* size: remaining bytes */
    put_le32(&pkt[4], KEYBOARD_MAGIC);       /* magic 0x05 */
    pkt[8] = key_action;                     /* KEY_ACTION_DOWN or _UP */
    /* pkt[9..11] = padding (0) */
    put_le16(&pkt[12], 0x0000);              /* reserved */
    put_le16(&pkt[14], vk_code);             /* Windows VK code */
    pkt[16] = modifiers;                     /* modifier bitmask */

#ifdef RETAIL_BUILD
    control_stream_send_input(pkt, sizeof(pkt), CTRL_CHANNEL_KEYBOARD);
#else
    int ret = control_stream_send_input(pkt, sizeof(pkt), CTRL_CHANNEL_KEYBOARD);
    diag_log_write("INP", "Keyboard event: action=%d vk=0x%04X mod=0x%02X ret=%d",
                   key_action, vk_code, modifiers, ret);
#endif
}

/* Send a complete key tap (down + up) for a VK code */
static void send_key_tap(uint16_t vk_code, uint8_t modifiers)
{
    input_send_keyboard_event(KEY_ACTION_DOWN, vk_code, modifiers);
    sceKernelDelayThread(5000); /* 5ms between down/up for server to register */
    input_send_keyboard_event(KEY_ACTION_UP, vk_code, modifiers);
}

/* ── Phase 3.4: Scroll Event Senders ────────────────────────────── */
void input_send_scroll(int16_t scroll_amount)
{
    uint8_t pkt[10];
    memset(pkt, 0, sizeof(pkt));
    put_be32(&pkt[0], 6);                   /* size: remaining bytes */
    put_le32(&pkt[4], SCROLL_MAGIC_GEN5);   /* magic 0x09 */
    pkt[8] = (uint8_t)((scroll_amount >> 8) & 0xFF);  /* BE16 scrollAmt high */
    pkt[9] = (uint8_t)(scroll_amount & 0xFF);          /* BE16 scrollAmt low */

    int ret = control_stream_send_input(pkt, sizeof(pkt), CTRL_CHANNEL_MOUSE);
    if (ret <= 0) {
        diag_log_write("INP", "Scroll FAILED: ret=%d amt=%d", ret, scroll_amount);
    }
}

void input_send_scroll_hires(int16_t scroll_amount_120ths)
{
    uint8_t pkt[10];
    memset(pkt, 0, sizeof(pkt));
    put_be32(&pkt[0], 6);                       /* size: remaining bytes */
    put_le32(&pkt[4], SCROLL_HIRES_MAGIC);       /* magic 0x33 */
    pkt[8] = (uint8_t)((scroll_amount_120ths >> 8) & 0xFF);
    pkt[9] = (uint8_t)(scroll_amount_120ths & 0xFF);

    int ret = control_stream_send_input(pkt, sizeof(pkt), CTRL_CHANNEL_MOUSE);
    if (ret <= 0) {
        diag_log_write("INP", "HiRes scroll FAILED: ret=%d amt=%d", ret, scroll_amount_120ths);
    }
}

/* ================================================================== *
 * Public API
 * ================================================================== */

/*
 * input_set_destination - No-op (input now routes through control stream)
 *
 * @host_ip: IPv4 address string (ignored, kept for API compatibility)
 */
void input_set_destination(const char *host_ip)
{
    (void)host_ip;
}

/*
 * input_init - Initialise input subsystem
 *
 * @sock: UDP socket (ignored — input now routes through control stream)
 *
 * Sets controller sampling to 60 Hz with analog mode, loads the button
 * mapper from map.cfg, and pre-fills the "previous state".
 */
void input_init(int sock)
{
    (void)sock;
    g_initialized = 1;
    g_prev_valid = 0;

    /* Load button combo mappings from map.cfg (defaults on first run) */
    load_mapping();

    /* Set controller sampling: 0 µs cycle = every VBlank = 60 Hz */
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);

    /* Pre-populate previous state with a zero-filled read */
    memset(&g_prev, 0, sizeof(g_prev));
    sceCtrlPeekBufferPositive(&g_prev, 1);
    g_prev_valid = 1;

    /* Phase 3.1: Send controller arrival event to tell server our type */
    send_controller_arrival();

    /* Phase 3.2: Send initial battery report */
    send_controller_battery();
    g_last_battery_report_us = sceKernelGetSystemTimeLow();

    diag_log_write("INP", "Input initialized: arrival+battery sent");
}

/*
 * input_poll_and_send - Poll controller and send through control stream
 *
 * Called once per frame (~60 Hz) from the main loop.
 * Xbox mode: gamepad packet with L+combo mapper (L2/R2/R-stick/L3/R3).
 * Browser mode: analog→mouse move, L=MB1, R=MB2, face buttons→gamepad.
 */
void input_poll_and_send(void)
{
    SceCtrlData pad;
    int browser_mode;
    int browser_dx = 0;
    int browser_dy = 0;
    int scroll_combo = 0;
    int browser_osk_combo = 0;
    int browser_hud_combo = 0;

    if (!g_initialized)
        return;

    /* Phase 3.2: Periodic battery report every 30 seconds */
    {
        SceUInt32 now_us = sceKernelGetSystemTimeLow();
        if ((now_us - g_last_battery_report_us) >= BATTERY_REPORT_INTERVAL_US) {
            send_controller_battery();
            g_last_battery_report_us = now_us;
        }
    }

    /* Read controller (non-blocking — peek, not read) */
    sceCtrlPeekBufferPositive(&pad, 1);

    /* Inject remote/automation button state (from pspsh pokew).
     * NOTE: Do NOT clear g_remote_buttons here — main.c clears it
     * after all consumers (including hud_handle_input) have read it. */
    {
        extern volatile unsigned int g_remote_buttons;
        pad.Buttons |= g_remote_buttons;
    }

    if (g_remote_analog_active) {
        unsigned int lx = g_remote_analog_lx;
        unsigned int ly = g_remote_analog_ly;
        if (lx > 255) lx = 255;
        if (ly > 255) ly = 255;
        pad.Lx = (unsigned char)lx;
        pad.Ly = (unsigned char)ly;
    }

    browser_mode = (g_psp_config.controlMode == CONTROL_MODE_BROWSER);
    {
        int stream_exit_combo = (pad.Buttons & PSP_CTRL_START) &&
                                (pad.Buttons & PSP_CTRL_SELECT);
        int hud_combo = (pad.Buttons & PSP_CTRL_RTRIGGER) &&
                        (pad.Buttons & PSP_CTRL_UP);
        if (stream_exit_combo || hud_combo) {
            if (!g_suppress_combo_prev) {
                diag_log_write("INP",
                               "App combo suppress host input: exit=%d hud=%d btn=0x%08X",
                               stream_exit_combo ? 1 : 0,
                               hud_combo ? 1 : 0,
                               pad.Buttons);
            }
            g_suppress_combo_prev = 1;
            return;
        }
        g_suppress_combo_prev = 0;
    }
    if (browser_mode) {
        scroll_combo = (pad.Buttons & PSP_CTRL_LTRIGGER) &&
                       (pad.Buttons & (PSP_CTRL_UP | PSP_CTRL_DOWN));
        browser_osk_combo = (pad.Buttons & PSP_CTRL_RTRIGGER) &&
                            (pad.Buttons & PSP_CTRL_TRIANGLE);
        browser_hud_combo = (pad.Buttons & PSP_CTRL_RTRIGGER) &&
                            (pad.Buttons & PSP_CTRL_UP);
        int ax = (int)pad.Lx - 128;
        int ay = (int)pad.Ly - 128;
        browser_dx = browser_mouse_delta(ax);
        browser_dy = browser_mouse_delta(ay);
    }

    /* Only transmit when state actually changed */
    if (!state_changed(&pad) && !(browser_mode && (browser_dx != 0 || browser_dy != 0)))
        return;

    /* Rate-limit analog input to ~30 Hz max (33ms) to avoid ENOBUFS.
     * PSP 802.11b WiFi has a tiny send buffer; at 60Hz input alone
     * floods it within 10 seconds.  30Hz halves worst-case analog
     * latency from 67ms to 33ms (G-1).
     * Button changes bypass the rate-limit for instant response. */
    {
        static SceUInt32 s_last_send_tick = 0;
        SceUInt32 now = sceKernelGetSystemTimeLow();
        int buttons_changed = (pad.Buttons != g_prev.Buttons);
        if (!buttons_changed && s_last_send_tick != 0 &&
            (now - s_last_send_tick) < 33333) {
            return;  /* analog-only change, throttled */
        }
        s_last_send_tick = now;
    }

    /* Log button transitions with timestamps for latency analysis */
    if (pad.Buttons != g_prev.Buttons) {
        diag_log_write("INP", "Button transition: 0x%08X -> 0x%08X (XOR: 0x%08X)",
                       g_prev.Buttons, pad.Buttons, pad.Buttons ^ g_prev.Buttons);
    }

    if (browser_mode) {
        /* ---- Browser Mode: analog stick → mouse, L/R → MB1/MB2 ---- */

        /* Phase 3.4: L+DpadUp/Down → scroll events in browser mode */
        if (pad.Buttons & PSP_CTRL_LTRIGGER) {
            if ((pad.Buttons & PSP_CTRL_UP) && !(g_prev.Buttons & PSP_CTRL_UP)) {
                input_send_scroll(120);  /* scroll up */
                diag_log_write("INP", "Browser scroll up");
            }
            if ((pad.Buttons & PSP_CTRL_DOWN) && !(g_prev.Buttons & PSP_CTRL_DOWN)) {
                input_send_scroll(-120);  /* scroll down */
                diag_log_write("INP", "Browser scroll down");
            }
        }

        /* Phase 3.3: R+Triangle → send Enter key (OSK placeholder).
         * Full PSP OSK (sceUtilityOskInit) requires blocking the render
         * loop which stalls the stream. Instead, send Enter as a quick
         * keyboard action for common desktop use (confirm dialogs, etc). */
        {
            int osk_now = browser_osk_combo;
            if (osk_now && !g_osk_trigger_prev) {
                send_key_tap(0x0D, 0);  /* VK_RETURN */
                diag_log_write("INP", "R+Triangle: sent Enter key");
            }
            g_osk_trigger_prev = osk_now;
        }

        /* Analog stick → relative mouse delta with deadzone */
        int dx = browser_dx;
        int dy = browser_dy;
        if (dx != 0 || dy != 0) {
            send_mouse_move((int16_t)dx, (int16_t)dy);
        }

        /* L trigger → left mouse button (edge-triggered) */
        int l_now = ((pad.Buttons & PSP_CTRL_LTRIGGER) && !scroll_combo) ? 1 : 0;
        if (l_now && !g_mouse_l_down) send_mouse_button(1, MOUSE_BUTTON_LEFT);
        else if (!l_now && g_mouse_l_down) send_mouse_button(0, MOUSE_BUTTON_LEFT);
        g_mouse_l_down = l_now;

        /* R trigger → right mouse button (edge-triggered) */
        int r_now = ((pad.Buttons & PSP_CTRL_RTRIGGER) &&
                     !browser_osk_combo && !browser_hud_combo) ? 1 : 0;
        if (r_now && !g_mouse_r_down) send_mouse_button(1, MOUSE_BUTTON_RIGHT);
        else if (!r_now && g_mouse_r_down) send_mouse_button(0, MOUSE_BUTTON_RIGHT);
        g_mouse_r_down = r_now;

        /* D-pad and face buttons send keyboard taps for browser navigation.
         * Only send when buttons changed — analog-only changes are handled by
         * mouse movement is handled separately above. */
        if (pad.Buttons != g_prev.Buttons) {
            unsigned int pressed = pad.Buttons & ~g_prev.Buttons;
            if (!scroll_combo && !browser_hud_combo && (pressed & PSP_CTRL_UP)) {
                send_key_tap(0x26, 0);  /* VK_UP */
                diag_log_write("INP", "Browser key: Up");
            }
            if (!scroll_combo && (pressed & PSP_CTRL_DOWN)) {
                send_key_tap(0x28, 0);  /* VK_DOWN */
                diag_log_write("INP", "Browser key: Down");
            }
            if (pressed & PSP_CTRL_LEFT) {
                send_key_tap(0x25, 0);  /* VK_LEFT */
                diag_log_write("INP", "Browser key: Left");
            }
            if (pressed & PSP_CTRL_RIGHT) {
                send_key_tap(0x27, 0);  /* VK_RIGHT */
                diag_log_write("INP", "Browser key: Right");
            }
            if (pressed & PSP_CTRL_CROSS) {
                send_key_tap(0x0D, 0);  /* VK_RETURN */
                diag_log_write("INP", "Browser key: Enter");
            }
            if (pressed & PSP_CTRL_CIRCLE) {
                send_key_tap(0x1B, 0);  /* VK_ESCAPE */
                diag_log_write("INP", "Browser key: Escape");
            }
            if (!browser_osk_combo && (pressed & PSP_CTRL_TRIANGLE)) {
                send_key_tap(0x09, 0);  /* VK_TAB */
                diag_log_write("INP", "Browser key: Tab");
            }
            if (pressed & PSP_CTRL_SQUARE) {
                send_key_tap(0x20, 0);  /* VK_SPACE */
                diag_log_write("INP", "Browser key: Space");
            }
            if (pressed & PSP_CTRL_START) {
                send_key_tap(0x0D, 0);  /* VK_RETURN */
                diag_log_write("INP", "Browser key: Start/Enter");
            }
            if (pressed & PSP_CTRL_SELECT) {
                send_key_tap(0x1B, 0);  /* VK_ESCAPE */
                diag_log_write("INP", "Browser key: Select/Escape");
            }
        }
    } else {
        /* ---- Xbox Mode: full gamepad with L+combo mapper ---- */

        /* Phase 3.3: R+Triangle → send Enter key in Xbox mode too */
        {
            int osk_now = (pad.Buttons & PSP_CTRL_RTRIGGER) &&
                          (pad.Buttons & PSP_CTRL_TRIANGLE);
            if (osk_now && !g_osk_trigger_prev) {
                send_key_tap(0x0D, 0);  /* VK_RETURN */
                diag_log_write("INP", "R+Triangle: sent Enter key (Xbox mode)");
            }
            g_osk_trigger_prev = osk_now;
        }

        uint16_t buttons = translate_buttons(pad.Buttons);
        int16_t lsx = map_analog(pad.Lx);
        int16_t lsy = -map_analog(pad.Ly);   /* invert Y */
        int16_t rsx, rsy;
        uint8_t lt, rt;

        /* Apply button mapper: L+combo → virtual L2/R2/right-stick/L3/R3 */
        apply_button_mapper_v2(pad.Buttons, &lsx, &lsy, &buttons, &lt, &rt, &rsx, &rsy);

        /* Assemble the 34-byte NV_MULTI_CONTROLLER_PACKET */
        uint8_t packet[34];
        build_packet(packet, buttons, lt, rt, lsx, lsy, rsx, rsy);

        /* Send through encrypted control stream (unsequenced) */
        {
            int ret = control_stream_send_input(packet, sizeof(packet), CTRL_CHANNEL_GAMEPAD0);
            if (ret <= 0) {
                diag_log_write("INP", "Send FAILED: ret=%d btn=0x%04X", ret, buttons);
            }
#ifndef RETAIL_BUILD
            else if (pad.Buttons != g_prev.Buttons) {
                diag_log_write("INP",
                               "GAMEPAD send: psp=0x%08X btn=0x%04X lt=%u rt=%u ls=%d,%d rs=%d,%d ret=%d",
                               pad.Buttons, buttons, lt, rt, lsx, lsy, rsx, rsy, ret);
            }
#endif
        }
    }

    /* Save current state for next-frame comparison */
    g_prev = pad;
}

void input_shutdown(void)
{
    g_initialized = 0;
}
