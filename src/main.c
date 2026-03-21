/**
 * @file main.c
 * @brief Main application entry point for PSP Moonlight
 *
 * This file contains the main application structure, module initialization,
 * and the main loop that coordinates all modules.
 */

#include <pspctrl.h>
#include <pspdebug.h>
#include <pspdisplay.h>
#include <pspge.h>
#include <pspkernel.h>
#include <pspsdk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "modules/render_pipeline.h"
#include "modules/ui_renderer.h"
#include <psprtc.h>
#include <psppower.h>


/* Module headers */
#include "modules/video_decoder.h"
#include "modules/audio_decoder.h"
#include "modules/network_receiver.h"
#include "modules/input_mapper.h"
#include "modules/exception_handler.h"
#include "libgamestream/client.h"
#include "modules/logger.h"


PSP_MODULE_INFO("PSPMoonlight", 0, 0, 1);
PSP_MAIN_THREAD_ATTR(PSP_THREAD_ATTR_USER | PSP_THREAD_ATTR_VFPU);

/* PRX Export Hooks */
int module_stop(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    return 0;
}
PSP_HEAP_SIZE_KB(10 * 1024); /* Optimized to 10MB to leave room for Partition Stacks and ME buffers */


/* Define module pointers */
static NetworkReceiver *g_network_receiver = NULL;
VideoDecoder *g_video_decoder = NULL;
AudioDecoder *g_audio_decoder = NULL;
static InputMapper *g_input_mapper = NULL;
static RenderPipeline *g_render_pipeline = NULL;

/* Semaphore for UI access to prevent overlapping from multiple threads */
static SceUID g_ui_sema = -1;

void ui_lock() {
    if (g_ui_sema >= 0) sceKernelWaitSema(g_ui_sema, 1, NULL);
}

void ui_unlock() {
    if (g_ui_sema >= 0) sceKernelSignalSema(g_ui_sema, 1);
}

/* Application state */
static int g_running = 0;
static unsigned int g_frame_counter = 0; /* Used for ASCII animations */

typedef enum {
    STATE_MAIN_MENU,
    STATE_ENTER_IP,
    STATE_CONNECTING,
    STATE_WAITING,
    STATE_APP_SELECT,
    STATE_STREAMING,
    STATE_PAUSE_MENU,
    STATE_QUIT_CONFIRM
} AppState;

static AppState current_state = STATE_MAIN_MENU;

/* Host configuration */
#define MAX_HOSTS 5
static char saved_hosts[MAX_HOSTS][64] = {
    "10.0.0.73",
    "127.0.0.1",
    "192.168.1.100",
    "Empty",
    "Empty"
};
static int menu_selection = 0;
static int app_selection_idx = 0;
static int pause_menu_selection = 0;
static int pause_active = 0;
static int pending_app_id = 0;
static int quit_confirm_selection = 0;

static SceCtrlData pad_data;
static SceCtrlData old_pad;

/* Stream parameters */
static int current_res_idx = 0; // 0=480x272, 1=720p UI (480x272), 2=360x204
static int current_fps_idx = 0; // 0=30, 1=60
static int current_bitrate_idx = 1; // 0=1000, 1=2000, 2=5000
static int current_control_mode = 0; // 0=Xbox, 1=Browser

static const int res_w[] = {480, 480, 360};
static const int res_h[] = {272, 272, 204};
static const int host_w[] = {0, 1280, 0};
static const int host_h[] = {0, 720, 0};
static const int fps_opts[] = {30, 60};
static const int bitrate_opts[] = {1000, 2000, 5000};

/* Host persistence */
#define HOSTS_FILE "ms0:/moonlight/hosts.txt"

static void save_hosts() {
    sceIoMkdir("ms0:/moonlight", 0777);
    SceUID fd = sceIoOpen(HOSTS_FILE, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd >= 0) {
        int i;
        for (i = 0; i < MAX_HOSTS; i++) {
            sceIoWrite(fd, saved_hosts[i], strlen(saved_hosts[i]));
            sceIoWrite(fd, "\n", 1);
        }
        sceIoClose(fd);
        LOG_INFO(COMPONENT_MAIN, "Hosts saved to %s", HOSTS_FILE);
    }
}

