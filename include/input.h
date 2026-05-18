#ifndef INPUT_H
#define INPUT_H

#include <psptypes.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /* Raw file-format version. Missing/old files are migrated at load. */
    uint32_t version;
    /* Combo modifier button. Default: PSP_CTRL_LTRIGGER. */
    uint32_t modifier_button;
    /* Right stick source:
     *   0 = modifier + mapped buttons
     *   1 = modifier + analog nub
     */
    uint32_t right_stick_mode;
    /* Virtual L2 trigger (left analog trigger 0xFF) */
    uint32_t l2_button;      /* default: PSP_CTRL_LEFT */
    /* Virtual R2 trigger (right analog trigger 0xFF) */
    uint32_t r2_button;      /* default: PSP_CTRL_RIGHT */
    /* Right stick directions when right_stick_mode == 0 */
    uint32_t rs_up_button;   /* default: PSP_CTRL_TRIANGLE */
    uint32_t rs_down_button; /* default: PSP_CTRL_CROSS    */
    uint32_t rs_left_button; /* default: PSP_CTRL_SQUARE   */
    uint32_t rs_right_button;/* default: PSP_CTRL_CIRCLE   */
    /* L3 / R3 (stick clicks) */
    uint32_t l3_button;      /* default: PSP_CTRL_DOWN */
    uint32_t r3_button;      /* default: PSP_CTRL_UP   */
} ButtonMapping;

#define BUTTON_MAPPING_VERSION          2u
#define RIGHT_STICK_MODE_BUTTONS        0u
#define RIGHT_STICK_MODE_ANALOG_NUB     1u

void button_mapping_get(ButtonMapping *mapping);
void button_mapping_set(const ButtonMapping *mapping);

/* Phase 3: input_init now also sends controller arrival event */
void input_init(int sock);
void input_poll_and_send(void);
void input_shutdown(void);

/* Phase 3.1: Keyboard input via PSP OSK */
void input_send_keyboard_event(uint8_t key_action, uint16_t vk_code, uint8_t modifiers);

/* Phase 3.4: Scroll events */
void input_send_scroll(int16_t scroll_amount);
void input_send_scroll_hires(int16_t scroll_amount_120ths);

#ifdef __cplusplus
}
#endif

#endif /* INPUT_H */
