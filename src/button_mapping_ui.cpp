/*
 * button_mapping_ui.cpp - Vertical scrolling button mapping menu
 */

#include <pspkernel.h>
#include <pspctrl.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

extern "C" {
#include "ui_manager.h"
#include "input.h"
}

#define SCREEN_W      480
#define SCREEN_H      272
#define HEADER_H      32
#define FOOTER_H      26

#define ITEM_W        360
#define ITEM_H        44
#define ITEM_PAD      6   /* step=50: 4 items end at y=235 unscaled; scaled 1.05→y=236, 8px gap to footer */

#define NUM_MAPPINGS  8

static int s_selected = 0;
static float s_scroll_curr = 0.0f;
static float s_focus_anim = 0.0f;
static ButtonMapping s_mapping;

static const char *getItemName(int index) {
    switch (index) {
        case 0: return "Virtual L2 Trigger";
        case 1: return "Virtual R2 Trigger";
        case 2: return "Right Stick Up";
        case 3: return "Right Stick Down";
        case 4: return "Right Stick Left";
        case 5: return "Right Stick Right";
        case 6: return "Left Stick Click (L3)";
        case 7: return "Right Stick Click (R3)";
    }
    return "";
}

static const char *getPSPButtonToken(uint32_t btn) {
    if (btn == 0)                 return "None";
    if (btn == PSP_CTRL_CROSS)    return "{X}";
    if (btn == PSP_CTRL_CIRCLE)   return "{O}";
    if (btn == PSP_CTRL_SQUARE)   return "{SQ}";
    if (btn == PSP_CTRL_TRIANGLE) return "{TR}";
    if (btn == PSP_CTRL_UP)       return "{UP}";
    if (btn == PSP_CTRL_DOWN)     return "{DN}";
    if (btn == PSP_CTRL_LEFT)     return "{LF}";
    if (btn == PSP_CTRL_RIGHT)    return "{RF}";
    if (btn == PSP_CTRL_LTRIGGER) return "{L}";
    if (btn == PSP_CTRL_RTRIGGER) return "{R}";
    if (btn == PSP_CTRL_START)    return "{ST}";
    if (btn == PSP_CTRL_SELECT)   return "{SE}";
    return "?";
}

static uint32_t *getMappedButtonPtr(int index) {
    switch(index) {
        case 0: return &s_mapping.l2_button;
        case 1: return &s_mapping.r2_button;
        case 2: return &s_mapping.rs_up_button;
        case 3: return &s_mapping.rs_down_button;
        case 4: return &s_mapping.rs_left_button;
        case 5: return &s_mapping.rs_right_button;
        case 6: return &s_mapping.l3_button;
        case 7: return &s_mapping.r3_button;
    }
    return NULL;
}

static float s_target_camera = 0.0f;

static void update_animations(void) {
    float sel = (float)s_selected;
    if (sel < s_target_camera) {
        s_target_camera = sel;
    } else if (sel > s_target_camera + 3.0f) { /* 4 items visible exactly between headers */
        s_target_camera = sel - 3.0f;
    }

    s_scroll_curr += (s_target_camera - s_scroll_curr) * 0.15f;
    s_focus_anim  += (sel - s_focus_anim) * 0.20f;
}

static int tile_screen_pos(int idx, int *out_x, int *out_y) {
    int start_y = 41; /* Starts after header height + padding */
    
    *out_x = (SCREEN_W - ITEM_W) / 2;
    *out_y = start_y + (int)(((float)idx - s_scroll_curr) * (float)(ITEM_H + ITEM_PAD));
    
    if (*out_y + ITEM_H < 0) return 0;
    if (*out_y > SCREEN_H) return 0;
    return 1;
}