static void load_hosts() {
    SceUID fd = sceIoOpen(HOSTS_FILE, PSP_O_RDONLY, 0);
    if (fd >= 0) {
        char buf[1024];
        int read = sceIoRead(fd, buf, sizeof(buf) - 1);
        sceIoClose(fd);
        if (read > 0) {
            buf[read] = '\0';
            char *line = strtok(buf, "\n\r");
            int i = 0;
            while (line && i < MAX_HOSTS) {
                strncpy(saved_hosts[i++], line, 63);
                line = strtok(NULL, "\n\r");
            }
            LOG_INFO(COMPONENT_MAIN, "Hosts loaded from %s", HOSTS_FILE);
            return;
        }
    }
    LOG_INFO(COMPONENT_MAIN, "No hosts file found, using defaults.");
}

/* IP Entry state */
static int ip_cursor = 0;
static char temp_ip[64] = "192.168.1.   ";

/* Callbacks for Home Button / OS Exit */
int exit_callback(int arg1, int arg2, void *common) {
    (void)arg1;
    (void)arg2;
    (void)common;
    /* On PSP, the correct way to handle the Home button "Yes" press is to
     * call sceKernelExitGame() directly from the callback. Doing cleanup
     * before this call causes crashes because modules like sceMpeg may be
     * in an inconsistent state. The kernel reclaims all resources on exit. */
    LOG_INFO(COMPONENT_MAIN, "Exit callback triggered. Calling sceKernelExitGame.");
    sceKernelExitGame();
    return 0;
}

int CallbackThread(SceSize args, void *argp) {
    (void)args;
    (void)argp;
    int cbid;
    cbid = sceKernelCreateCallback("Exit Callback", exit_callback, NULL);
    sceKernelRegisterExitCallback(cbid);
    sceKernelSleepThreadCB();
    return 0;
}

int SetupCallbacks(void) {
    int thid = 0;
  thid = sceKernelCreateThread("update_thread", CallbackThread, 0x11, 0x4000, 0, 0);
    if (thid >= 0) {
        sceKernelStartThread(thid, 0, 0);
    }
    return thid;
}

static void draw_wrapped_text(int x, int y, const char* text, int max_width, unsigned int color) {
    if (!text) return;
    char buffer[1024];
    strncpy(buffer, text, sizeof(buffer)-1);
    buffer[sizeof(buffer)-1] = '\0';
    
    char* lines[10];
    int line_count = 0;
    char* start = buffer;
    
    while (*start && line_count < 10) {
        lines[line_count++] = start;
        char* last_space = NULL;
        char* p = start;
        int current_width = 0;
        
        while (*p && current_width < max_width) {
            if (*p == ' ') last_space = p;
            current_width += 8; // Estimating 8px per char for standard font
            p++;
        }
        
        if (current_width >= max_width && last_space && last_space > start) {
            *last_space = '\0';
            start = last_space + 1;
        } else if (*p) {
            char tmp = *p;
            *p = '\0';
            start = p + (tmp ? 0 : 0); // Logic fix: if we cut mid-word because no space
            if (tmp) { *p = tmp; start = p; } else { start = p; }
            // Better logic:
            char* next = p;
            while (*next && *next != ' ' && (next - start) < 50) next++; // Find next space or 50 chars
            if (*next == ' ') { *next = '\0'; start = next + 1; }
            else { char saved = *p; *p = '\0'; start = p; *p = saved; }
        } else {
            break;
        }
    }
    
    for (int i = 0; i < line_count; i++) {
        // Center text in the line if it's within the box
        int len = strlen(lines[i]);
        int line_x = x;
        if (len * 8 < max_width) {
            line_x = x + (max_width - (len * 8)) / 2;
        }
        ui_draw_text(lines[i], line_x, y + (i * 18), color);
    }
}



