/**
 * @file ui_renderer.c
 * @brief Premium GU-based UI renderer for PSP Moonlight
 */

#ifndef UI_RENDERER_C
#define UI_RENDERER_C

#include "ui_renderer.h"
#include <pspgu.h>
#include <pspgum.h>
#include <pspdisplay.h>
#include <pspkernel.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define SCR_WIDTH  480
#define SCR_HEIGHT 272
#define BUF_WIDTH  512

/** Shared display list for all GU modules, defined in main.c */
extern unsigned int __attribute__((aligned(16))) g_gu_display_list[65536];

/* Simplified font for all common chars (0-9, A-Z, space, punctuation) */
static const unsigned char font_basic[128][8] = {
    [' ']={0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    ['.']={0x00,0x00,0x00,0x00,0x00,0x60,0x60,0x00},
    [':']={0x00,0x00,0x18,0x18,0x00,0x18,0x18,0x00},
    ['-']={0x00,0x00,0x00,0x3C,0x00,0x00,0x00,0x00},
    ['_']={0x00,0x00,0x00,0x00,0x00,0x00,0xFF,0x00},
    ['[']={0x1C,0x10,0x10,0x10,0x10,0x10,0x1C,0x00},
    [']']={0x38,0x08,0x08,0x08,0x08,0x08,0x38,0x00},
    ['(']={0x0C,0x10,0x10,0x10,0x10,0x10,0x0C,0x00},
    [')']={0x30,0x08,0x08,0x08,0x08,0x08,0x30,0x00},
    ['|']={0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x00},
    ['/']={0x02,0x04,0x08,0x10,0x20,0x40,0x80,0x00},
    ['\\']={0x80,0x40,0x20,0x10,0x08,0x04,0x02,0x00},
    ['0']={0x3C,0x42,0x46,0x4A,0x52,0x62,0x3C,0x00},
    ['1']={0x18,0x28,0x08,0x08,0x08,0x08,0x18,0x00},
    ['2']={0x3C,0x42,0x02,0x3C,0x40,0x40,0x7E,0x00},
    ['3']={0x3C,0x42,0x02,0x1C,0x02,0x42,0x3C,0x00},
    ['4']={0x08,0x18,0x28,0x48,0x7E,0x08,0x08,0x00},
    ['5']={0x7E,0x40,0x40,0x7C,0x02,0x02,0x7C,0x00},
    ['6']={0x3C,0x40,0x40,0x7C,0x42,0x42,0x3C,0x00},
    ['7']={0x7E,0x02,0x04,0x08,0x10,0x20,0x40,0x00},
    ['8']={0x3C,0x42,0x42,0x3C,0x42,0x42,0x3C,0x00},
    ['9']={0x3C,0x42,0x42,0x3E,0x02,0x02,0x3C,0x00},
    ['A']={0x18,0x24,0x42,0x42,0x7E,0x42,0x42,0x00},
    ['B']={0x7C,0x42,0x42,0x7C,0x42,0x42,0x7C,0x00},
    ['C']={0x3C,0x42,0x40,0x40,0x40,0x42,0x3C,0x00},
    ['D']={0x78,0x44,0x42,0x42,0x42,0x44,0x78,0x00},
    ['E']={0x7E,0x40,0x40,0x78,0x40,0x40,0x7E,0x00},
    ['F']={0x7E,0x40,0x40,0x78,0x40,0x40,0x40,0x00},
    ['G']={0x3C,0x42,0x40,0x4E,0x42,0x42,0x3C,0x00},
    ['H']={0x42,0x42,0x42,0x7E,0x42,0x42,0x42,0x00},
    ['I']={0x3C,0x18,0x18,0x18,0x18,0x18,0x3C,0x00},
    ['J']={0x1E,0x04,0x04,0x04,0x04,0x44,0x38,0x00},
    ['K']={0x42,0x44,0x48,0x70,0x48,0x44,0x42,0x00},
    ['L']={0x40,0x40,0x40,0x40,0x40,0x40,0x7E,0x00},
    ['M']={0x42,0x66,0x5A,0x42,0x42,0x42,0x42,0x00},
    ['N']={0x42,0x62,0x52,0x4A,0x46,0x42,0x42,0x00},
    ['O']={0x3C,0x42,0x42,0x42,0x42,0x42,0x3C,0x00},
    ['P']={0x7C,0x42,0x42,0x7C,0x40,0x40,0x40,0x00},
    ['Q']={0x3C,0x42,0x42,0x42,0x4A,0x44,0x3A,0x00},
    ['R']={0x7C,0x42,0x42,0x7C,0x48,0x44,0x42,0x00},
    ['S']={0x3C,0x42,0x40,0x3C,0x02,0x42,0x3C,0x00},
    ['T']={0x7E,0x18,0x18,0x18,0x18,0x18,0x18,0x00},
    ['U']={0x42,0x42,0x42,0x42,0x42,0x42,0x3C,0x00},
    ['V']={0x42,0x42,0x42,0x42,0x42,0x24,0x18,0x00},
    ['W']={0x42,0x42,0x42,0x42,0x5A,0x66,0x42,0x00},
    ['X']={0x42,0x42,0x24,0x18,0x24,0x42,0x42,0x00},
    ['Y']={0x42,0x42,0x42,0x3C,0x18,0x18,0x18,0x00},
    ['Z']={0x7E,0x02,0x04,0x18,0x20,0x40,0x7E,0x00},
    ['a']={0x00,0x00,0x3C,0x02,0x3E,0x42,0x3E,0x00},
    ['b']={0x40,0x40,0x7C,0x42,0x42,0x42,0x7C,0x00},
    ['c']={0x00,0x00,0x3C,0x40,0x40,0x42,0x3C,0x00},
    ['d']={0x02,0x02,0x3E,0x42,0x42,0x42,0x3E,0x00},
    ['e']={0x00,0x00,0x3C,0x42,0x7E,0x40,0x3C,0x00},
    ['f']={0x1C,0x22,0x20,0x7C,0x20,0x20,0x20,0x00},
    ['g']={0x00,0x3E,0x42,0x42,0x3E,0x02,0x3C,0x00},
    ['h']={0x40,0x40,0x7C,0x42,0x42,0x42,0x42,0x00},
    ['i']={0x18,0x00,0x38,0x18,0x18,0x18,0x3C,0x00},
    ['j']={0x00,0x0C,0x00,0x0C,0x04,0x44,0x38,0x00},
    ['k']={0x40,0x40,0x44,0x48,0x70,0x48,0x44,0x00},
    ['l']={0x38,0x18,0x18,0x18,0x18,0x18,0x3C,0x00},
    ['m']={0x00,0x00,0x76,0x49,0x49,0x49,0x49,0x00},
    ['n']={0x00,0x00,0x7C,0x42,0x42,0x42,0x42,0x00},
    ['o']={0x00,0x00,0x3C,0x42,0x42,0x42,0x3C,0x00},
    ['p']={0x00,0x7C,0x42,0x42,0x7C,0x40,0x40,0x00},
    ['q']={0x00,0x3E,0x42,0x42,0x3E,0x02,0x02,0x00},
    ['r']={0x00,0x00,0x5C,0x62,0x40,0x40,0x40,0x00},
    ['s']={0x00,0x00,0x3E,0x40,0x3C,0x02,0x7C,0x00},
    ['t']={0x20,0x20,0x7C,0x20,0x20,0x22,0x1C,0x00},
    ['u']={0x00,0x00,0x42,0x42,0x42,0x46,0x3A,0x00},
    ['v']={0x00,0x00,0x42,0x42,0x42,0x24,0x18,0x00},
    ['w']={0x00,0x00,0x42,0x42,0x5A,0x66,0x42,0x00},
    ['x']={0x00,0x00,0x42,0x24,0x18,0x24,0x42,0x00},
    ['y']={0x00,0x42,0x42,0x42,0x3E,0x02,0x3C,0x00},
    ['z']={0x00,0x00,0x7E,0x08,0x10,0x20,0x7E,0x00},
    ['<']={0x00,0x04,0x08,0x10,0x08,0x04,0x00,0x00},
    ['>']={0x00,0x20,0x10,0x08,0x10,0x20,0x00,0x00},
    ['?']={0x3C,0x42,0x02,0x0C,0x10,0x00,0x10,0x00},
    ['!']={0x18,0x18,0x18,0x18,0x18,0x00,0x18,0x00},
    [',']={0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x08},

};

struct __attribute__((aligned(4))) UIVertex {
    unsigned int color;
    short x, y, z, pad;
};

int ui_renderer_init(void) {
    sceGuInit();
    sceGuStart(GU_DIRECT, g_gu_display_list);

    sceGuDrawBuffer(GU_PSM_5650, (void *)0, BUF_WIDTH);
    sceGuDispBuffer(SCR_WIDTH, SCR_HEIGHT, (void *)(BUF_WIDTH * SCR_HEIGHT * 2), BUF_WIDTH);

    sceGuOffset(2048 - (SCR_WIDTH / 2), 2048 - (SCR_HEIGHT / 2));
    sceGuViewport(2048, 2048, SCR_WIDTH, SCR_HEIGHT);
    sceGuDepthRange(65535, 0);

    sceGuScissor(0, 0, SCR_WIDTH, SCR_HEIGHT);
    sceGuEnable(GU_SCISSOR_TEST);

    sceGuTexFilter(GU_LINEAR, GU_LINEAR);
    sceGuTexMode(GU_PSM_5650, 0, 0, 0);
    sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA);
    
    sceGuDisable(GU_TEXTURE_2D);
    sceGuDisable(GU_DEPTH_TEST);
    
    sceGuClearColor(0xFF000000);
    sceGuClear(GU_COLOR_BUFFER_BIT);
    sceGuFinish();
    sceGuSync(0, 0);
    
    sceDisplayWaitVblankStart();
    sceGuDisplay(GU_TRUE);
    return 0;
}

