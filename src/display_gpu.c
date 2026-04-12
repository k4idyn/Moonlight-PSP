/*
 * display_gpu.c - PSP GE (Graphics Engine) display driver
 *
 * Ported from PSPdisp's proven graphic.c display pipeline, adapted for
 * Moonlight's streaming architecture.  Uses PSPdisp's battle-tested
 * patterns that work reliably on real PSP hardware over WiFi:
 *
 *   - Full GU state initialisation (alpha, blend, depth, shade model)
 *   - sceGum 3D-transform pipeline with orthographic projection
 *   - 32-pixel sliced blitting (works around GPU texture cache limits)
 *   - Rotation support via sceGumRotateZ matrix transforms
 *   - Proper texture scale/offset management
 *   - sceKernelDcacheWritebackInvalidateAll() for cache coherency
 *
 * Both H.264 (sceMpeg) and JPEG (sceJpeg) decoded RGBA8888 frames
 * are rendered through this unified display path.
 */

#include <pspkernel.h>
#include <pspdisplay.h>
#include <pspgu.h>
#include <pspgum.h>
#include <psptypes.h>
#include <stdio.h>
#include <string.h>

#include "shared.h"

/* Unified resolution table — single source of truth for stream dimensions */
#include "stream_resolution.h"

/* Use g_psp_config for stream resolution (GPU upscale sub-native res) */
#include "settings_menu.h"
extern PspConfig g_psp_config;

/* ------------------------------------------------------------------ *
 * Constants — matches PSPdisp's graphic.h proven values
 * ------------------------------------------------------------------ */
#define BUF_WIDTH   512
#define SCR_WIDTH   480
#define SCR_HEIGHT  272

/* GU command list — 256 KB, 16-byte aligned (PSPdisp uses 1 MB but
 * 256K is sufficient for video frame + HUD overlay) */
static u32 __attribute__((aligned(16))) display_list[256 * 1024 / 4];

/* Frame buffer pointer — tracks current draw target for double-buffering */
static void *s_frame_buffer = NULL;

/* Last frame tracking — enables menu overlay redraw (PSPdisp pattern) */
static void *s_last_frame_data = NULL;

/* ------------------------------------------------------------------ *
 * Vertex struct for 2D textured sprite (proven PSP hardware approach)
 *
 * Uses 16-bit texture + vertex coords with GU_TRANSFORM_2D for direct
 * texel-to-pixel mapping.  No matrix transforms, no floating point
 * precision issues.  This is the standard way to blit textures on PSP.
 * ------------------------------------------------------------------ */
typedef struct {
    u16 u, v;       /* texture coordinates in texels */
    s16 x, y, z;    /* screen position in pixels     */
} SpriteVertex;

/* display_setup_ortho removed — 2D sprite path bypasses all matrices */

/* ------------------------------------------------------------------ *
 * display_sliced_blit - 2D sprite sliced blit (proven PSP hardware)
 *
 * Draws the texture in vertical strips using GU_SPRITES with
 * GU_TRANSFORM_2D.  This bypasses the 3D transform pipeline entirely:
 *   - Texture coords are raw texel values (no tex matrix)
 *   - Vertex coords are raw screen pixels (no proj/view/model)
 *   - 2 vertices per sprite (top-left + bottom-right)
 *
 * 64-pixel strip width works within the PSP GPU's 512-byte texture
 * cache line.  At 4 bytes/pixel (RGBA8888), 64 pixels = 256 bytes =
 * exactly half a cache line, ensuring no thrashing.
 * ------------------------------------------------------------------ */
static void display_sliced_blit(int dst_x, int dst_y,
                                int img_w, int img_h)
{
    const int slice = 64;

    for (int x = 0; x < img_w; x += slice) {
        int w = (x + slice > img_w) ? (img_w - x) : slice;

        SpriteVertex *v = (SpriteVertex *)sceGuGetMemory(
                              2 * sizeof(SpriteVertex));
        if (!v) break;

        /* Top-left corner */
        v[0].u = (u16)x;
        v[0].v = 0;
        v[0].x = (s16)(dst_x + x);
        v[0].y = (s16)dst_y;
        v[0].z = 0;

        /* Bottom-right corner */
        v[1].u = (u16)(x + w);
        v[1].v = (u16)img_h;
        v[1].x = (s16)(dst_x + x + w);
        v[1].y = (s16)(dst_y + img_h);
        v[1].z = 0;

        sceGuDrawArray(GU_SPRITES,
                       GU_TEXTURE_16BIT | GU_VERTEX_16BIT |
                       GU_TRANSFORM_2D,
                       2, NULL, v);
    }
}

/* ------------------------------------------------------------------ *
 * Float vertex struct for GPU-upscaled sub-native resolution blitting.
 * Float UVs give precise texel mapping when src != dst dimensions.
 * ------------------------------------------------------------------ */
typedef struct {
    float u, v;
    float x, y, z;
} FloatSpriteVertex;

