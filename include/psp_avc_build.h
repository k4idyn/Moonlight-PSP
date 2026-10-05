#ifndef MOONLIGHT_PSP_AVC_BUILD_H
#define MOONLIGHT_PSP_AVC_BUILD_H
/* Sony hardware AVC is the v1.5 decoder. */
#ifndef PSP_HARDWARE_AVC
#define PSP_HARDWARE_AVC 1
#endif
extern volatile int g_avc_last_input_accepted;
void display_avc_frame(void *frame);
/* Main/presenter thread only, before decoder shutdown after GPU completion. */
void display_avc_release_frame(void);
#endif