void ui_renderer_shutdown(void) {
    sceGuTerm();
}

void ui_renderer_begin_frame(void) {
    sceGuStart(GU_DIRECT, g_gu_display_list);
    sceGuScissor(0, 0, SCR_WIDTH, SCR_HEIGHT);
    sceGuClearColor(0xFF000000);
    sceGuClear(GU_COLOR_BUFFER_BIT);
}

void ui_renderer_end_frame(void) {
    sceGuFinish();
    sceGuSync(0, 0);
    sceDisplayWaitVblankStart();
    sceGuSwapBuffers();
}

void ui_draw_background(void) {
    /* Absolute Perfection: Solid Dark Grey background for premium look */
    struct UIVertex* v = (struct UIVertex*)sceGuGetMemory(2 * sizeof(struct UIVertex));
    v[0].color = 0xFF111111; v[0].x = 0; v[0].y = 0; v[0].z = 0;
    v[1].color = 0xFF111111; v[1].x = 480; v[1].y = 272; v[1].z = 0;

    sceGuDisable(GU_TEXTURE_2D);
    sceGuDisable(GU_BLEND);
    sceGuDrawArray(GU_SPRITES, GU_COLOR_8888|GU_VERTEX_16BIT|GU_TRANSFORM_2D, 2, 0, v);

    /* Stars / Particle Overlay */
    static struct UIVertex stars[50];
    static int stars_init = 0;
    if (!stars_init) {
        for (int i = 0; i < 50; i++) {
            stars[i].x = rand() % 480;
            stars[i].y = rand() % 272;
            stars[i].z = 0;
            stars[i].color = 0x88FFFFFF;
        }
        stars_init = 1;
    }
    
    sceGuEnable(GU_BLEND);
    sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
    sceKernelDcacheWritebackInvalidateRange(stars, 50 * sizeof(struct UIVertex));
    sceGuDrawArray(GU_POINTS, GU_COLOR_8888|GU_VERTEX_16BIT|GU_TRANSFORM_2D, 50, 0, stars);
}

