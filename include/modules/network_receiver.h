/**
 * @file network_receiver.h
 * @brief Network receiver module for PSP Moonlight
 * 
 * Handles connection to Moonlight server using ENet and GameStream protocol.
 */

#ifndef NETWORK_RECEIVER_H
#define NETWORK_RECEIVER_H

#include <pspkernel.h>
#include <pspnet_inet.h>
#include <pspnet_resolver.h>


#ifdef __cplusplus
extern "C" {
#endif

/* Forward declaration */
typedef struct NetworkReceiver NetworkReceiver;

/* Special Key Constants (Win32 Virtual Key Codes) */
#define VK_TAB      0x09
#define VK_ESCAPE   0x1B
#define VK_SPACE    0x20
#define VK_DELETE   0x2E
#define VK_LWIN     0x5B
#define VK_RWIN     0x5C
#define VK_LCONTROL 0xA2
#define VK_RCONTROL 0xA3
#define VK_MENU     0x12
#define VK_LMENU    0xA4
#define VK_RMENU    0xA5


/* Create and destroy functions */
NetworkReceiver* network_receiver_create(void);
void network_receiver_destroy(NetworkReceiver* receiver);

/* Initialization and update */
int network_receiver_init(NetworkReceiver* receiver);
void network_receiver_update(NetworkReceiver* receiver);

/* Connection status */
int network_receiver_is_connected(NetworkReceiver* receiver);
void network_receiver_disconnect(NetworkReceiver* receiver);

/* Server connection */
int network_receiver_connect(NetworkReceiver* receiver, const char* host, int port, int width, int height, int host_width, int host_height, int fps, int bitrate);

/* Status and UI */
const char* network_receiver_get_status(NetworkReceiver* receiver);

/* App Selection */
int network_receiver_get_app_list(NetworkReceiver* receiver, char names[][64], int* ids, int max_apps);
void network_receiver_start_app(NetworkReceiver* receiver, int app_id);

/* Bind hardware decoders */
#include "video_decoder.h"
#include "audio_decoder.h"
void network_receiver_set_video_decoder(NetworkReceiver* receiver, VideoDecoder* video_decoder);
void network_receiver_set_audio_decoder(NetworkReceiver* receiver, AudioDecoder* audio_decoder);

/* Input reporting to host */
#include "input_mapper.h"
void network_receiver_send_input(NetworkReceiver* receiver, InputState* input_state);
void network_receiver_send_key(NetworkReceiver* receiver, short vkey, int down, char modifiers);
int network_receiver_quit_app(NetworkReceiver* receiver);
void network_receiver_send_ctrl_alt_del(NetworkReceiver* receiver);
int network_receiver_get_current_app_id(NetworkReceiver* receiver);

#ifdef __cplusplus
}
#endif

#endif /* NETWORK_RECEIVER_H */