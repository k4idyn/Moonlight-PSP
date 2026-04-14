#ifndef INPUT_H
#define INPUT_H

#include <psptypes.h>

#ifdef __cplusplus
extern "C" {
#endif

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

void button_mapping_get(ButtonMapping *mapping);
void button_mapping_set(const ButtonMapping *mapping);

#ifdef __cplusplus
}
#endif

#endif /* INPUT_H */
