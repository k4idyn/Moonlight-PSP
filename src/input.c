/*
 * input.c - PSP button input polling and Moonlight controller transmission
 *
 * Polls the PSP controller at 60Hz using sceCtrlPeekBufferPositive,
 * maps PSP buttons and analog stick to Moonlight's controller format,
 * and sends NV_MULTI_CONTROLLER_PACKET (34 bytes) through the encrypted
 * ENet control stream (channel 0x10, type 0x0206).
 *
 * Button Mapper (PSP has no L2/R2 or right stick):
 *   L  + D-pad Up/Down/Left/Right  → Right Stick Up/Down/Left/Right
 *   L  + Cross                     → virtual L2 (left trigger  = 0xFF)
 *   L  + Square                    → virtual R2 (right trigger = 0xFF)
 */

#include <pspkernel.h>
#include <pspctrl.h>
#include <pspiofilemgr.h>
#include <string.h>
#include <stdio.h>

#include "shared.h"
#include "control_stream.h"
#include "diag_log.h"
#include "settings_menu.h"

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

/* Map config path on Memory Stick */
#define MAP_CFG_PATH        "ms0:/moonlight/map.cfg"

/* ------------------------------------------------------------------ *
 * ButtonMapping - loaded from map.cfg
 *
 * Combo modifier is always L-trigger (PSP_CTRL_LTRIGGER).
 * When L is held, the mapped psp_button activates the action.
 * ------------------------------------------------------------------ */
typedef struct {
    /* Virtual L2 trigger (left analog trigger 0xFF) */
    uint32_t l2_button;      /* default: PSP_CTRL_CROSS */
    /* Virtual R2 trigger (right analog trigger 0xFF) */
    uint32_t r2_button;      /* default: PSP_CTRL_SQUARE */
    /* Right stick directions */
    uint32_t rs_up_button;   /* default: PSP_CTRL_UP    */
    uint32_t rs_down_button; /* default: PSP_CTRL_DOWN  */
    uint32_t rs_left_button; /* default: PSP_CTRL_LEFT  */
    uint32_t rs_right_button;/* default: PSP_CTRL_RIGHT */
    /* L3 / R3 (stick clicks) */
    uint32_t l3_button;      /* default: PSP_CTRL_TRIANGLE (L+Tri=L3) */
    uint32_t r3_button;      /* default: PSP_CTRL_CIRCLE   (L+Cir=R3) */
} ButtonMapping;

/* ------------------------------------------------------------------ *
 * Browser-mode mouse packet constants (Gen5+ protocol)
 * ------------------------------------------------------------------ */
#define MOUSE_MOVE_REL_MAGIC_GEN5   0x00000007
#define MOUSE_BUTTON_DOWN_MAGIC     0x00000008
#define MOUSE_BUTTON_UP_MAGIC       0x00000009
#define MOUSE_BUTTON_LEFT           1
#define MOUSE_BUTTON_RIGHT          3
#define MOUSE_SENSITIVITY           6   /* analog-to-delta multiplier */

/* ENet channel assignments per Moonlight protocol (moonlight-common-c) */
#define CTRL_CHANNEL_KEYBOARD     0x02
#define CTRL_CHANNEL_MOUSE        0x03
#define CTRL_CHANNEL_GAMEPAD0     0x10  /* CTRL_CHANNEL_GAMEPAD_BASE + 0 */

/* ------------------------------------------------------------------ *
 * State tracking
 * ------------------------------------------------------------------ */
static int          g_initialized = 0;
static SceCtrlData  g_prev;      /* previous frame's controller state */
static int          g_prev_valid; /* 0 until first poll completes  */
static ButtonMapping g_mapping;  /* loaded combo mappings */

/* Access to global config for control mode */
extern PspConfig g_psp_config;

/* Browser-mode button-press memory for edge detection */
static int g_mouse_l_down = 0;
static int g_mouse_r_down = 0;

/* ------------------------------------------------------------------ *
 * map_cfg I/O
 * ------------------------------------------------------------------ */
static void set_default_mapping(void)
{
    g_mapping.l2_button      = PSP_CTRL_CROSS;
    g_mapping.r2_button      = PSP_CTRL_SQUARE;
    g_mapping.rs_up_button   = PSP_CTRL_UP;
    g_mapping.rs_down_button = PSP_CTRL_DOWN;
    g_mapping.rs_left_button = PSP_CTRL_LEFT;
    g_mapping.rs_right_button= PSP_CTRL_RIGHT;
    g_mapping.l3_button      = PSP_CTRL_TRIANGLE;
    g_mapping.r3_button      = PSP_CTRL_CIRCLE;
}