/* ------------------------------------------------------------------ *
 * display_sliced_blit_upscale - GPU bilinear upscale for sub-native res
 *
 * Maps texture region (0,0)-(src_w,src_h) to screen (0,0)-(dst_w,dst_h).
 * Uses float UVs for precise texel-to-pixel mapping with bilinear filter.
 * 64-pixel screen-space slices for GPU texture cache efficiency.
 * ------------------------------------------------------------------ */
static void display_sliced_blit_upscale(int src_w, int src_h,
                                        int dst_w, int dst_h,
                                        int off_x, int off_y)
{
    const int slice = 64;
    float u_scale = (float)src_w / (float)dst_w;

    for (int x = 0; x < dst_w; x += slice) {
        int w = (x + slice > dst_w) ? (dst_w - x) : slice;

        FloatSpriteVertex *fv = (FloatSpriteVertex *)sceGuGetMemory(
                                    2 * sizeof(FloatSpriteVertex));
        if (!fv) break;

        fv[0].u = (float)x * u_scale;
        fv[0].v = 0.0f;
        fv[0].x = (float)(off_x + x);
        fv[0].y = (float)off_y;
        fv[0].z = 0.0f;

        fv[1].u = (float)(x + w) * u_scale;
        fv[1].v = (float)src_h;
        fv[1].x = (float)(off_x + x + w);
        fv[1].y = (float)(off_y + dst_h);
        fv[1].z = 0.0f;

        sceGuDrawArray(GU_SPRITES,
                       GU_TEXTURE_32BITF | GU_VERTEX_32BITF |
                       GU_TRANSFORM_2D,
                       2, NULL, fv);
    }
}

/* ------------------------------------------------------------------ *
 * display_init - Full GU initialisation (ported from PSPdisp graphicInit)
 *
 * PSPdisp's init sets up the complete GU state machine including alpha
 * test, blending, shade model, and depth buffer — all of which are
 * needed for reliable rendering alongside HUD overlays and menus.
 * ------------------------------------------------------------------ */
void display_init(void)
{
    s_frame_buffer = (void *)0;

    sceGuInit();
    sceGuStart(GU_DIRECT, display_list);

    /* Double-buffered RGBA8888 */
    sceGuDrawBuffer(GU_PSM_8888, (void *)0, BUF_WIDTH);
    sceGuDispBuffer(SCR_WIDTH, SCR_HEIGHT,
                    (void *)(BUF_WIDTH * SCR_HEIGHT * 4), BUF_WIDTH);
    sceGuDepthBuffer((void *)0x110000, BUF_WIDTH);

    /* Viewport — PSPdisp's proven offset calculation */
    sceGuOffset(2048 - (SCR_WIDTH / 2), 2048 - (SCR_HEIGHT / 2));
    sceGuViewport(2048, 2048, SCR_WIDTH, SCR_HEIGHT);
    sceGuDepthRange(0xc350, 0x2710);

    /* Scissor */
    sceGuScissor(0, 0, SCR_WIDTH, SCR_HEIGHT);
    sceGuEnable(GU_SCISSOR_TEST);

    /* Alpha test — needed for HUD overlay transparency */
    sceGuAlphaFunc(GU_GREATER, 0, 0xFF);
    sceGuEnable(GU_ALPHA_TEST);

    /* Depth — disabled for 2D but properly configured */
    sceGuDepthFunc(GU_GEQUAL);
    sceGuDisable(GU_DEPTH_TEST);

    /* Face culling — disabled for 2D quads */
    sceGuFrontFace(GU_CW);
    sceGuShadeModel(GU_SMOOTH);
    sceGuDisable(GU_CULL_FACE);

    /* Texturing */
    sceGuEnable(GU_TEXTURE_2D);
    sceGuTexMode(GU_PSM_8888, 0, 0, 0);
    sceGuTexFunc(GU_TFX_DECAL, GU_TCC_RGB);
    sceGuTexFilter(GU_NEAREST, GU_NEAREST);
    sceGuAmbientColor(0xFFFFFFFF);
    sceGuTexOffset(0.0f, 0.0f);
    sceGuTexScale(1.0f, 1.0f);

    /* Blending — PSPdisp pattern for alpha compositing */
    sceGuEnable(GU_BLEND);
    sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);

    sceGuFinish();
    sceGuSync(0, 0);

    sceDisplayWaitVblankStart();
    sceGuDisplay(GU_TRUE);
}

/* ------------------------------------------------------------------ *
 * display_frame - Render decoded RGBA8888 frame (PSPdisp drawFrame)
 *
 * Ported from PSPdisp's graphicDrawFrame — uses the sceGum matrix
 * pipeline with sliced blitting for maximum hardware compatibility.
 *
 * @frame_data:  Pointer to RGBA8888 pixel buffer (480x272 or rotated)
 *               Works with both H.264 (sceMpeg) and JPEG (sceJpeg) output.
 *               For uncached ME buffers, caller should pass | 0x40000000.
 * ------------------------------------------------------------------ */
