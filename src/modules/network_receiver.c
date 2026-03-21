#include <pspkernel.h>
#include <pspiofilemgr.h>
#include <pspiofilemgr_fcntl.h>
#include <pspdebug.h>
#include <pspwlan.h>
#include <psputility.h>
#include <pspdisplay.h>
#include <pspgu.h>
#include <pspgum.h>
#include <pspnet.h>
#include "ui_renderer.h"
#include <enet/enet.h>
#include <pspnet_inet.h>
#include <pspnet_apctl.h>
#include <pspnet_resolver.h>
#include <psputility.h>
#include <psputility_netparam.h>
#include <psputility_modules.h>

#include <pspsdk.h>
#include <pspctrl.h>
#include <string.h>
#include <malloc.h>
#include <stdint.h>
#include "network_receiver.h"
#include "video_decoder.h"
#include "audio_decoder.h"
#include "logger.h"
#include "exception_handler.h"
#include <Limelight.h>
#include "../libgamestream/client.h"
#include "../libgamestream/errors.h"

struct NetworkReceiver {
  int connected; // 0=disconnected, 1=connecting, 2=connected, -1=failed
  char server_host[64];
  int server_port;
  SceUID streaming_thread;
  char status_msg[128];
  
  int width;
  int height;
  int fps;
  int bitrate;
  int host_width;
  int host_height;
  SceUID connection_thread;
  int app_ids[64];
  char app_names[64][64];
  int app_count;
  int selected_app_id;
  volatile int start_app_now;
  int current_live_app_id;
  uint64_t last_app_refresh_time;

  
  VideoDecoder* video_decoder;
  AudioDecoder* audio_decoder;
  
  /* Connection Metadata for the worker thread */
  SERVER_INFORMATION li_server_info;
  STREAM_CONFIGURATION li_stream_config;
  CONNECTION_LISTENER_CALLBACKS li_cl_callbacks;
  DECODER_RENDERER_CALLBACKS li_dr_callbacks;
  AUDIO_RENDERER_CALLBACKS li_ar_callbacks;
  SERVER_DATA gs_server_data;

  /* Absolute Perfection: Performance monitoring without I/O stalls */
  unsigned int total_bytes_received;
  unsigned long long last_stats_time;
  
  /* Input State Tracking */
  unsigned char last_mouse_buttons;
};

static NetworkReceiver* g_current_receiver = NULL;
char g_local_ip[16] = "0.0.0.0";
/* Absolute Perfection: avoid repeated exception triggers within one run. */
static volatile int g_rtsp_timeout_exception_triggered = 0;

/* Forward declarations of helper functions */
static int init_psp_networking(void);
static int streaming_thread_func(SceSize args, void *argp);
static int ConnectionThread(SceSize args, void *argp);

NetworkReceiver *network_receiver_create(void) {
  NetworkReceiver *receiver = (NetworkReceiver *)malloc(sizeof(NetworkReceiver));
  if (!receiver) return NULL;
  memset(receiver, 0, sizeof(NetworkReceiver));
  receiver->streaming_thread = -1;
  receiver->connection_thread = -1; // Initialize new thread ID
  return receiver;
}

void network_receiver_destroy(NetworkReceiver *receiver) {
  if (!receiver) return;
  network_receiver_disconnect(receiver);
  free(receiver);
}

int network_receiver_init(NetworkReceiver *receiver) {
  if (!receiver) return -1;
  if (init_psp_networking() < 0) {
    LOG_ERROR(COMPONENT_NETWORK, "Failed to initialize PSP networking");
    return -1;
  }
  return 0;
}

void network_receiver_update(NetworkReceiver *receiver) {
  (void)receiver;
  // handled by threads
}

int network_receiver_is_connected(NetworkReceiver *receiver) {
  if (!receiver) return 0;
  return receiver->connected;
}

void network_receiver_disconnect(NetworkReceiver *receiver) {
  if (!receiver || receiver->connected == 0) return;
  
  LOG_INFO(COMPONENT_NETWORK, "Stopping connection...");
  LiStopConnection();
  
  if (receiver->streaming_thread >= 0) {
    LOG_INFO(COMPONENT_NETWORK, "Waiting for streaming thread to exit (500ms timeout)...");
    
    /* Wait with a timeout to prevent hanging the whole system if a socket is blocked */
    /* sceKernelWaitThreadEnd doesn't have a timeout, so we poll or use a shorter wait if possible */
    /* Since we want to ensure OS exit doesn't hang, we'll wait a bit then force terminate if needed */
    int timeout = 50; // 50 * 10ms = 500ms
    int exited = 0;
    while (timeout > 0) {
        if (sceKernelGetThreadExitStatus(receiver->streaming_thread) != 0) { // Thread still alive (approx check)
            /* Non-blocking check or small wait */
            SceUInt timeout_val = 10000; // 10ms
            if (sceKernelWaitThreadEnd(receiver->streaming_thread, &timeout_val) == 0) {
                exited = 1;
                break;
            }
        } else {
            exited = 1;
            break;
        }
        timeout--;
    }

    if (!exited) {
        LOG_ERROR(COMPONENT_NETWORK, "Streaming thread failed to exit gracefully. Forcing termination.");
        sceKernelTerminateThread(receiver->streaming_thread);
    }
    
    sceKernelDeleteThread(receiver->streaming_thread);
    receiver->streaming_thread = -1;
    LOG_INFO(COMPONENT_NETWORK, "Streaming thread cleaned up.");
  }

  if (receiver->connection_thread >= 0) {
      LOG_INFO(COMPONENT_NETWORK, "Waiting for connection thread to exit (500ms timeout)...");
      int timeout = 50; // 50 * 10ms = 500ms
      int exited = 0;
      while (timeout > 0) {
          if (sceKernelGetThreadExitStatus(receiver->connection_thread) != 0) {
              SceUInt timeout_val = 10000; // 10ms
              if (sceKernelWaitThreadEnd(receiver->connection_thread, &timeout_val) == 0) {
                  exited = 1;
                  break;
              }
          } else {
              exited = 1;
              break;
          }
          timeout--;
      }

      if (!exited) {
          LOG_ERROR(COMPONENT_NETWORK, "Connection thread failed to exit gracefully. Forcing termination.");
          sceKernelTerminateThread(receiver->connection_thread);
      }

      sceKernelDeleteThread(receiver->connection_thread);
      receiver->connection_thread = -1;
      LOG_INFO(COMPONENT_NETWORK, "Connection thread cleaned up.");
  }
  
  receiver->connected = 0;
  receiver->start_app_now = 0;
  receiver->app_count = 0;
}