static uint32_t prompt_mapping(int index) {
    /* Flush pad */
    SceCtrlData pad, prev;
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);
    
    /* Wait for release */
    do {
        sceCtrlPeekBufferPositive(&pad, 1);
        sceKernelDelayThread(16667);
    } while (pad.Buttons != 0);

    sceCtrlPeekBufferPositive(&pad, 1);
    prev = pad;

    while (1) {
        sceCtrlPeekBufferPositive(&pad, 1);
        uint32_t pressed = pad.Buttons & ~prev.Buttons;
        if (pressed != 0) {
            /* Find lowest set bit */
            uint32_t mapped = 0;
            for(int i=0; i<32; i++) {
                if (pressed & (1<<i)) {
                    mapped = (1<<i);
                    break;
                }
            }
            if (mapped == PSP_CTRL_HOME || mapped == PSP_CTRL_NOTE || mapped == PSP_CTRL_SCREEN || mapped == PSP_CTRL_VOLUP || mapped == PSP_CTRL_VOLDOWN) {
                // ignore system buttons
            } else {
                return mapped;
            }
        }
        prev = pad;

        ui_begin_frame();
        ui_draw_gradient_bg(UI_COL_BG_TOP, UI_COL_BG_BOT);
        
        ui_draw_rect_rounded((SCREEN_W - 300) / 2, (SCREEN_H - 100) / 2, 300, 100, 12, UI_COL_PANEL);
        ui_draw_border((SCREEN_W - 300) / 2, (SCREEN_H - 100) / 2, 300, 100, 2, UI_COL_BORDER_FOC);
        
        ui_draw_text_centered((float)SCREEN_W / 2, 0.0f, (float)(SCREEN_H / 2 - 10), g_ui_text_foc_color ? g_ui_text_foc_color : 0xFFFFFFFF, getItemName(index));
        ui_draw_text_centered((float)SCREEN_W / 2, 0.0f, (float)(SCREEN_H / 2 + 15), 0xFFDDDDDD, "Press new button to map...");
        
        ui_end_frame();
    }
    return 0;
}

