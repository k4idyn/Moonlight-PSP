/**
 * @file input_mapper.c
 * @brief Input mapper module implementation for PSP Moonlight
 *
 * Reads PSP controls and sends to server.
 */

#include "input_mapper.h"
#include <pspctrl.h>
#include <pspkernel.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


/* Define the InputMapper structure */
struct InputMapper {
  int initialized;
  SceCtrlData ctrl_data; /* Current controller state */
};

/* Create and destroy functions */
InputMapper *input_mapper_create(void) {
  InputMapper *mapper = (InputMapper *)malloc(sizeof(InputMapper));
  if (!mapper) {
    return NULL;
  }

  memset(mapper, 0, sizeof(InputMapper));
  mapper->initialized = 0;
  memset(&mapper->ctrl_data, 0, sizeof(SceCtrlData));

  return mapper;
}

void input_mapper_destroy(InputMapper *mapper) {
  if (!mapper) {
    return;
  }

  free(mapper);
}

/* Initialization and update */
int input_mapper_init(InputMapper *mapper) {
  if (!mapper) {
    return -1;
  }

  /* Initialize the controller */
  sceCtrlSetSamplingCycle(0);
  sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);

  mapper->initialized = 1;
  return 0;
}

void input_mapper_update(InputMapper *mapper, SceCtrlData* pad_data) {
  if (!mapper || !mapper->initialized || !pad_data) {
    return;
  }

  /* Use the provided controller state */
  memcpy(&mapper->ctrl_data, pad_data, sizeof(SceCtrlData));
}

void input_mapper_send_special(InputMapper *mapper, int type) {
  (void)mapper;
  (void)type;
  /* This is a placeholder for sending special keys like Alt-Tab or Win via the network_receiver.
     In a real implementation, this might set a flag or call network_receiver_send_key directly. */
  // For now, we'll just log it
  // printf("Special input sent: %d\n", type);
}

/* Mapping helper: PSP buttons to GameStream standard */
unsigned int input_mapper_translate_buttons(unsigned int psp_buttons) {
  unsigned int gs_buttons = 0;

  if (psp_buttons & PSP_CTRL_TRIANGLE)
    gs_buttons |= (1 << 15); /* GS_BUTTON_Y */
  if (psp_buttons & PSP_CTRL_CIRCLE)
    gs_buttons |= (1 << 13); /* GS_BUTTON_B */
  if (psp_buttons & PSP_CTRL_CROSS)
    gs_buttons |= (1 << 12); /* GS_BUTTON_A */
  if (psp_buttons & PSP_CTRL_SQUARE)
    gs_buttons |= (1 << 14); /* GS_BUTTON_X */

  if (psp_buttons & PSP_CTRL_START)
    gs_buttons |= 0x0010; /* GS_BUTTON_PLAY */
  if (psp_buttons & PSP_CTRL_SELECT)
    gs_buttons |= 0x0020; /* GS_BUTTON_BACK */

  if (psp_buttons & PSP_CTRL_LTRIGGER)
    gs_buttons |= 0x0100; /* GS_BUTTON_LB */
  if (psp_buttons & PSP_CTRL_RTRIGGER)
    gs_buttons |= 0x0200; /* GS_BUTTON_RB */

  if (psp_buttons & PSP_CTRL_UP)
    gs_buttons |= 0x0001; /* GS_BUTTON_UP */
  if (psp_buttons & PSP_CTRL_DOWN)
    gs_buttons |= 0x0002; /* GS_BUTTON_DOWN */
  if (psp_buttons & PSP_CTRL_LEFT)
    gs_buttons |= 0x0004; /* GS_BUTTON_LEFT */
  if (psp_buttons & PSP_CTRL_RIGHT)
    gs_buttons |= 0x0008; /* GS_BUTTON_RIGHT */

  return gs_buttons;
}

/* Get current input state */
void input_mapper_get_state(InputMapper *mapper, InputState *state, ControlMode mode) {
  if (!mapper || !mapper->initialized || !state) {
    /* Return zero state */
    memset(state, 0, sizeof(InputState));
    return;
  }

  memset(state, 0, sizeof(InputState));
  state->buttons = mapper->ctrl_data.Buttons;
  
  /* Standard left analog mapping */
  signed char lx = (signed char)(mapper->ctrl_data.Lx - 128);
  signed char ly = (signed char)(mapper->ctrl_data.Ly - 128);
  
  /* Apply deadzone (approx 15% of 128) */
  if (lx > -20 && lx < 20) lx = 0;
  if (ly > -20 && ly < 20) ly = 0;

  if (mode == CONTROL_MODE_XBOX) {
    state->lx = lx;
    state->ly = ly;
    state->rx = 0;
    state->ry = 0;
  } 
  else if (mode == CONTROL_MODE_BROWSER) {
    /* Browser mode specific mapping - Absolute Perfection */
    /* Implementation of mouse acceleration for precise control + fast movement */
    float abs_lx = (lx < 0) ? -lx : lx;
    float abs_ly = (ly < 0) ? -ly : ly;
    
    /* Non-linear scaling: square the input for acceleration but with much higher divisor for PSP-size stick */
    /* Absolute Perfection: Divisor 80.0f provides smooth movement while 10.0f was way too fast. */
    state->mouse_dx = (short)((lx * abs_lx) / 80.0f); 
    state->mouse_dy = (short)((ly * abs_ly) / 80.0f);

    state->scroll_v = 0;

    /* Mouse buttons: Triggers are more natural for clicks */
    if (state->buttons & PSP_CTRL_RTRIGGER) state->mouse_buttons |= 0x01; /* LClick */
    if (state->buttons & PSP_CTRL_LTRIGGER) state->mouse_buttons |= 0x02; /* RClick */
    
    /* Dedicated Scroll: D-pad Up/Down (use 120 for standard tick) */
    if (state->buttons & PSP_CTRL_UP)   state->scroll_v = 120;
    if (state->buttons & PSP_CTRL_DOWN) state->scroll_v = -120;

    /* Clear game buttons that are hijacked for mouse */
    state->buttons &= ~(PSP_CTRL_LTRIGGER | PSP_CTRL_RTRIGGER | PSP_CTRL_UP | PSP_CTRL_DOWN);
    
    state->lx = 0;
    state->ly = 0;
    state->rx = 0;
    state->ry = 0;
  }
}