void ui_draw_panel(int x, int y, int w, int h, unsigned int color, int border) {
    if ((color >> 24) < 0xFF) {
        sceGuEnable(GU_BLEND);
        sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
    } else {
        sceGuDisable(GU_BLEND);
    }

    struct UIVertex* v = (struct UIVertex*)sceGuGetMemory(2 * sizeof(struct UIVertex));
    v[0].color = color; v[0].x = x; v[0].y = y; v[0].z = 0;
    v[1].color = color; v[1].x = x + w; v[1].y = y + h; v[1].z = 0;
    sceGuDrawArray(GU_SPRITES, GU_COLOR_8888|GU_VERTEX_16BIT|GU_TRANSFORM_2D, 2, 0, v);

    if (border) {
        sceGuEnable(GU_BLEND);
        sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
        unsigned int border_color = 0x88FFFFFF;
        struct UIVertex* bv = (struct UIVertex*)sceGuGetMemory(5 * sizeof(struct UIVertex));
        bv[0].color=border_color; bv[0].x=x; bv[0].y=y; bv[0].z=0;
        bv[1].color=border_color; bv[1].x=x+w; bv[1].y=y; bv[1].z=0;
        bv[2].color=border_color; bv[2].x=x+w; bv[2].y=y+h; bv[2].z=0;
        bv[3].color=border_color; bv[3].x=x; bv[3].y=y+h; bv[3].z=0;
        bv[4].color=border_color; bv[4].x=x; bv[4].y=y; bv[4].z=0;
        sceGuDrawArray(GU_LINE_STRIP, GU_COLOR_8888|GU_VERTEX_16BIT|GU_TRANSFORM_2D, 5, 0, bv);
    }
}