void display_frame(void *frame_data)
{
    if (!frame_data)
        return;

    s_last_frame_data = frame_data;

    sceKernelDcacheWritebackInvalidateAll();

    sceGuStart(GU_DIRECT, display_list);

    /* Determine actual source dimensions from unified resolution table */
    int src_w = g_stream_res.initialized ? g_stream_res.width  : (g_psp_config.width  > 0 ? g_psp_config.width  : PSP_LCD_WIDTH);
    int src_h = g_stream_res.initialized ? g_stream_res.height : (g_psp_config.height > 0 ? g_psp_config.height : PSP_LCD_HEIGHT);

    /* Texture buffer width (stride in pixels) — from resolution table.
     * Must be power-of-2 and >= source width. */
    int tex_stride = g_stream_res.initialized ? g_stream_res.stride : ((src_w > 512) ? 1024 : 512);

    sceGuTexMode(GU_PSM_8888, 0, 0, 0);
    sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGB);

    if (src_w <= 512) {
        /* Single-pass: source fits in one 512-wide texture page.
         * Handles both sub-native (upscale) and native (1:1). */
        sceGuTexImage(0, BUF_WIDTH, 512, tex_stride, frame_data);

        if (src_w < SCR_WIDTH || src_h < SCR_HEIGHT) {
            sceGuTexFilter(GU_LINEAR, GU_LINEAR);
            display_sliced_blit_upscale(src_w, src_h, SCR_WIDTH, SCR_HEIGHT, 0, 0);
        } else {
            sceGuTexFilter(GU_NEAREST, GU_NEAREST);
            display_sliced_blit(0, 0, SCR_WIDTH, SCR_HEIGHT);
        }
    } else {
        /* Two-pass: source wider than 512 (e.g. 640x360).
         * PSP GPU max texture width is 512, so we render left and right
         * halves with different texture base pointers.
         * tex_stride=1024 ensures correct row addressing for both passes. */
        sceGuTexFilter(GU_LINEAR, GU_LINEAR);

        /* Pass 1: source columns 0–511 → left portion of screen */
        int pass1_src_end = 512;
        int pass1_scr_w = (int)((float)SCR_WIDTH * (float)pass1_src_end / (float)src_w);

        sceGuTexImage(0, 512, 512, tex_stride, frame_data);
        display_sliced_blit_upscale(pass1_src_end, src_h, pass1_scr_w, SCR_HEIGHT, 0, 0);

        /* Pass 2: source columns 512–src_w → right portion of screen.
         * Shift texture base by 512 pixels (2048 bytes at RGBA8888).
         * tex_stride=1024 ensures the GPU reads correct row offsets. */
        int pass2_src_w = src_w - 512;
        int pass2_scr_w = SCR_WIDTH - pass1_scr_w;
        u8 *pass2_base = (u8 *)frame_data + 512 * 4;

        sceGuTexImage(0, 512, 512, tex_stride, pass2_base);
        display_sliced_blit_upscale(pass2_src_w, src_h, pass2_scr_w, SCR_HEIGHT, pass1_scr_w, 0);
    }

    sceGuFinish();
    sceGuSync(0, 0);
}

/* ------------------------------------------------------------------ *
 * display_frame_finish - Swap buffers and VBlank sync
 *
 * Call AFTER hud_render() so HUD composites before the swap.
 * PSPdisp calls sceGuSwapBuffers() then VBlank — we reverse
 * the order (VBlank first) to match existing Moonlight timing
 * and avoid tearing.
 * ------------------------------------------------------------------ */
void display_frame_finish(void)
{
    sceDisplayWaitVblankStart();
    s_frame_buffer = sceGuSwapBuffers();
}

/* ------------------------------------------------------------------ *
 * display_frame_repeat - Re-render the last video frame
 *
 * Used when the HUD is visible but no new decoded frame arrived.
 * Re-blits the saved frame so the HUD can composite on top of it
 * without causing double-buffer flashing from stale back-buffer data.
 * ------------------------------------------------------------------ */
void display_frame_repeat(void)
{
    if (s_last_frame_data)
        display_frame(s_last_frame_data);
}

/* ------------------------------------------------------------------ *
 * display_clear - Clear screen to solid color (PSPdisp pattern)
 * Used during menus and connection screens.
 * ------------------------------------------------------------------ */
void display_clear(unsigned int color)
{
    sceGuStart(GU_DIRECT, display_list);
    sceGuClearColor(color);
    sceGuClearDepth(0);
    sceGuClear(GU_COLOR_BUFFER_BIT | GU_DEPTH_BUFFER_BIT);
    sceGuFinish();
    sceGuSync(0, 0);
}

/* ------------------------------------------------------------------ */
void display_shutdown(void)
{
    sceGuTerm();
}