static int ConnectionThread(SceSize args, void *argp) {
    (void)args;
    NetworkReceiver* receiver = *(NetworkReceiver**)argp;
    if (!receiver) return -1;
    
    LOG_INFO(COMPONENT_NETWORK, "ConnectionThread: Starting Limelight connection...");
    
    if (receiver->li_server_info.rtspSessionUrl == NULL) {
        LOG_ERROR(COMPONENT_NETWORK, "ConnectionThread: CRITICAL - RTSP Session URL is NULL! Aborting.");
        receiver->connected = -1;
        return -1;
    }
    
    /* Absolute Perfection: Use higher priority for the network thread to prevent jitter */
    sceKernelChangeThreadPriority(sceKernelGetThreadId(), 0x10);

    /* Absolute Perfection: Ensure the receiver pointer remains globally valid for callbacks */
    g_current_receiver = receiver;
    
    int li_err = LiStartConnection(&receiver->li_server_info, &receiver->li_stream_config, 
                                   &receiver->li_cl_callbacks, &receiver->li_dr_callbacks, 
                                   &receiver->li_ar_callbacks, receiver, 0, receiver, 0);
    
    if (li_err == 0) {
        LOG_INFO(COMPONENT_NETWORK, "Streaming active. Thread monitored until disconnect...");
        while (receiver->connected > 0) {
            sceKernelDelayThread(100000); /* 100ms poll */
        }
        /* Absolute Perfection: Do not double-free!
           LiStopConnection is handled exactly once by network_receiver_disconnect() */
    } else {
        LOG_ERROR(COMPONENT_NETWORK, "LiStartConnection failed: %d", li_err);
        receiver->connected = -1;
    }
    
    gs_quit_app(&receiver->gs_server_data);
    gs_cleanup(&receiver->gs_server_data);
    receiver->connection_thread = -1;
    return 0;
}

int network_receiver_connect(NetworkReceiver *receiver, const char *host,
                             int port, int width, int height, int host_width, int host_height, int fps, int bitrate) {
  if (!receiver || !host) {
    return -1;
  }
  
  /* Store connection details */
  strncpy(receiver->server_host, host, sizeof(receiver->server_host) - 1);
  receiver->server_host[sizeof(receiver->server_host) - 1] = '\0';
  receiver->server_port = port;
  receiver->width = width;
  receiver->height = height;
  receiver->fps = fps;
  receiver->bitrate = bitrate;
  receiver->host_width = host_width;
  receiver->host_height = host_height;

  LOG_INFO(COMPONENT_NETWORK, "network_receiver_connect %s:%d", host, port);

  /* Start the streaming thread */
  receiver->connected = 1;
  strcpy(receiver->status_msg, "Starting connection thread...");
  /* Start the streaming thread with a lower priority (0x40) so it doesn't starve the UI thread (0x20).
   * Increased stack to 0x40000 (256KB) to safely handle extremely deep mbedTLS / RTSP call stacks (Stability Secret). */
  receiver->streaming_thread = sceKernelCreateThread("connect_worker", streaming_thread_func, 0x40, 0x40000, 0, NULL);
  if (receiver->streaming_thread < 0) {
    receiver->connected = -1;
    strcpy(receiver->status_msg, "Failed to start thread");
    LOG_ERROR(COMPONENT_NETWORK, "sceKernelCreateThread FAILED: 0x%08X", receiver->streaming_thread);
    return -1;
  }
  
  sceKernelStartThread(receiver->streaming_thread, sizeof(NetworkReceiver *), &receiver);
  return 0;
}

const char* network_receiver_get_status(NetworkReceiver* receiver) {
    if (!receiver) return "";
    
    /* Absolute Perfection: Show GameStream-level status (like PIN/Countdown) during connecting phase */
    if (gs_error && gs_error[0] != '\0') {
        if (receiver->connected == -1 || strstr(gs_error, "approval") != NULL || strstr(gs_error, "PIN") != NULL) {
            static char err_buf[256];
            snprintf(err_buf, sizeof(err_buf), "%s%s", (receiver->connected == -1 ? "FAILED: " : ""), gs_error);
            return err_buf;
        }
    }
    
    return receiver->status_msg;
}

// Callbacks