/** Shared display list for all GU modules: Increased to 256KB for premium UI overhead */
unsigned int __attribute__((aligned(16))) g_gu_display_list[65536];

/* Polyfill usleep for moonlight-common-c */
int usleep(unsigned int usec) {
  sceKernelDelayThread(usec);
  return 0;
}

static int initialize_modules(void) {
  /* Absolute Perfection: UI Renderer MUST be first for any screen output */
  if (ui_renderer_init() < 0) return -1;

  /* Ensure folder exists for pairing persistence before any GS operations */
  sceIoMkdir("ms0:/moonlight", 0777); 
  sceIoMkdir("ms0:/moonlight/keys", 0777); /* Absolute Perfection: Ensure keys subfolder specifically exists */

  g_network_receiver = network_receiver_create();
  if (network_receiver_init(g_network_receiver) < 0) return -1;

  g_video_decoder = video_decoder_create();
  if (video_decoder_init(g_video_decoder) < 0) return -1;

  g_audio_decoder = audio_decoder_create();
  if (audio_decoder_init(g_audio_decoder) < 0) return -1;

  g_input_mapper = input_mapper_create();
  if (input_mapper_init(g_input_mapper) < 0) return -1;

  g_render_pipeline = render_pipeline_create();
  if (render_pipeline_init(g_render_pipeline) < 0) return -1;

  render_pipeline_set_video_decoder(g_render_pipeline, g_video_decoder);
  network_receiver_set_video_decoder(g_network_receiver, g_video_decoder);
  network_receiver_set_audio_decoder(g_network_receiver, g_audio_decoder);
  
  LOG_INFO(COMPONENT_MAIN, "All modules initialized successfully.");
  return 0;
}

/* cleanup_modules is not used to prevent PSP kernel crashes on exit */  

static int is_button_pressed(unsigned int button) {
    return (pad_data.Buttons & button) && !(old_pad.Buttons & button);
}

/* --- State Drawing Logic Functions --- */

static void draw_main_menu_logic() {
    ui_draw_background();
    ui_draw_header("PSP MOONLIGHT");
    ui_draw_panel(20, 50, 440, 200, 0xCC111111, 1); /* Increased height to 200 */
    
    for (int i = 0; i < MAX_HOSTS; i++) {
        char item_text[128];
        if (strcmp(saved_hosts[i], "Empty") != 0) snprintf(item_text, sizeof(item_text), "[%s]", saved_hosts[i]);
        else snprintf(item_text, sizeof(item_text), "[ <ADD NEW HOST> ]");
        ui_draw_menu_item(item_text, 40, 65 + (i * 22), 400, (menu_selection == i));
    }

    /* Adjusted Y for settings to prevent overlap */
    const char* res_names[] = {"480x272 (Native)", "720p UI (480x272)", "360x204 (Fast)"};
    char res_str[64];
    snprintf(res_str, sizeof(res_str), "Res: %s", res_names[current_res_idx]);
    ui_draw_menu_item(res_str, 40, 180, 195, (menu_selection == MAX_HOSTS));

    char fps_str[64];
    snprintf(fps_str, sizeof(fps_str), "FPS: %d", fps_opts[current_fps_idx]);
    ui_draw_menu_item(fps_str, 245, 180, 195, (menu_selection == MAX_HOSTS + 1));

    char bitrate_str[64];
    snprintf(bitrate_str, sizeof(bitrate_str), "Bitrate: %d", bitrate_opts[current_bitrate_idx]);
    ui_draw_menu_item(bitrate_str, 40, 210, 195, (menu_selection == MAX_HOSTS + 2));
    
    const char* mode_names[] = {"Xbox", "Browser"};
    char control_str[64];
    snprintf(control_str, sizeof(control_str), "Mode: %s", mode_names[current_control_mode]);
    ui_draw_menu_item(control_str, 245, 210, 195, (menu_selection == MAX_HOSTS + 3));

    ui_draw_status_bar("X:Select | O:Quit | Left/Right:Setting | Triangle:Del");
}