static void load_mapping(void)
{
    SceUID fd;
    set_default_mapping();

    fd = sceIoOpen(MAP_CFG_PATH, PSP_O_RDONLY, 0);
    if (fd < 0) return; /* no file — use defaults */

    /* File is a raw struct dump for simplicity */
    sceIoRead(fd, &g_mapping, sizeof(g_mapping));
    sceIoClose(fd);
}

void input_save_mapping(void)
{
    SceUID fd;
    sceIoMkdir("ms0:/moonlight", 0777);
    fd = sceIoOpen(MAP_CFG_PATH,
                   PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd < 0) return;
    sceIoWrite(fd, &g_mapping, sizeof(g_mapping));
    sceIoClose(fd);
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

/* ------------------------------------------------------------------ *
 * apply_button_mapper
 *
 * When L-trigger is held, intercept mapped buttons and produce
 * virtual L2/R2 triggers and right-stick axes.  Matched PSP buttons
 * are consumed (removed from button_flags) so they are not also sent
 * as LB or D-pad inputs.
 * ------------------------------------------------------------------ */
static void apply_button_mapper(uint32_t psp_buttons,
                                uint16_t *button_flags,
                                uint8_t  *lt, uint8_t  *rt,
                                int16_t  *rsx, int16_t *rsy)
{
    *lt  = 0;
    *rt  = 0;
    *rsx = 0;
    *rsy = 0;

    if (!(psp_buttons & PSP_CTRL_LTRIGGER))
        return; /* L not held — no combos active */

    /* L is held — remove LB from flags (it is now the modifier) */
    *button_flags &= ~ML_LB;

    /* Virtual L2 */
    if (psp_buttons & g_mapping.l2_button) {
        *lt = 0xFF;
        /* Consume the PSP button so it is not sent as a face button */
        if (g_mapping.l2_button == PSP_CTRL_CROSS)    *button_flags &= ~ML_A;
        if (g_mapping.l2_button == PSP_CTRL_CIRCLE)   *button_flags &= ~ML_B;
        if (g_mapping.l2_button == PSP_CTRL_SQUARE)   *button_flags &= ~ML_X;
        if (g_mapping.l2_button == PSP_CTRL_TRIANGLE) *button_flags &= ~ML_Y;
    }

    /* Virtual R2 */
    if (psp_buttons & g_mapping.r2_button) {
        *rt = 0xFF;
        if (g_mapping.r2_button == PSP_CTRL_CROSS)    *button_flags &= ~ML_A;
        if (g_mapping.r2_button == PSP_CTRL_CIRCLE)   *button_flags &= ~ML_B;
        if (g_mapping.r2_button == PSP_CTRL_SQUARE)   *button_flags &= ~ML_X;
        if (g_mapping.r2_button == PSP_CTRL_TRIANGLE) *button_flags &= ~ML_Y;
    }

    /* Right stick emulation via L+D-pad */
    if (psp_buttons & g_mapping.rs_up_button) {
        *rsy =  RSTICK_MAGNITUDE;
        *button_flags &= ~ML_DPAD_UP;
    }
    if (psp_buttons & g_mapping.rs_down_button) {
        *rsy = -RSTICK_MAGNITUDE;
        *button_flags &= ~ML_DPAD_DOWN;
    }
    if (psp_buttons & g_mapping.rs_left_button) {
        *rsx = -RSTICK_MAGNITUDE;
        *button_flags &= ~ML_DPAD_LEFT;
    }
    if (psp_buttons & g_mapping.rs_right_button) {
        *rsx =  RSTICK_MAGNITUDE;
        *button_flags &= ~ML_DPAD_RIGHT;
    }

    /* Virtual L3 (left stick click) via L+Triangle */
    if (psp_buttons & g_mapping.l3_button) {
        *button_flags |= ML_LS_CLK;
        if (g_mapping.l3_button == PSP_CTRL_CROSS)    *button_flags &= ~ML_A;
        if (g_mapping.l3_button == PSP_CTRL_CIRCLE)   *button_flags &= ~ML_B;
        if (g_mapping.l3_button == PSP_CTRL_SQUARE)   *button_flags &= ~ML_X;
        if (g_mapping.l3_button == PSP_CTRL_TRIANGLE) *button_flags &= ~ML_Y;
    }

    /* Virtual R3 (right stick click) via L+Circle */
    if (psp_buttons & g_mapping.r3_button) {
        *button_flags |= ML_RS_CLK;
        if (g_mapping.r3_button == PSP_CTRL_CROSS)    *button_flags &= ~ML_A;
        if (g_mapping.r3_button == PSP_CTRL_CIRCLE)   *button_flags &= ~ML_B;
        if (g_mapping.r3_button == PSP_CTRL_SQUARE)   *button_flags &= ~ML_X;
        if (g_mapping.r3_button == PSP_CTRL_TRIANGLE) *button_flags &= ~ML_Y;
    }
}


/* ------------------------------------------------------------------ *
 * Convert raw PSP button bitmask → Moonlight button flags
 * ------------------------------------------------------------------ */
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
    }
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

    if (!g_initialized)
        return;

    /* Read controller (non-blocking — peek, not read) */
    sceCtrlPeekBufferPositive(&pad, 1);

    /* Inject remote/automation button state (from pspsh pokew) */
    {
        extern volatile unsigned int g_remote_buttons;
        pad.Buttons |= g_remote_buttons;
        g_remote_buttons = 0;
    }

    /* Only transmit when state actually changed */
    if (!state_changed(&pad))
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

    if (g_psp_config.controlMode == CONTROL_MODE_BROWSER) {
        /* ---- Browser Mode: analog stick → mouse, L/R → MB1/MB2 ---- */

        /* Analog stick → relative mouse delta with deadzone */
        int ax = (int)pad.Lx - 128;
        int ay = (int)pad.Ly - 128;
        #define MOUSE_DEADZONE 20
        int dx = (ax > MOUSE_DEADZONE || ax < -MOUSE_DEADZONE) ? (ax * MOUSE_SENSITIVITY / 128) : 0;
        int dy = (ay > MOUSE_DEADZONE || ay < -MOUSE_DEADZONE) ? (ay * MOUSE_SENSITIVITY / 128) : 0;
        #undef MOUSE_DEADZONE
        if (dx != 0 || dy != 0) {
            send_mouse_move((int16_t)dx, (int16_t)dy);
        }

        /* L trigger → left mouse button (edge-triggered) */
        int l_now = (pad.Buttons & PSP_CTRL_LTRIGGER) ? 1 : 0;
        if (l_now && !g_mouse_l_down) send_mouse_button(1, MOUSE_BUTTON_LEFT);
        else if (!l_now && g_mouse_l_down) send_mouse_button(0, MOUSE_BUTTON_LEFT);
        g_mouse_l_down = l_now;

        /* R trigger → right mouse button (edge-triggered) */
        int r_now = (pad.Buttons & PSP_CTRL_RTRIGGER) ? 1 : 0;
        if (r_now && !g_mouse_r_down) send_mouse_button(1, MOUSE_BUTTON_RIGHT);
        else if (!r_now && g_mouse_r_down) send_mouse_button(0, MOUSE_BUTTON_RIGHT);
        g_mouse_r_down = r_now;

        /* D-pad and face buttons still send as gamepad for in-browser navigation.
         * Only send when buttons changed — analog-only changes are handled by
         * mouse move above.  Sending redundant gamepad packets on every analog
         * tick doubles WiFi traffic and can overflow the 802.11b send buffer. */
        if (pad.Buttons != g_prev.Buttons) {
            uint16_t buttons = translate_buttons(pad.Buttons);
            /* Strip L/R from gamepad since they're mouse buttons now */
            buttons &= ~(ML_LB | ML_RB);
            uint8_t packet[34];
            build_packet(packet, buttons, 0, 0,
                         0, 0,  /* no left stick in browser mode */
                         0, 0); /* no right stick */
            control_stream_send_input(packet, sizeof(packet), CTRL_CHANNEL_GAMEPAD0);
        }
    } else {
        /* ---- Xbox Mode: full gamepad with L+combo mapper ---- */
        uint16_t buttons = translate_buttons(pad.Buttons);
        int16_t lsx = map_analog(pad.Lx);
        int16_t lsy = -map_analog(pad.Ly);   /* invert Y */
        int16_t rsx, rsy;
        uint8_t lt, rt;

        /* Apply button mapper: L+combo → virtual L2/R2/right-stick/L3/R3 */
        apply_button_mapper(pad.Buttons, &buttons, &lt, &rt, &rsx, &rsy);

        /* Assemble the 34-byte NV_MULTI_CONTROLLER_PACKET */
        uint8_t packet[34];
        build_packet(packet, buttons, lt, rt, lsx, lsy, rsx, rsy);

        /* Send through encrypted control stream (unsequenced) */
        {
            int ret = control_stream_send_input(packet, sizeof(packet), CTRL_CHANNEL_GAMEPAD0);
            if (ret <= 0) {
                diag_log_write("INP", "Send FAILED: ret=%d btn=0x%04X", ret, buttons);
            }
        }
    }

    /* Save current state for next-frame comparison */
    g_prev = pad;
}

void input_shutdown(void)
{
    g_initialized = 0;
}