static void cl_stage_starting(int stage) { 
    LOG_INFO(COMPONENT_LIMELIGHT, "Starting stage: %s", LiGetStageName(stage));
    if (g_current_receiver) {
        snprintf(g_current_receiver->status_msg, sizeof(g_current_receiver->status_msg), "%s", LiGetStageName(stage));
    }
}
static void cl_stage_complete(int stage) { 
    LOG_INFO(COMPONENT_LIMELIGHT, "Completed stage: %s", LiGetStageName(stage));
}
static void cl_message(const char* message) {
    LOG_INFO(COMPONENT_LIMELIGHT, "Sunshine Message: %s", message);
    if (g_current_receiver) {
        snprintf(g_current_receiver->status_msg, sizeof(g_current_receiver->status_msg), "MSG: %s", message);
    }
}
static void cl_stage_failed(int stage, int errorCode) { 
    LOG_ERROR(COMPONENT_LIMELIGHT, "Failed stage: %s (0x%08X)", LiGetStageName(stage), errorCode);
    if (g_current_receiver) {
        if (gs_error && gs_error[0] != '\0') {
            snprintf(g_current_receiver->status_msg, sizeof(g_current_receiver->status_msg),
                     "FAILED: %s", gs_error);
        } else {
            snprintf(g_current_receiver->status_msg, sizeof(g_current_receiver->status_msg),
                     "FAILED: %s (0x%08X)", LiGetStageName(stage), errorCode);
        }
    }

    /* Soft-fail coverage:
     * RTSP handshake failures (like OPTIONS timeouts) often don't "crash" the PSP,
     * but they do prevent streaming_active.flag from being created. Trigger the
     * same exception-log mechanism used for hard crashes so the perfection loop
     * can immediately react and rebuild/restart instead of waiting for its
     * long streaming_active timeout. */
    const char* stageName = LiGetStageName(stage);
    if (stageName && strstr(stageName, "RTSP") != NULL) {
        LOG_ERROR(COMPONENT_LIMELIGHT, "RTSP Stage Failed (Graceful Exit): %s", stageName);
        /* Return to main menu state instead of crashing */
    }
}
static void cl_connection_started(void) {
    LOG_INFO(COMPONENT_NETWORK, "CB_ENTER: cl_connection_started");
    if (g_current_receiver) {
        strcpy(g_current_receiver->status_msg, "Connected! Streaming...");
        /* Handshake complete! Safe to start video/gu now. */
        g_current_receiver->connected = 2;
        
        /* Signal the testing loop that active streaming has begun */
        SceUID fd = sceIoOpen("ms0:/streaming_active.flag", PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
        if (fd >= 0) {
            sceIoWrite(fd, "ACTIVE", 6);
            sceIoClose(fd);
        }
    }
    LOG_INFO(COMPONENT_NETWORK, "CB_EXIT: cl_connection_started (connected=2)");
}
static void cl_connection_terminated(int errorCode) {
    LOG_INFO(COMPONENT_NETWORK, "Connection terminated (0x%08X)", errorCode);
    if (g_current_receiver) {
        snprintf(g_current_receiver->status_msg, sizeof(g_current_receiver->status_msg),
                 "Disconnected (0x%08X)", errorCode);
        g_current_receiver->connected = -1;
    }
}

#include <stdarg.h>
static SceUID log_file = -1;
static SceUID log_mutex = -1;

void network_receiver_init_logger() {
    if (log_mutex < 0) {
        log_mutex = sceKernelCreateSema("LimelogMutex", 0, 1, 1, NULL);
    }
    if (log_file < 0) {
        log_file = sceIoOpen("moonlight_debug.log", PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    }
}
static void cl_log_message(const char* format, ...) {
    char buffer[2048];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    
    LOG_INFO(COMPONENT_LIMELIGHT, "%s", buffer);

    /* Absolute Perfection diagnostic:
     * RTSP OPTIONS timeouts often don't produce a "hard crash" on PSP, but they
     * still must be treated as failures. We'll log them and return to menu 
     * instead of freezing the system with an exception. */
    if (!g_rtsp_timeout_exception_triggered) {
        if (strstr(buffer, "RTSP request timed out") != NULL) {
            g_rtsp_timeout_exception_triggered = 1;
            LOG_ERROR(COMPONENT_LIMELIGHT, "RTSP request timed out (soft-fail)");
        } else if (strstr(buffer, "RTSP OPTIONS attempt") != NULL &&
                   strstr(buffer, "failed") != NULL) {
            g_rtsp_timeout_exception_triggered = 1;
            LOG_ERROR(COMPONENT_LIMELIGHT, "RTSP OPTIONS attempt failed (soft-fail)");
        }
    }
}
static void cl_connection_status_update(int connectionStatus) { 
    LOG_INFO(COMPONENT_NETWORK, "Connection status update (0x%08X)", connectionStatus);
}
static void cl_rumble(unsigned short controllerNumber, unsigned short lowFreqMotor, unsigned short highFreqMotor) { (void)controllerNumber; (void)lowFreqMotor; (void)highFreqMotor; }
static void cl_set_hdr_mode(bool hdrEnabled) { (void)hdrEnabled; }
static void cl_rumble_triggers(uint16_t controllerNumber, uint16_t leftTriggerMotor, uint16_t rightTriggerMotor) { (void)controllerNumber; (void)leftTriggerMotor; (void)rightTriggerMotor; }
static void cl_set_motion_event_state(uint16_t controllerNumber, uint8_t motionType, uint16_t reportRateHz) { (void)controllerNumber; (void)motionType; (void)reportRateHz; }
static void cl_set_controller_led(uint16_t controllerNumber, uint8_t r, uint8_t g, uint8_t b) { (void)controllerNumber; (void)r; (void)g; (void)b; }
static void cl_set_adaptive_triggers(uint16_t controllerNumber, uint8_t eventFlags, uint8_t typeLeft, uint8_t typeRight, uint8_t *left, uint8_t *right) { (void)controllerNumber; (void)eventFlags; (void)typeLeft; (void)typeRight; (void)left; (void)right; }


static int dr_setup(int videoFormat, int width, int height, int redrawRate, void* context, int drFlags) {
    (void)context;
    LOG_INFO(COMPONENT_VIDEO, "dr_setup: fmt=%d %dx%d fps=%d flags=%d", videoFormat, width, height, redrawRate, drFlags);
    return 0;
}


/* Legacy dr_submit_decode_unit removed to prevent I/O stalls on hot path.
   Using CAPABILITY_PULL_RENDERER instead. */


static int ar_init(int audioConfiguration, const POPUS_MULTISTREAM_CONFIGURATION opusConfig, void* context, int arFlags) { 
    (void)audioConfiguration; (void)opusConfig; (void)context; (void)arFlags;
    return 0; 
}
static void ar_start(void) { }
static void ar_stop(void) { }
static void ar_cleanup(void) { }
static void ar_decode_and_play_sample(char* sampleData, int sampleLength) {
    if (g_current_receiver && g_current_receiver->audio_decoder) {
        if (sampleData && sampleLength > 0) {
            audio_decoder_submit_packet(g_current_receiver->audio_decoder, (unsigned char*)sampleData, sampleLength);
        }
    }
}

static int streaming_thread_func(SceSize args, void *argp) {
  (void)args;
  NetworkReceiver *receiver = *(NetworkReceiver **)argp;
  if (!receiver) return -1;

  LiInitializeConnectionCallbacks(&receiver->li_cl_callbacks);
  receiver->li_cl_callbacks.stageStarting = cl_stage_starting;
  receiver->li_cl_callbacks.stageComplete = cl_stage_complete;
  receiver->li_cl_callbacks.stageFailed = cl_stage_failed;
  receiver->li_cl_callbacks.connectionStarted = cl_connection_started;
  receiver->li_cl_callbacks.connectionTerminated = cl_connection_terminated;
  receiver->li_cl_callbacks.logMessage = cl_log_message;
  receiver->li_cl_callbacks.connectionStatusUpdate = cl_connection_status_update;
  receiver->li_cl_callbacks.rumble = cl_rumble;
  receiver->li_cl_callbacks.setHdrMode = cl_set_hdr_mode;
  receiver->li_cl_callbacks.rumbleTriggers = cl_rumble_triggers;
  receiver->li_cl_callbacks.setMotionEventState = cl_set_motion_event_state;
  receiver->li_cl_callbacks.setControllerLED = cl_set_controller_led;
  receiver->li_cl_callbacks.setAdaptiveTriggers = cl_set_adaptive_triggers;
  receiver->li_cl_callbacks.clMessage = cl_message;

  
  LiInitializeVideoCallbacks(&receiver->li_dr_callbacks);
  receiver->li_dr_callbacks.setup = dr_setup;
  /* Absolute Perfection: Use Pull Model to avoid synchronous I/O stalls in network thread */
  receiver->li_dr_callbacks.capabilities = CAPABILITY_PULL_RENDERER;
  receiver->li_dr_callbacks.submitDecodeUnit = NULL;
  
  LiInitializeAudioCallbacks(&receiver->li_ar_callbacks);
  receiver->li_ar_callbacks.init = ar_init;
  receiver->li_ar_callbacks.start = ar_start;
  receiver->li_ar_callbacks.stop = ar_stop;
  receiver->li_ar_callbacks.cleanup = ar_cleanup;
  receiver->li_ar_callbacks.decodeAndPlaySample = ar_decode_and_play_sample;
  receiver->li_ar_callbacks.capabilities = CAPABILITY_DIRECT_SUBMIT;
  
  LiInitializeServerInformation(&receiver->li_server_info);
  receiver->li_server_info.address = receiver->server_host;
  receiver->li_server_info.serverInfoAppVersion = "7.1.431.0";
  receiver->li_server_info.serverCodecModeSupport = SCM_H264;
  
  LiInitializeStreamConfiguration(&receiver->li_stream_config);
  receiver->li_stream_config.width = receiver->width ? receiver->width : 480;
  receiver->li_stream_config.height = receiver->height ? receiver->height : 272;
  receiver->li_stream_config.fps = receiver->fps ? receiver->fps : 30;
  receiver->li_stream_config.bitrate = receiver->bitrate ? receiver->bitrate : 1000;
  receiver->li_stream_config.packetSize = 1024;
  receiver->li_stream_config.streamingRemotely = STREAM_CFG_LOCAL;
  receiver->li_stream_config.audioConfiguration = AUDIO_CONFIGURATION_STEREO;
  receiver->li_stream_config.supportedVideoFormats = VIDEO_FORMAT_H264;
  receiver->li_stream_config.encryptionFlags = 0; // PSP: Disabled AES-GCM control encryption to avoid mbedtls Ext crash
  
  if (receiver->video_decoder) {
  video_decoder_set_stream_resolution(receiver->video_decoder, receiver->li_stream_config.width, receiver->li_stream_config.height);
  }

  LOG_INFO(COMPONENT_NETWORK, "Attempting gs_init to %s:47989...", receiver->server_host);
  strcpy(receiver->status_msg, "Initializing gamestream...");
  int gs_err = gs_init(&receiver->gs_server_data, receiver->server_host, 47989, "ms0:/moonlight/keys", 14, true);
  if (gs_err != GS_OK) {
    LOG_ERROR(COMPONENT_NETWORK, "Failed to initialize GameStream client (err: %d)", gs_err);
    snprintf(receiver->status_msg, sizeof(receiver->status_msg), "Init error: %d", gs_err);
    receiver->connected = -1;
    return -1;
  }
  LOG_INFO(COMPONENT_NETWORK, "gs_init successful. Paired: %d", receiver->gs_server_data.paired);
  
  /* [B21] Extra delay to let TCP stack settle before SSL handshake */
  sceKernelDelayThread(200000); 

  if (!receiver->gs_server_data.paired) {
      char pin[5];
      memset(pin, 0, sizeof(pin));
      SceUID pin_fd = sceIoOpen("ms0:/moonlight/keys/pin.dat", PSP_O_RDONLY, 0777);
      if (pin_fd >= 0) {
          int bytesRead = sceIoRead(pin_fd, pin, 4);
          if (bytesRead == 4) {
              pin[4] = '\0';
          } else {
              pin[0] = '\0';
          }
          sceIoClose(pin_fd);
      }
      
      if (strlen(pin) != 4) {
          int random_pin = 1000 + (sceKernelGetSystemTimeLow() % 9000);
          snprintf(pin, sizeof(pin), "%04d", random_pin);
          pin_fd = sceIoOpen("ms0:/moonlight/keys/pin.dat", PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
          if (pin_fd >= 0) {
              sceIoWrite(pin_fd, pin, 4);
              sceIoClose(pin_fd);
          }
      }

      LOG_INFO(COMPONENT_NETWORK, "Pairing required. PIN: %s", pin);
      snprintf(receiver->status_msg, sizeof(receiver->status_msg), "Enter PIN %s on PC.", pin);
      gs_err = gs_pair(&receiver->gs_server_data, pin);
      if (gs_err != 0) {
          LOG_ERROR(COMPONENT_NETWORK, "gs_pair failed: %d", gs_err);
          snprintf(receiver->status_msg, sizeof(receiver->status_msg), "Pairing failed: %d", gs_err);
          receiver->connected = -1;
          gs_cleanup(&receiver->gs_server_data);
          return -1;
      }
  }

  PAPP_LIST appList = NULL;
  gs_err = gs_applist(&receiver->gs_server_data, &appList);
  if (gs_err != 0 || !appList) {
      LOG_ERROR(COMPONENT_NETWORK, "gs_applist failed: %d", gs_err);
      receiver->connected = -1;
      gs_cleanup(&receiver->gs_server_data);
      return -1;
  }
  
  receiver->app_count = 0;
  PAPP_LIST curr_app = appList;
  while(curr_app && receiver->app_count < 64) {
      strncpy(receiver->app_names[receiver->app_count], curr_app->name, 63);
      receiver->app_ids[receiver->app_count] = curr_app->id;
      receiver->app_count++;
      curr_app = curr_app->next;
  }
  gs_free_applist(appList);

  receiver->connected = 3; /* Pending App Select */
  while (receiver->connected == 3 && receiver->start_app_now == 0) {
      sceKernelDelayThread(100000);
  }
  
  if (receiver->connected <= 0) {
      gs_cleanup(&receiver->gs_server_data);
      return 0;
  }

  receiver->connected = 1;
  int appId = receiver->selected_app_id;
  int current_live = network_receiver_get_current_app_id(receiver);
  LOG_INFO(COMPONENT_NETWORK, "Starting app ID: %d (Current live on host: %d)...", appId, current_live);

  STREAM_CONFIGURATION hostConfig = receiver->li_stream_config;
  if (receiver->host_width > 0 && receiver->host_height > 0) {
      hostConfig.width = receiver->host_width;
      hostConfig.height = receiver->host_height;
  }

  gs_err = gs_start_app(&receiver->gs_server_data, &hostConfig, appId, false, false, 1);
  if (gs_err != 0) {
      LOG_ERROR(COMPONENT_NETWORK, "gs_start_app failed: %d", gs_err);
      if (gs_error && gs_error[0] != '\0') {
          snprintf(receiver->status_msg, sizeof(receiver->status_msg), "FAILED: %s", gs_error);
      } else {
          snprintf(receiver->status_msg, sizeof(receiver->status_msg), "FAILED: Start App (%d)", gs_err);
      }
      receiver->connected = -1;
      gs_cleanup(&receiver->gs_server_data);
      return -1;
  }
  
  if (receiver->gs_server_data.serverInfo.rtspSessionUrl) {
      // Absolute Perfection: Preserve rtspenc:// or rtsps:// schemes 
      // so moonlight-common-c can handle encryption correctly.
      receiver->li_server_info.rtspSessionUrl = receiver->gs_server_data.serverInfo.rtspSessionUrl;
  }

  if (receiver->gs_server_data.serverInfo.serverInfoAppVersion) {
      receiver->li_server_info.serverInfoAppVersion = receiver->gs_server_data.serverInfo.serverInfoAppVersion;
  }
  if (receiver->gs_server_data.serverInfo.serverCodecModeSupport != 0) {
      receiver->li_server_info.serverCodecModeSupport = receiver->gs_server_data.serverInfo.serverCodecModeSupport;
  }
  
  LOG_INFO(COMPONENT_NETWORK, "Spawning Connection Thread...");
  receiver->connection_thread = sceKernelCreateThread("MoonlightNet", ConnectionThread, 0x10, 0x40000, 0, NULL);
  if (receiver->connection_thread >= 0) {
      NetworkReceiver* self = receiver;
      sceKernelStartThread(receiver->connection_thread, sizeof(NetworkReceiver*), &self);
      return 0;
  } else {
      LOG_ERROR(COMPONENT_NETWORK, "Failed to create connection thread: 0x%08X", receiver->connection_thread);
      gs_quit_app(&receiver->gs_server_data);
      gs_cleanup(&receiver->gs_server_data);
      receiver->connected = 0;
      return -1;
  }
}


void network_receiver_set_video_decoder(NetworkReceiver* receiver, VideoDecoder* video_decoder) {
    if (receiver) receiver->video_decoder = video_decoder;
}

void network_receiver_set_audio_decoder(NetworkReceiver* receiver, AudioDecoder* audio_decoder) {
    if (receiver) receiver->audio_decoder = audio_decoder;
}

void network_receiver_send_input(NetworkReceiver* receiver, InputState* input_state) {
    if (!receiver || receiver->connected != 2 || !input_state) return;

    /* Handle Mouse Movement */
    if (input_state->mouse_dx != 0 || input_state->mouse_dy != 0) {
        LiSendMouseMoveEvent(input_state->mouse_dx, input_state->mouse_dy);
    }
    
    /* Handle Mouse Buttons (Press/Release) */
    unsigned char current = input_state->mouse_buttons;
    unsigned char last = receiver->last_mouse_buttons;
    
    if ((current & 0x01) && !(last & 0x01)) LiSendMouseButtonEvent(BUTTON_ACTION_PRESS, BUTTON_LEFT);
    if (!(current & 0x01) && (last & 0x01)) LiSendMouseButtonEvent(BUTTON_ACTION_RELEASE, BUTTON_LEFT);
    
    if ((current & 0x02) && !(last & 0x02)) LiSendMouseButtonEvent(BUTTON_ACTION_PRESS, BUTTON_RIGHT);
    if (!(current & 0x02) && (last & 0x02)) LiSendMouseButtonEvent(BUTTON_ACTION_RELEASE, BUTTON_RIGHT);
    
    receiver->last_mouse_buttons = current;
    
    /* Handle Mouse Scroll */
    if (input_state->scroll_v != 0) {
        LiSendScrollEvent(input_state->scroll_v);
    }

    // Convert to GS buttons
    short gs_buttons = input_mapper_translate_buttons(input_state->buttons);
    
    // Send standard controller event
    LiSendControllerEvent(gs_buttons, 0, 0, input_state->lx, input_state->ly, input_state->rx, input_state->ry);
}

void network_receiver_send_key(NetworkReceiver* receiver, short vkey, int down, char modifiers) {
    if (!receiver || receiver->connected != 2) return;
    LOG_INFO(COMPONENT_NETWORK, "Sending Key: 0x%02X (%s) mod: 0x%02X", vkey, down ? "DOWN" : "UP", modifiers);
    char action = down ? KEY_ACTION_DOWN : KEY_ACTION_UP;
    
    /* Sunshine Optimization: Use LiSendKeyboardEvent2 for better flag support */
    LiSendKeyboardEvent2(vkey, action, modifiers, 0);
}


int network_receiver_quit_app(NetworkReceiver* receiver) {
    if (!receiver) return -1;
    LOG_INFO(COMPONENT_NETWORK, "Quitting current app...");
    int ret = gs_quit_app(&receiver->gs_server_data);
    receiver->gs_server_data.currentGame = 0;
    return ret;
}

int network_receiver_get_current_app_id(NetworkReceiver* receiver) {
    if (!receiver) return 0;
    
    uint64_t now = sceKernelGetSystemTimeLow();
    /* Cache app status for 5 seconds to prevent network/IO stalls from frequent polling */
    /* Fix: Only refresh if 5s passed, regardless of current value, but allow initial 0 refresh */
    if (receiver->last_app_refresh_time == 0 || (now - receiver->last_app_refresh_time > 5000000)) {
        LOG_INFO(COMPONENT_NETWORK, "Returning cached app status from host...");
        // Bypassing gs_init() here to avoid resetting paired state and TLS context!
        // We already fetched currentGame during the initial gs_init sequence.
        receiver->current_live_app_id = receiver->gs_server_data.currentGame;
        receiver->last_app_refresh_time = now;
    }

    
    return receiver->current_live_app_id;
}


void network_receiver_send_ctrl_alt_del(NetworkReceiver* receiver) {
    if (!receiver || receiver->connected != 2) return;
    LOG_INFO(COMPONENT_NETWORK, "Action: Sending CTRL+ALT+DEL...");
    LiSendKeyboardEvent(VK_LCONTROL, KEY_ACTION_DOWN, 0);
    LiSendKeyboardEvent(VK_LMENU, KEY_ACTION_DOWN, 0);
    LiSendKeyboardEvent(VK_DELETE, KEY_ACTION_DOWN, 0);
    
    /* Small delay to ensure host registers the triple-key-down sequence */
    sceKernelDelayThread(20000); 
    
    LiSendKeyboardEvent(VK_DELETE, KEY_ACTION_UP, 0);
    LiSendKeyboardEvent(VK_LMENU, KEY_ACTION_UP, 0);
    LiSendKeyboardEvent(VK_LCONTROL, KEY_ACTION_UP, 0);
}


int network_receiver_get_app_list(NetworkReceiver* receiver, char names[][64], int* ids, int max_apps) {
    if (!receiver) return 0;
    int count = receiver->app_count > max_apps ? max_apps : receiver->app_count;
    for (int i = 0; i < count; i++) {
        strncpy(names[i], receiver->app_names[i], 63);
        ids[i] = receiver->app_ids[i];
    }
    return count;
}

void network_receiver_start_app(NetworkReceiver* receiver, int app_id) {
    if (!receiver) return;
    receiver->selected_app_id = app_id;
    receiver->start_app_now = 1;
    receiver->connected = 4; // Synchronously tell UI thread we are leaving APP_SELECT
}

/* Internal APCTL Event Handler to trace WPA/EAP Drops */
static void apctl_handler(int old_state, int new_state, int event, int error, void *pArg) {
    (void)pArg;
    const char *ev_name = "UNKNOWN";
    switch(event) {
        case PSP_NET_APCTL_EVENT_CONNECT_REQUEST: ev_name = "CONNECT_REQ"; break;
        case PSP_NET_APCTL_EVENT_SCAN_REQUEST: ev_name = "SCAN_REQ"; break;
        case PSP_NET_APCTL_EVENT_SCAN_COMPLETE: ev_name = "SCAN_DONE"; break;
        case PSP_NET_APCTL_EVENT_ESTABLISHED: ev_name = "ESTABLISHED"; break;
        case PSP_NET_APCTL_EVENT_GET_IP: ev_name = "GET_IP"; break;
        case PSP_NET_APCTL_EVENT_DISCONNECT_REQUEST: ev_name = "DISCONNECT_REQ"; break;
        case PSP_NET_APCTL_EVENT_ERROR: ev_name = "ERROR"; break;
        case PSP_NET_APCTL_EVENT_INFO: ev_name = "INFO"; break;
        case PSP_NET_APCTL_EVENT_EAP_AUTH: ev_name = "EAP_AUTH"; break;
        case PSP_NET_APCTL_EVENT_KEY_EXCHANGE: ev_name = "KEY_EXCHANGE"; break;
        case PSP_NET_APCTL_EVENT_RECONNECT: ev_name = "RECONNECT"; break;
    }
    LOG_INFO(COMPONENT_NETWORK, "[ApctlHandler] State: %d->%d | Event: %s (%d) | Error: 0x%08X", 
             old_state, new_state, ev_name, event, error);
}

static int init_psp_networking(void) {
  int err;
  int state = 0;

  LOG_INFO(COMPONENT_NETWORK, "Loading net modules (Exhaustive Absolute Perfection Recipe)...");
  
  sceUtilityLoadModule(PSP_MODULE_NET_COMMON);
  sceUtilityLoadModule(PSP_MODULE_NET_INET);
  sceUtilityLoadModule(PSP_MODULE_NET_PARSEURI);
  sceUtilityLoadModule(PSP_MODULE_NET_PARSEHTTP);
  sceUtilityLoadModule(PSP_MODULE_NET_HTTP);

  /* Absolute Perfection: Optimized pool sizes for PSP-1000 hardware (32MB RAM limit)
   * Scaling to 2MB as per Handbook B12/B4 to eliminate jitter and I/O stalls.
   * Using 64KB stacks for callout and net threads to avoid overflow/crash. */
  err = sceNetInit(2 * 1024 * 1024, 42, 64 * 1024, 42, 64 * 1024);
  if (err < 0) {
    LOG_ERROR(COMPONENT_NETWORK, "sceNetInit FAILED: 0x%08X", err);
    return err;
  }
  
  err = sceNetInetInit();
  if (err < 0) {
    LOG_ERROR(COMPONENT_NETWORK, "sceNetInetInit FAILED: 0x%08X", err);
    return err;
  }
  
  err = sceNetResolverInit();
  if (err < 0) {
    LOG_ERROR(COMPONENT_NETWORK, "sceNetResolverInit FAILED: 0x%08X", err);
    return err;
  }

  err = sceNetApctlInit(0x4000, 42); 
  if (err < 0) {
    LOG_ERROR(COMPONENT_NETWORK, "sceNetApctlInit FAILED: 0x%08X", err);
    return err;
  }
  
  /* Add our custom handler to trace network events (drops, etc.) */
  sceNetApctlAddHandler(apctl_handler, NULL);

  /* NOTE: WPA2 kernel patching (ARK-4 style) requires kernel-mode privileges.
   * It CANNOT be done from a user-mode PRX. The user must install ARK-4 CFW 
   * which applies the WPA2 patch at the firmware level automatically. */

  /* ---- Step 3: Check WLAN switch & Radio Power (Non-blocking) ---- */
  {
    if (sceWlanGetSwitchState() == 0) {
        LOG_INFO(COMPONENT_NETWORK, "WLAN switch is OFF. (Ignoring for emulator compatibility)");
    }

    if (sceWlanDevIsPowerOn() == 0) {
        LOG_INFO(COMPONENT_NETWORK, "WLAN radio power is OFF. (Attempting wakeup via connect later)");
    }
  }

  /* ---- Step 4: Skip dialog if already connected ---- */
  sceNetApctlGetState(&state);
  if (state == 4) {
    LOG_INFO(COMPONENT_NETWORK, "Already connected - skipping selector");
    goto get_ip;
  }


  /* ---- Step 5: Launch Custom In-App WiFi Selector ---- */
  {
    ui_renderer_begin_frame();
    ui_draw_header("MISSION CALIBRATION");
    ui_draw_panel(40, 80, 400, 100, 0xAA111111, 1);
    ui_draw_text("Scanning for saved WiFi networks...", 60, 100, 0xFFFFFFFF);
    ui_renderer_end_frame();

    typedef struct {
      int index;
      char ssid[128];
    } WifiProfile;

    WifiProfile profiles[10];
    int num_profiles = 0;

    /* Scan PSP registry for saved network param profiles (indices 1 to 10) */
    for (int i = 1; i <= 10; i++) {
        if (sceUtilityCheckNetParam(i) == 0) {
            netData data;
            if (sceUtilityGetNetParam(i, PSP_NETPARAM_SSID, &data) == 0) {
                profiles[num_profiles].index = i;
                strncpy(profiles[num_profiles].ssid, data.asString, 127);
                profiles[num_profiles].ssid[127] = '\0';
                num_profiles++;
            }
        }
    }

    if (num_profiles == 0) {
        LOG_ERROR(COMPONENT_NETWORK, "No configured WiFi profiles found on PSP.");
        ui_renderer_begin_frame();
        ui_draw_header("ERROR: NO WI-FI PROFILES");
        ui_draw_panel(40, 80, 400, 100, 0xAA221111, 1);
        ui_draw_text("Please configure a connection in PSP Settings.", 50, 110, 0xFFFFFFFF);
        ui_renderer_end_frame();
        sceKernelDelayThread(5000000); 
        return -1;
    }

    LOG_INFO(COMPONENT_NETWORK, "Found %d saved WiFi profiles.", num_profiles);
    
    int selected = 0;

    LOG_INFO(COMPONENT_NETWORK, "Entering Wi-Fi Selector Loop...");
    SceCtrlData pad;
    unsigned int last_buttons = 0;
    
    /* Ensure analog/dpad mode is set */
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);

    /* Custom Input UI Loop using Premium UI Renderer */
    while (1) {
        ui_renderer_begin_frame();
        
        const char* profile_ssids[10];
        for (int i = 0; i < num_profiles; i++) profile_ssids[i] = profiles[i].ssid;
        
        ui_draw_wifi_selector(profile_ssids, num_profiles, selected);
        
        ui_renderer_end_frame();

        sceCtrlReadBufferPositive(&pad, 1);
        unsigned int new_buttons = pad.Buttons & ~last_buttons;

        if (new_buttons & PSP_CTRL_UP) {
            selected--;
            if (selected < 0) selected = num_profiles - 1;
        }
        if (new_buttons & PSP_CTRL_DOWN) {
            selected++;
            if (selected >= num_profiles) selected = 0;
        }
        if (new_buttons & PSP_CTRL_CROSS) break;
        if (new_buttons & PSP_CTRL_CIRCLE) {
            LOG_ERROR(COMPONENT_NETWORK, "User cancelled Wi-Fi selection.");
            return -1;
        }

        last_buttons = pad.Buttons;
        sceKernelDelayThread(10000);
    }
    LOG_INFO(COMPONENT_NETWORK, "Selected profile index %d (%s). Connecting...", profiles[selected].index, profiles[selected].ssid);

    /* Wait for WLAN radio to fully power up before attempting connection.
     * The PSP's WLAN hardware needs time after sceNetApctlInit before the
     * radio is ready to associate. Without this delay, sceNetApctlConnect
     * fires before the LED even turns on, causing immediate failures. */
    /* Wait for WLAN radio to fully power up before attempting connection. */

    /* Absolute Perfection: Non-blocking Radio Power Check for Emulator Compatibility */
    if (sceWlanDevIsPowerOn() == 0) {
        LOG_INFO(COMPONENT_NETWORK, "WLAN radio power is OFF (Normal for emulator, checking settings on hardware)");
    }
    
    /* Absolute Perfection: Increase warmup to 3.0s as per Handbook 0xDEADEAD Fix.
     * This ensures voltage stabilization before radio association on real hardware. */
    LOG_INFO(COMPONENT_NETWORK, "Warming up radio (3s)...");
    for (int warmup = 0; warmup < 30; warmup++) {
        ui_renderer_begin_frame();
        ui_draw_wifi_status(profiles[selected].ssid, "Warming up radio...");
        ui_renderer_end_frame();
        sceKernelDelayThread(100000); /* 100ms * 30 = 3s */
    }

    /* Connect directly to the chosen profile without XMB dialogs */
    int wifi_attempt;
    int wifi_connected = 0;
    for (wifi_attempt = 0; wifi_attempt < 3; wifi_attempt++) {
        /* Absolute Perfection: UI Feedback */
        ui_renderer_begin_frame();
        ui_draw_wifi_status(profiles[selected].ssid, wifi_attempt > 0 ? "Retrying Connection..." : "Connecting...");
        ui_renderer_end_frame();

        if (wifi_attempt > 0) {
            /* Hardware Reset Stage only after failure */
            LOG_INFO(COMPONENT_NETWORK, "Absolute Perfection Reset Stage (Failure recovery)...");
            sceNetApctlDisconnect();
            
            /* Wait for Idle State (0) */
            int reset_timeout = 20;
            int current_state = -1;
            while (reset_timeout > 0) {
                sceNetApctlGetState(&current_state);
                if (current_state == 0) break;
                sceKernelDelayThread(100000); // 100ms
                reset_timeout--;
            }
            sceKernelDelayThread(1000000); /* 1s cooldown */
        }

        LOG_INFO(COMPONENT_NETWORK, "sceNetApctlConnect(index=%d) SSID: %s (attempt %d)", profiles[selected].index, profiles[selected].ssid, wifi_attempt + 1);
        err = sceNetApctlConnect(profiles[selected].index);
        
        if (err != 0 && err != (int)0x8002013A) {
            LOG_ERROR(COMPONENT_NETWORK, "sceNetApctlConnect FAILED: 0x%08X", err);
            continue;
        }



        
        if (err == (int)0x8002013A) {
            LOG_INFO(COMPONENT_NETWORK, "sceNetApctlConnect: Already connected/connecting (0x8002013A)");
        }
        
        /* Wait for connection state to reach Connected (4) */
        uint64_t t0 = sceKernelGetSystemTimeWide();
        const uint64_t TIMEOUT = 15ULL * 1000000ULL; /* 15s per attempt */
        int last_state = -1;
        int max_state = -1;
        int timed_out = 0;
        while (1) {
          err = sceNetApctlGetState(&state);
          if (err != 0 && err != (int)0x8002013A) {
            LOG_ERROR(COMPONENT_NETWORK, "sceNetApctlGetState err: 0x%08X", err);
            break;
          }
          
          if (state == 4) {
            LOG_INFO(COMPONENT_NETWORK, "WiFi connected effectively (state 4)");
            /* Stability Secret: Even after state 4, the network stack needs 
             * a moment to stabilize before high-throughput socket calls. */
            sceKernelDelayThread(500000); 
            wifi_connected = 1;
            break;
          }
          if (state > max_state) max_state = state;
          
          if (state != last_state) {
            const char *s = "Joining";
            if (state == 0) s = "Idle";
            else if (state == 1) s = "Scanning";
            else if (state == 3) s = "DHCP";
            else if (state == 4) s = "Connected";
            
            ui_renderer_begin_frame();
            ui_draw_wifi_status(profiles[selected].ssid, s);
            ui_renderer_end_frame();

            LOG_INFO(COMPONENT_NETWORK, "WiFi state -> %d (%s)", state, s);
            last_state = state;
          }
          if (state == 4) { wifi_connected = 1; break; }
          
          /* Only drop the attempt if we actually started connecting (>0) and fell back to Idle */
          if (state == 0 && max_state > 0) {
            LOG_INFO(COMPONENT_NETWORK, "WiFi dropped from state %d to Idle, retrying...", max_state);
            break; /* Go to next attempt */
          }
          if (state == 0 && (sceKernelGetSystemTimeWide() - t0) > (5ULL * 1000000ULL)) {
            LOG_INFO(COMPONENT_NETWORK, "WiFi stuck in Idle for 5s, retrying...");
            break; /* Radio failed to engage */
          }
          
          if ((sceKernelGetSystemTimeWide() - t0) > TIMEOUT) {
            LOG_ERROR(COMPONENT_NETWORK, "WiFi TIMEOUT after 15s (attempt %d)", wifi_attempt + 1);
            LOG_ERROR(COMPONENT_NETWORK, "WiFi TIMEOUT after 15s (attempt %d)", wifi_attempt + 1);
            timed_out = 1;
            break;
          }
          sceKernelDelayThread(100000); /* 100ms */
        }
        if (wifi_connected) break;
    }
    
    if (!wifi_connected) {
        LOG_ERROR(COMPONENT_NETWORK, "WiFi failed after 3 attempts.");
        LOG_ERROR(COMPONENT_NETWORK, "WiFi failed after 3 attempts.");
        sceKernelDelayThread(3000000);
        return (int)0x80410D02;
    }
  }

get_ip:
  LOG_INFO(COMPONENT_NETWORK, "WiFi connected!");
  LOG_INFO(COMPONENT_NETWORK, "WiFi connected!");
  {
    union SceNetApctlInfo info;
    memset(&info, 0, sizeof(info));
    if (sceNetApctlGetInfo(8 /* PSP_NET_APCTL_INFO_IP */, &info) == 0) {
      strncpy(g_local_ip, info.ip, 15);
      g_local_ip[15] = '\0';
      LOG_INFO(COMPONENT_NETWORK, "Local IP: %s", g_local_ip);
      LOG_INFO(COMPONENT_NETWORK, "Local IP: %s", g_local_ip);
    }
  }
  return 0;
}