extern "C" void button_mapping_ui_run(void)
{
    s_selected = 0;
    s_scroll_curr   = 0.0f;
    s_target_camera = 0.0f;   /* reset stale camera from prior session */
    s_focus_anim    = 0.0f;

    button_mapping_get(&s_mapping);

    /* Flush stales */
    SceCtrlData pad, prev;
    do {
        sceCtrlPeekBufferPositive(&pad, 1);
        sceKernelDelayThread(16667);
    } while (pad.Buttons != 0);
    prev = pad;

    while (1) {
        update_animations();

        sceCtrlPeekBufferPositive(&pad, 1);
        uint32_t pressed = pad.Buttons & ~prev.Buttons;
        prev = pad;

        if (pressed & PSP_CTRL_UP) {
            if (s_selected > 0) s_selected--;
        }
        if (pressed & PSP_CTRL_DOWN) {
            if (s_selected < NUM_MAPPINGS - 1) s_selected++;
        }
        
        if (pressed & PSP_CTRL_CIRCLE) {
            button_mapping_set(&s_mapping);
            break; /* Back to settings menu */
        }
        
        /* SQUARE: clear the focused mapping (disables that virtual button) */
        if (pressed & PSP_CTRL_SQUARE) {
            uint32_t *ptr = getMappedButtonPtr(s_selected);
            if (ptr) *ptr = 0;
        }

        /* SELECT: reset ALL mappings to factory defaults (confirm on O to persist) */
        if (pressed & PSP_CTRL_SELECT) {
            s_mapping.l2_button       = PSP_CTRL_CROSS;
            s_mapping.r2_button       = PSP_CTRL_SQUARE;
            s_mapping.rs_up_button    = PSP_CTRL_UP;
            s_mapping.rs_down_button  = PSP_CTRL_DOWN;
            s_mapping.rs_left_button  = PSP_CTRL_LEFT;
            s_mapping.rs_right_button = PSP_CTRL_RIGHT;
            s_mapping.l3_button       = PSP_CTRL_TRIANGLE;
            s_mapping.r3_button       = PSP_CTRL_CIRCLE;
        }
        
        if (pressed & PSP_CTRL_CROSS) {
            uint32_t new_btn = prompt_mapping(s_selected);
            if (new_btn != 0) {
                uint32_t *ptr = getMappedButtonPtr(s_selected);
                if (ptr) *ptr = new_btn;
            }
            /* Flush after prompt */
            do {
                sceCtrlPeekBufferPositive(&pad, 1);
                sceKernelDelayThread(16667);
            } while (pad.Buttons != 0);
            prev = pad;
        }

        ui_begin_frame();
        ui_draw_gradient_bg(UI_COL_BG_TOP, UI_COL_BG_BOT);

        /* Clamp all pill drawing to the safe zone between header and footer */
        ui_set_scissor(0, 30, SCREEN_W, 214); /* y=30..244: matches settings menu gap fix */

        for (int i = 0; i < NUM_MAPPINGS; i++) {
            int tx, ty;
            if (!tile_screen_pos(i, &tx, &ty)) continue;

            float dist = fabsf((float)i - s_focus_anim);
            float scale = (dist < 1.0f) ? (1.0f + (1.0f - dist) * 0.05f) : 1.0f;

            int tw = (int)((float)ITEM_W * scale);
            int th = (int)((float)ITEM_H * scale);
            int ox = tx - (tw - ITEM_W) / 2;
            int oy = ty - (th - ITEM_H) / 2;
            int focused = (i == s_selected);

            u32 bg_col = focused ? g_ui_sel_color : g_ui_card_color;
            u32 brd_col = focused ? UI_COL_BORDER_FOC : UI_COL_BORDER;
            int t = focused ? 2 : 1;
            int radius = th / 2;

            /* 3-layer drop shadow — grows +2px when focused for hover effect */
            ui_set_blend(1);
            {
                int so = focused ? 1 : 0;
                ui_draw_rect_rounded(ox + 3 + so, oy + 3 + so, tw, th, radius, 0x18000000u);
                ui_draw_rect_rounded(ox + 2 + so, oy + 2 + so, tw, th, radius, 0x28000000u);
                ui_draw_rect_rounded(ox + 1 + so, oy + 1 + so, tw, th, radius, 0x38000000u);
            }

            ui_draw_rect_rounded(ox, oy, tw, th, radius, brd_col);
            ui_draw_rect_rounded(ox + t, oy + t, tw - 2*t, th - 2*t, radius - t, bg_col);
            ui_set_blend(0);

            // Draw Label (Left aligned)
            ui_draw_text((float)(ox + 16), (float)(oy + th / 2 + 5), focused ? UI_COL_TEXT_FOCUS : UI_COL_TEXT, getItemName(i));

            // Draw Value (inline badge rendering, right portion of card)
            uint32_t *ptr = getMappedButtonPtr(i);
            const char *tok = ptr ? getPSPButtonToken(*ptr) : "?";
            char map_text[32];
            snprintf(map_text, sizeof(map_text), "{L} + %s", tok);
            ui_draw_text_inline((float)(ox + tw - 90), (float)(oy + th / 2 + 5),
                                focused ? UI_COL_TEXT_FOCUS : UI_COL_TEXT_DIM, map_text);
        }

        /* Restore full-screen scissor so header/footer draw unclipped */
        ui_clear_scissor();

        ui_draw_header("Custom Button Mapping");

        char progress[32];
        snprintf(progress, sizeof(progress), "%d / %d", s_selected + 1, NUM_MAPPINGS);
        ui_draw_text_right((float)(SCREEN_W - 18), 22.0f, UI_COL_TEXT_DIM, progress);

        ui_draw_footer_hint("{X}: Map  {SQ}: Clear  {SE}: Defaults  {O}: Save");
        ui_end_frame();
    }
}