void ui_draw_text(const char* text, int x, int y, unsigned int color) {
    if (!text) return;
    int len = strlen(text);
    if (len == 0) return;
    
    int max_vcount = len * 64; 
    struct UIVertex* v_base = (struct UIVertex*)sceGuGetMemory(max_vcount * sizeof(struct UIVertex));
    if (!v_base) return;
    
    int v_idx = 0;
    
    for (int i = 0; i < len; i++) {
        unsigned char c = (unsigned char)text[i];
        if (c > 127 || c < 32) c = ' ';
        
        for (int row = 0; row < 8; row++) {
            unsigned char bits = font_basic[c][row];
            if (bits == 0) continue;
            
            int start_col = -1;
            for (int col = 0; col < 8; col++) {
                if (bits & (1 << (7 - col))) {
                    if (start_col == -1) start_col = col;
                } else {
                    if (start_col != -1) {
                        if (v_idx + 2 <= max_vcount) {
                            int width = col - start_col;
                            v_base[v_idx].color = color; v_base[v_idx].x = x + (i * 8) + start_col; v_base[v_idx].y = y + row; v_base[v_idx].z = 0;
                            v_base[v_idx+1].color = color; v_base[v_idx+1].x = x + (i * 8) + start_col + width; v_base[v_idx+1].y = y + row + 1; v_base[v_idx+1].z = 0;
                            v_idx += 2;
                        }
                        start_col = -1;
                    }
                }
            }
            if (start_col != -1) {
                if (v_idx + 2 <= max_vcount) {
                    int width = 8 - start_col;
                    v_base[v_idx].color = color; v_base[v_idx].x = x + (i * 8) + start_col; v_base[v_idx].y = y + row; v_base[v_idx].z = 0;
                    v_base[v_idx+1].color = color; v_base[v_idx+1].x = x + (i * 8) + start_col + width; v_base[v_idx+1].y = y + row + 1; v_base[v_idx+1].z = 0;
                    v_idx += 2;
                }
            }
        }
    }

    if (v_idx > 0) {
        sceGuEnable(GU_BLEND);
        sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
        sceGuDisable(GU_TEXTURE_2D);
        sceGuDrawArray(GU_SPRITES, GU_COLOR_8888|GU_VERTEX_16BIT|GU_TRANSFORM_2D, v_idx, 0, v_base);
        sceGuDisable(GU_BLEND);
    }
}