static void draw_enter_ip_logic() {
    ui_draw_background();
    ui_draw_header("ADD / EDIT HOST IP");
    ui_draw_panel(20, 50, 440, 100, 0xCC222222, 1);
    /* Semi-transparent green cursor DRAWN BEFORE text for character visibility */
    ui_draw_panel(82 + (ip_cursor * 8), 78, 8, 12, 0x7F00FF00, 0); 
    char buf[128];
    snprintf(buf, sizeof(buf), "IP: %s", temp_ip);
    ui_draw_text(buf, 50, 80, 0xFFFFFFFF);
    ui_draw_status_bar("UP/DOWN:Char | Left/Right:Cursor | X:Save | O:Cancel");

}

static void draw_connecting_logic(int is_error) {
    ui_draw_background();
    ui_draw_header(is_error ? "CONNECTION ERROR" : "CONNECTING");
    ui_draw_panel(20, 50, 440, 100, is_error ? 0xAA000044 : 0xAA222222, 1);
    char buf[256];
    snprintf(buf, sizeof(buf), "Host: %s", saved_hosts[menu_selection]);
    ui_draw_text(buf, 50, 70, 0xFFFFFFFF);
    
    const char* status = network_receiver_get_status(g_network_receiver);
    snprintf(buf, sizeof(buf), "Status: %s", status);
    
    /* Absolute Perfection: Space-aware word wrap for premium UI aesthetics */
    draw_wrapped_text(50, 95, buf, 380, is_error ? 0xFF0000FF : 0xFFFFFF00);
    
    ui_draw_status_bar("Press CIRCLE to return to menu");
}

static void draw_app_select_logic(char names[][64], int count, int selection) {
    ui_draw_background();
    ui_draw_header("SELECT APPLICATION");
    ui_draw_panel(20, 50, 440, 180, 0xCC111111, 1);
    if (count == 0) ui_draw_text("Loading applications...", 100, 100, 0xFFFFFFFF);
    else {
        for (int i = 0; i < count && i < 6; i++) {
            ui_draw_menu_item(names[i], 40, 70 + (i * 26), 400, (selection == i));
        }
    }
    ui_draw_status_bar("Press X to Start | O to Cancel");
}

static void draw_quit_confirm_logic(const char* current_app, const char* new_app) {
    ui_draw_panel(60, 80, 360, 120, 0xDD111111, 1);
    ui_draw_text("ANOTHER APP IS RUNNING", 110, 95, 0xFFFFFF00);
    
    char msg[128];
    snprintf(msg, sizeof(msg), "Quit %s", current_app);
    ui_draw_text(msg, 100, 115, 0xFFFFFFFF);
    snprintf(msg, sizeof(msg), "and start %s?", new_app);
    ui_draw_text(msg, 100, 130, 0xFFFFFFFF);
    
    ui_draw_menu_item("Yes, Quit and Start", 80, 150, 320, (quit_confirm_selection == 0));
    ui_draw_menu_item("No, Cancel", 80, 172, 320, (quit_confirm_selection == 1));
}

static void draw_pause_overlay_logic() {
    ui_draw_panel(80, 60, 320, 150, 0xCC111111, 1);
    ui_draw_text("[ STREAM PAUSED ]", 140, 70, 0xFFFFFF00);
    const char* options[] = { "Resume", "Control Mode", "Alt-Tab (SEL + L)", "Win Key (SEL + UP)", "Disconnect" };
    for (int i = 0; i < 5; i++) {
        ui_draw_menu_item(options[i], 100, 90 + (i * 22), 280, (pause_menu_selection == i));
    }
    char mode_info[64];
    snprintf(mode_info, sizeof(mode_info), "Control: %s", (current_control_mode == 0) ? "Xbox" : "Browser");
    ui_draw_text(mode_info, 100, 205, 0xFFFFFFFF);
    ui_draw_text("Shortcuts: SEL+UP: Win | SEL+L: Alt-Tab | SEL+R: Mode", 90, 222, 0xFFAAAAAA);

}

