/**
 * @file input_mapper.h
 * @brief Input mapper module for PSP Moonlight
 * 
 * Reads PSP controls and sends to server.
 */

#ifndef INPUT_MAPPER_H
#define INPUT_MAPPER_H

#include <pspkernel.h>
#include <pspctrl.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declaration */
typedef struct InputMapper InputMapper;

typedef enum {
    CONTROL_MODE_XBOX = 0,
    CONTROL_MODE_BROWSER = 1
} ControlMode;

/* Create and destroy functions */
InputMapper* input_mapper_create(void);
void input_mapper_destroy(InputMapper* mapper);

/* Initialization and update */
int input_mapper_init(InputMapper* mapper);
void input_mapper_update(InputMapper* mapper, SceCtrlData* pad_data);
void input_mapper_send_special(InputMapper* mapper, int type);

/* Input state */
typedef struct {
    unsigned short buttons;   /* PSP button mask */
    signed char lx;           /* Left analog X (-128 to 127) */
    signed char ly;           /* Left analog Y (-128 to 127) */
    signed char rx;           /* Right analog X (-128 to 127) */
    signed char ry;           /* Right analog Y (-128 to 127) */
    
    /* Browser mode specific */
    short mouse_dx;
    short mouse_dy;
    signed char scroll_v;
    unsigned char mouse_buttons; /* 1:L, 2:R, 4:M */
} InputState;

/* Get current input state */
void input_mapper_get_state(InputMapper* mapper, InputState* state, ControlMode mode);

/* Mapping helper: PSP buttons to GameStream standard */
unsigned int input_mapper_translate_buttons(unsigned int psp_buttons);

#ifdef __cplusplus
}
#endif

#endif /* INPUT_MAPPER_H */