void ui_draw_header(const char* title) {
    ui_draw_panel(0, 0, SCR_WIDTH, 40, 0xCC442200, 0);
    ui_draw_text(title, 20, 16, 0xFF00FFFF);
    
    unsigned int glow = 0xFF884400;
    struct UIVertex* v = (struct UIVertex*)sceGuGetMemory(2 * sizeof(struct UIVertex));
    v[0].color = glow; v[0].x = 0; v[0].y = 38; v[0].z = 0;
    v[1].color = glow; v[1].x = 480; v[1].y = 40; v[1].z = 0;
    sceGuDrawArray(GU_SPRITES, GU_COLOR_8888|GU_VERTEX_16BIT|GU_TRANSFORM_2D, 2, 0, v);
}

void ui_draw_menu_item(const char* label, int x, int y, int width, int selected) {
    ui_draw_panel(x, y, width, 24, selected ? 0xAAFFBB00 : 0x44AAAAAA, selected);
    ui_draw_text(label, x + 8, y + 8, selected ? 0xFFFFFFFF : 0xFFCCCCCC);
}

void ui_draw_status_bar(const char* text) {
    ui_draw_panel(0, SCR_HEIGHT - 20, SCR_WIDTH, 20, 0xCC000000, 0);
    ui_draw_text(text, 10, SCR_HEIGHT - 16, 0xFFCCCCCC);
    
    unsigned int glow = 0xFF553300;
    struct UIVertex* v = (struct UIVertex*)sceGuGetMemory(2 * sizeof(struct UIVertex));
    v[0].color = glow; v[0].x = 0; v[0].y = SCR_HEIGHT - 20; v[0].z = 0;
    v[1].color = glow; v[1].x = 480; v[1].y = SCR_HEIGHT - 19; v[1].z = 0;
    sceGuDrawArray(GU_SPRITES, GU_COLOR_8888|GU_VERTEX_16BIT|GU_TRANSFORM_2D, 2, 0, v);
}

void ui_draw_wifi_selector(const char* profiles[], int count, int selected) {
    ui_draw_background();
    ui_draw_header("MOONLIGHT WI-FI SETUP");
    ui_draw_panel(20, 50, 440, 160, 0xAA222222, 1);
    ui_draw_text("Select a Wi-Fi connection (Up/Down + X):", 40, 60, 0xFFFFFFFF);
    
    for (int i = 0; i < count && i < 5; i++) {
        ui_draw_menu_item(profiles[i], 40, 80 + (i * 24), 400, (selected == i));
    }
    
    ui_draw_status_bar("Press O to Cancel");
}

void ui_draw_wifi_status(const char* ssid, const char* state_str) {
    ui_draw_background();
    ui_draw_header("CONNECTING TO WI-FI");
    ui_draw_panel(40, 100, 400, 80, 0xAA222222, 1);
    
    char buf[128];
    snprintf(buf, sizeof(buf), "Network: %s", ssid);
    ui_draw_text(buf, 60, 120, 0xFFFFFFFF);
    
    snprintf(buf, sizeof(buf), "Status: %s", state_str);
    ui_draw_text(buf, 60, 145, 0xFF00FFFF); /* Opaque Cyan/Yellow */
    
    ui_draw_status_bar("Warming up WLAN radio...");
}

#endif