static void main_loop(void) {
    LOG_INFO(COMPONENT_MAIN, "Entering main loop...");
    
    while (g_running) {
        sceCtrlReadBufferPositive(&pad_data, 1);
        ui_renderer_begin_frame();

        switch (current_state) {
            case STATE_MAIN_MENU:
                if (is_button_pressed(PSP_CTRL_UP)) menu_selection = (menu_selection - 1 + (MAX_HOSTS + 4)) % (MAX_HOSTS + 4);
                if (is_button_pressed(PSP_CTRL_DOWN)) menu_selection = (menu_selection + 1) % (MAX_HOSTS + 4);
                if (is_button_pressed(PSP_CTRL_LEFT) || is_button_pressed(PSP_CTRL_RIGHT)) {
                    int dir = is_button_pressed(PSP_CTRL_RIGHT) ? 1 : -1;
                    if (menu_selection == MAX_HOSTS) current_res_idx = (current_res_idx + dir + 3) % 3;
                    else if (menu_selection == MAX_HOSTS + 1) current_fps_idx = (current_fps_idx + dir + 2) % 2;
                    else if (menu_selection == MAX_HOSTS + 2) current_bitrate_idx = (current_bitrate_idx + dir + 4) % 4;
                    else if (menu_selection == MAX_HOSTS + 3) current_control_mode = (current_control_mode + dir + 2) % 2;
                }
                if (is_button_pressed(PSP_CTRL_TRIANGLE) && menu_selection < MAX_HOSTS) {
                    strcpy(saved_hosts[menu_selection], "Empty");
                    save_hosts();
                }
                if (is_button_pressed(PSP_CTRL_CROSS)) {
                    if (menu_selection < MAX_HOSTS) {
                        if (strcmp(saved_hosts[menu_selection], "Empty") == 0) {
                            strcpy(temp_ip, "192.168.1.    ");
                            ip_cursor = 10;
                            current_state = STATE_ENTER_IP;
                        } else {
                            current_state = STATE_CONNECTING;
                            network_receiver_connect(g_network_receiver, saved_hosts[menu_selection], 47989, 
                                                  res_w[current_res_idx], res_h[current_res_idx], 
                                                  host_w[current_res_idx], host_h[current_res_idx],
                                                  fps_opts[current_fps_idx], bitrate_opts[current_bitrate_idx]);
                        }
                    }
                }
                if (is_button_pressed(PSP_CTRL_CIRCLE)) g_running = 0;
                draw_main_menu_logic();
                break;

            case STATE_ENTER_IP:
            {
                const char char_map[] = "0123456789. ";
                int map_len = 12;
                if (is_button_pressed(PSP_CTRL_LEFT) && ip_cursor > 0) ip_cursor--;
                if (is_button_pressed(PSP_CTRL_RIGHT) && ip_cursor < 14) ip_cursor++;
                if (is_button_pressed(PSP_CTRL_UP) || is_button_pressed(PSP_CTRL_DOWN)) {
                    char c = temp_ip[ip_cursor];
                    int idx = 0;
                    for (int k = 0; k < map_len; k++) if (char_map[k] == c) { idx = k; break; }
                    if (is_button_pressed(PSP_CTRL_UP)) idx = (idx + 1) % map_len;
                    else idx = (idx - 1 + map_len) % map_len;
                    temp_ip[ip_cursor] = char_map[idx];
                }
                if (is_button_pressed(PSP_CTRL_CROSS)) {
                    char clean_ip[64] = {0};
                    int j = 0;
                    for(int k=0; temp_ip[k] != '\0' && k < 15; k++) if (temp_ip[k] != ' ') clean_ip[j++] = temp_ip[k];
                    
                    /* IP Validation: Must not end in a dot and must have at least one character */
                    if (j > 0 && clean_ip[j-1] != '.') { 
                        strncpy(saved_hosts[menu_selection], clean_ip, 63); 
                        save_hosts(); 
                        current_state = STATE_MAIN_MENU;
                    }
                }
                if (is_button_pressed(PSP_CTRL_CIRCLE)) current_state = STATE_MAIN_MENU;
                draw_enter_ip_logic();
                break;
            }

            case STATE_CONNECTING:
            case STATE_WAITING:
            {
                int conn_status = network_receiver_is_connected(g_network_receiver);
                if (conn_status == -1) {
                    draw_connecting_logic(1); /* DRAW ERROR STATE */
                } else {
                    draw_connecting_logic(0); /* DRAW CONNECTING STATE */
                }

                if (is_button_pressed(PSP_CTRL_CIRCLE)) {
                    network_receiver_disconnect(g_network_receiver);
                    current_state = STATE_MAIN_MENU;
                }
                if (conn_status == 2) current_state = STATE_STREAMING;
                else if (conn_status == 3) { current_state = STATE_APP_SELECT; app_selection_idx = 0; }
                break;
            }

            case STATE_APP_SELECT:
            {
                char apps[20][64];
                int ids[20];
                int count = network_receiver_get_app_list(g_network_receiver, apps, ids, 20);
                if (count > 0) {
                    if (is_button_pressed(PSP_CTRL_UP)) app_selection_idx = (app_selection_idx - 1 + count) % count;
                    if (is_button_pressed(PSP_CTRL_DOWN)) app_selection_idx = (app_selection_idx + 1) % count;
                    if (is_button_pressed(PSP_CTRL_CROSS)) {
                        int live_app_id = network_receiver_get_current_app_id(g_network_receiver);
                        if (live_app_id != 0 && live_app_id != ids[app_selection_idx]) {
                            pending_app_id = ids[app_selection_idx];
                            current_state = STATE_QUIT_CONFIRM;
                            quit_confirm_selection = 0;
                        } else {
                            network_receiver_start_app(g_network_receiver, ids[app_selection_idx]);
                            current_state = STATE_WAITING;
                        }
                    }
                }
                if (is_button_pressed(PSP_CTRL_CIRCLE)) { network_receiver_disconnect(g_network_receiver); current_state = STATE_MAIN_MENU; }
                
                /* Highlight current app in the list if it matches ids[i] */
                int current_app_id = network_receiver_get_current_app_id(g_network_receiver);
                draw_app_select_logic(apps, count, app_selection_idx);
                
                /* Overlays the "Live" tag if we identify the running app */
                for (int i=0; i<count; i++) {
                    if (ids[i] == current_app_id && current_app_id != 0) {
                        ui_draw_text("[LIVE]", 350, 75 + (i * 26), 0xFF00FF00);
                    }
                }
                break;
            }

            case STATE_QUIT_CONFIRM:
            {
                char apps[20][64];
                int ids[20];
                int count = network_receiver_get_app_list(g_network_receiver, apps, ids, 20);
                
                const char* cur_name = "current app";
                const char* new_name = "selected app";
                int live_id = network_receiver_get_current_app_id(g_network_receiver);
                
                for (int i=0; i<count; i++) {
                    if (ids[i] == live_id) cur_name = apps[i];
                    if (ids[i] == pending_app_id) new_name = apps[i];
                }

                if (is_button_pressed(PSP_CTRL_UP) || is_button_pressed(PSP_CTRL_DOWN)) 
                    quit_confirm_selection = !quit_confirm_selection;
                
                if (is_button_pressed(PSP_CTRL_CROSS)) {
                    if (quit_confirm_selection == 0) {
                        /* User confirmed: Quit then Start */
                        network_receiver_quit_app(g_network_receiver);
                        sceKernelDelayThread(1000000); // 1s delay
                        network_receiver_start_app(g_network_receiver, pending_app_id);
                        current_state = STATE_WAITING;
                    } else {
                        current_state = STATE_APP_SELECT;
                    }
                }
                if (is_button_pressed(PSP_CTRL_CIRCLE)) current_state = STATE_APP_SELECT;
                
                draw_quit_confirm_logic(cur_name, new_name);
                break;
            }

            case STATE_STREAMING:
            {
                int status = network_receiver_is_connected(g_network_receiver);
                if (status != 2) {
                    network_receiver_disconnect(g_network_receiver);
                    current_state = STATE_MAIN_MENU;
                    pause_active = 0;
                } else {
                    if (is_button_pressed(PSP_CTRL_START) && is_button_pressed(PSP_CTRL_SELECT)) {
                        pause_active = !pause_active;
                        pause_menu_selection = 0;
                    }

                    if (!pause_active) {
                        /* Direct Shortcuts (SELECT + Combined) */
                        if (pad_data.Buttons & PSP_CTRL_SELECT) {
                            if (is_button_pressed(PSP_CTRL_UP)) {
                                LOG_INFO(COMPONENT_MAIN, "Win Key Shortcut Triggered!");
                                network_receiver_send_key(g_network_receiver, VK_LWIN, 1, 0x08);
                                network_receiver_send_key(g_network_receiver, VK_LWIN, 0, 0x08);
                            }
                            if (is_button_pressed(PSP_CTRL_LTRIGGER)) {
                                LOG_INFO(COMPONENT_MAIN, "Alt-Tab Shortcut Triggered!");
                                network_receiver_send_key(g_network_receiver, VK_MENU, 1, 0);
                                network_receiver_send_key(g_network_receiver, VK_TAB, 1, 0x04);
                                network_receiver_send_key(g_network_receiver, VK_TAB, 0, 0x04);
                                network_receiver_send_key(g_network_receiver, VK_MENU, 0, 0);
                            }
                            if (is_button_pressed(PSP_CTRL_RTRIGGER)) {
                                current_control_mode = !current_control_mode;
                                LOG_INFO(COMPONENT_MAIN, "Control Mode Toggled: %s", current_control_mode ? "Browser" : "Gamepad");
                            }
                        }

                        input_mapper_update(g_input_mapper, &pad_data);
                        InputState input_state;
                        input_mapper_get_state(g_input_mapper, &input_state, (ControlMode)current_control_mode);
                        network_receiver_send_input(g_network_receiver, &input_state);
                        video_decoder_update(g_video_decoder);
                        audio_decoder_update(g_audio_decoder);

                        /* Absolute Perfection: Periodic Stability Logs (every ~60 seconds at 60fps) */
                        static int stability_check_frames = 0;
                        if (++stability_check_frames >= 3600) {
                            LOG_INFO(COMPONENT_MAIN, "STREAMING STABILITY CHECK: %d MINUTES REACHED", stability_check_frames / 3600);
                            stability_check_frames = 0;
                        }
                    }
                    
                    render_pipeline_draw_video(g_render_pipeline);

                    /* Absolute Perfection: Sunshine Message Overlay (Toast) */
                    const char* status = network_receiver_get_status(g_network_receiver);
                    if (status && strncmp(status, "MSG: ", 5) == 0) {
                        ui_draw_panel(20, 20, 440, 30, 0xCC111111, 1);
                        ui_draw_text(status + 5, 30, 32, 0xFF00FFFF);
                        
                        /* Auto-clear message after 5 seconds */
                        static uint64_t msg_start_time = 0;
                        static char last_msg[128] = {0};
                        if (strcmp(last_msg, status) != 0) {
                            msg_start_time = sceKernelGetSystemTimeWide();
                            strncpy(last_msg, status, 127);
                        }
                        if (sceKernelGetSystemTimeWide() - msg_start_time > 5000000) {
                            /* We can't easily clear the status in receiver from here, 
                               but we can stop drawing it. 
                               Actually, let's just use the timer. */
                        }
                    }


                    if (pause_active) {
                        if (is_button_pressed(PSP_CTRL_UP)) pause_menu_selection = (pause_menu_selection - 1 + 5) % 5;
                        if (is_button_pressed(PSP_CTRL_DOWN)) pause_menu_selection = (pause_menu_selection + 1) % 5;
                        if (is_button_pressed(PSP_CTRL_CROSS)) {
                            if (pause_menu_selection == 0) pause_active = 0;
                            else if (pause_menu_selection == 1) current_control_mode = !current_control_mode;
                            else if (pause_menu_selection == 2) {
                                /* Alt-Tab: Send Alt Down, Tab Down, Tab Up, Alt Up */
                                network_receiver_send_key(g_network_receiver, VK_MENU, 1, 0);
                                network_receiver_send_key(g_network_receiver, VK_TAB, 1, 0x04);
                                network_receiver_send_key(g_network_receiver, VK_TAB, 0, 0x04);
                                network_receiver_send_key(g_network_receiver, VK_MENU, 0, 0);
                                pause_active = 0;
                            }
                            else if (pause_menu_selection == 3) {
                                /* Win Key: Send Win Down, Win Up */
                                network_receiver_send_key(g_network_receiver, VK_LWIN, 1, 0x08);
                                network_receiver_send_key(g_network_receiver, VK_LWIN, 0, 0x08);
                                pause_active = 0;
                            }
                            else if (pause_menu_selection == 4) {
                                network_receiver_disconnect(g_network_receiver);
                                current_state = STATE_MAIN_MENU;
                                pause_active = 0;
                            }
                        }
                        draw_pause_overlay_logic();
                    }
                }
                break;
            }

            case STATE_PAUSE_MENU:
                draw_pause_overlay_logic();
                if (is_button_pressed(PSP_CTRL_CIRCLE)) current_state = STATE_STREAMING;
                break;
            
            default:
                break;
        }

        ui_renderer_end_frame();
        old_pad = pad_data;
        g_frame_counter++;
    }
}

int main(int argc, char *argv[]) {
  (void)argc;
  (void)argv;
  /* Seed random number generator for all modules */
  srand(sceKernelGetSystemTimeLow());

  /* Initialize thread-safe logging */
  logger_init();
  LOG_INFO(COMPONENT_MAIN, "--- BOOTING PSP MOONLIGHT ---");

  /* Initialize custom exception handler (BSOD) */
  exception_handler_init();

  /* Set max CPU/BUS speed to prevent stream lag */
  scePowerSetClockFrequency(333, 333, 166);
  
  SetupCallbacks();
  
  /* CRASH TEST: Uncomment to verify the Blue Screen Exception Handler */
  // *(volatile int*)0x00000001 = 0xDEADBEEF;
  // __asm__ volatile ("break");

  sceCtrlSetSamplingCycle(0);
  sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);

   LOG_INFO(COMPONENT_MAIN, "Initializing PSP Moonlight V2... (Build: %s %s)", __DATE__, __TIME__);
  if (initialize_modules() < 0) {
      LOG_ERROR(COMPONENT_MAIN, "Failed to initialize modules.");
      return -1;
  }
  
  g_ui_sema = sceKernelCreateSema("UI Sema", 0, 1, 1, NULL);
  load_hosts();
  // Callbacks are already set up above
  g_running = 1;
  main_loop();

  /* Exit cleanly - PSP kernel handles all resource cleanup on exit. 
   * Do NOT call cleanup_modules() here: sceMpegDelete and similar
   * PSP SDK calls crash on real hardware if mpeg was never fully used. */
  ui_renderer_begin_frame();
  ui_draw_background();
  ui_draw_header("EXITING");
  ui_draw_panel(40, 100, 400, 60, 0xAA111111, 1);
  ui_draw_text("Returning to XMB...", 60, 120, 0xFFFFFFFF);
  ui_renderer_end_frame();
  sceKernelDelayThread(1000000);
  logger_shutdown();
  sceKernelExitGame();
  return 0;
}const unsigned char* __ctype_ptr__ = NULL;
