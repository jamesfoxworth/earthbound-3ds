#include "platform/platform.h"
#include "game/window.h"
#include "core/types.h"
#include <3ds.h>
#include <string.h>
#include <stdio.h>

/*
 * Top screen presentation. The viewport is the full 400x240 panel, so the
 * centering offsets below are zero; they remain so a narrower viewport still
 * lands centered. The bottom screen currently hosts a debug console and will
 * become the touch UI.
 *
 * Scanlines are accumulated into a linear row-major staging buffer (cheap
 * sequential writes, stays cached), then transposed into the screen
 * framebuffer in a single pass at end_frame. The 3DS framebuffer is stored
 * column-major with Y flipped (the LCD panel is mounted in portrait), so
 * writing it directly from send_scanline() meant every consecutive pixel
 * landed a full column apart — one cache line per pixel, ~57k scattered
 * writes per frame. The transpose pass below instead walks the destination
 * contiguously (one full column per inner loop) and takes the strided
 * access on the small cached source buffer.
 */

#define PHYS_WIDTH  400
#define PHYS_HEIGHT 240

static pixel_t frame[EB_VIEWPORT_WIDTH * EB_VIEWPORT_HEIGHT];

/* Set by main.c from the launch keys when LOG_WARN output should own the
 * bottom screen. Suppresses the UI mirror (which needs a different pixel
 * format) and the transpose readout, so neither fights the console. */
bool platform_console_logging;

bool platform_video_init(void) {
    gfxInitDefault();
    /* gfxInitDefault() allocates BGR8 (3 bytes/pixel); the game renders
     * 16-bit pixels, so switch the top screen to RGB565. The matching
     * channel order comes from building the game library with
     * EB_PIXEL_RGB565=1 (see core/types.h). */
    gfxSetScreenFormat(GFX_TOP, GSP_RGB565_OES);
    gfxSetDoubleBuffering(GFX_TOP, true);
    gfxSetDoubleBuffering(GFX_BOTTOM, false);

    /* Bottom screen: either a debug console (launched with L held) or the UI
     * mirror, which needs the same 16-bit format as the staging frame it
     * copies from. stderr is swallowed unless main.c routes it to the console.
     * main.c reads the launch keys before calling this so the choice is known
     * here. */
    if (platform_console_logging) {
        consoleInit(GFX_BOTTOM, NULL);
    } else {
        gfxSetScreenFormat(GFX_BOTTOM, GSP_RGB565_OES);
        u16 bw = 0, bh = 0;
        u8 *bfb = gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, &bw, &bh);
        if (bfb)
            memset(bfb, 0, (size_t)bw * bh * sizeof(pixel_t));
        consoleDebugInit(debugDevice_NULL);
    }

    /* Only the centered 256x224 region is drawn each frame, so clear both
     * buffers once — otherwise the surrounding letterbox is uninitialized
     * memory that alternates as the buffers flip. */
    for (int i = 0; i < 2; i++) {
        u16 w = 0, h = 0;
        u8 *fb = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, &w, &h);
        if (fb)
            memset(fb, 0, (size_t)w * h * sizeof(pixel_t));
        gfxSwapBuffers();
    }

    /* New 3DS runs homebrew at the old 268MHz clock unless the app opts in.
     * No-op on an original 3DS. */
    osSetSpeedupEnable(true);
    return true;
}

void platform_video_shutdown(void) {
    gfxExit();
}

void platform_video_begin_frame(void) {
    /* Scanlines land in the staging buffer; nothing to prepare. */
}

void platform_video_send_scanline(int y, const pixel_t *pixels) {
    if (y < 0 || y >= EB_VIEWPORT_HEIGHT)
        return;
    memcpy(&frame[y * EB_VIEWPORT_WIDTH], pixels, EB_VIEWPORT_WIDTH * sizeof(pixel_t));
}

pixel_t *platform_video_get_framebuffer(void) {
    return frame;
}

/* Transpose cost, averaged over the last second and printed to the bottom
 * screen console. Measured here rather than inferred from the profiling
 * overlay's PPU row: that row is stamped into the scanline data, so any
 * diagnostic that stops refreshing the screen also freezes the number it is
 * meant to report. */
static uint64_t blit_ticks_acc;
static int blit_frames;

#define BOTTOM_WIDTH   320
#define BOTTOM_HEIGHT  240

/*
 * Bottom-screen UI, stage 1: mirror one window to the bottom screen while it
 * still renders on top, so the two can be compared directly.
 *
 * The window's pixels are already composited into frame[] by the time
 * end_frame runs, so this copies that rectangle rather than rasterizing the
 * window's tilemap a second time. That means no second PPU pass, no scratch
 * VRAM, and no way for this to change what the top screen shows — the
 * suppression half (removing it from the top composite) is stage 2, and it is
 * the half that touches shared state.
 *
 * Window geometry is in 8x8 tiles. BG3 (the window layer) renders at
 * SNES_WIDTH centred in the wider viewport, so tile coordinates are offset by
 * the viewport padding to reach frame[] coordinates.
 */
static const WindowInfo *find_active_window(uint8_t window_id) {
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (win.windows[i].active && win.windows[i].id == window_id)
            return &win.windows[i];
    }
    return NULL;
}

static void mirror_window_to_bottom(uint8_t window_id) {
    u16 fbw = 0, fbh = 0;
    u16 *fb = (u16 *)gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, &fbw, &fbh);
    if (!fb)
        return;

    const WindowInfo *w = find_active_window(window_id);
    if (!w || w->width == 0 || w->height == 0) {
        memset(fb, 0, (size_t)fbw * fbh * sizeof(pixel_t));
        return;
    }

    int src_x = w->x * 8 + EB_VIEWPORT_PAD_LEFT;
    int src_y = w->y * 8 + EB_VIEWPORT_PAD_TOP;
    int src_w = w->width * 8;
    int src_h = w->height * 8;

    /* Clip to the staging frame. */
    if (src_x < 0) { src_w += src_x; src_x = 0; }
    if (src_y < 0) { src_h += src_y; src_y = 0; }
    if (src_x + src_w > EB_VIEWPORT_WIDTH)  src_w = EB_VIEWPORT_WIDTH  - src_x;
    if (src_y + src_h > EB_VIEWPORT_HEIGHT) src_h = EB_VIEWPORT_HEIGHT - src_y;
    if (src_w <= 0 || src_h <= 0) {
        memset(fb, 0, (size_t)fbw * fbh * sizeof(pixel_t));
        return;
    }
    if (src_w > BOTTOM_WIDTH)  src_w = BOTTOM_WIDTH;
    if (src_h > BOTTOM_HEIGHT) src_h = BOTTOM_HEIGHT;

    /* Centre it at 1:1 — no scaling, so the mirror is pixel-for-pixel. */
    int dst_x = (BOTTOM_WIDTH  - src_w) / 2;
    int dst_y = (BOTTOM_HEIGHT - src_h) / 2;

    memset(fb, 0, (size_t)fbw * fbh * sizeof(pixel_t));

    /* Same column-major, Y-flipped layout as the top screen. */
    for (int x = 0; x < src_w; x++) {
        u16 *dst = &fb[(size_t)(dst_x + x) * fbw + (fbw - 1 - dst_y)];
        const pixel_t *src = &frame[(size_t)src_y * EB_VIEWPORT_WIDTH + src_x + x];
        for (int y = 0; y < src_h; y++) {
            *dst-- = *src;
            src += EB_VIEWPORT_WIDTH;
        }
    }
}

void platform_video_end_frame(void) {
    u16 fbw = 0, fbh = 0;
    uint64_t t0 = svcGetSystemTick();

    /* Reported dimensions are in framebuffer space: fbw is the panel's short
     * axis (240), fbh its long axis (400). Use them rather than assuming, so
     * a stride change can't silently garble the image. */
    u16 *fb = (u16 *)gfxGetFramebuffer(GFX_TOP, GFX_LEFT, &fbw, &fbh);
    if (!fb)
        return;

    const int off_x = (PHYS_WIDTH  - EB_VIEWPORT_WIDTH)  / 2;
    const int off_y = (PHYS_HEIGHT - EB_VIEWPORT_HEIGHT) / 2;

    for (int x = 0; x < EB_VIEWPORT_WIDTH; x++) {
        /* Destination column (off_x + x), walked bottom-to-top because the
         * framebuffer's Y axis is flipped relative to screen coordinates. */
        u16 *dst = &fb[(size_t)(off_x + x) * fbw + (fbw - 1 - off_y)];
        const pixel_t *src = &frame[x];
        for (int y = 0; y < EB_VIEWPORT_HEIGHT; y++) {
            *dst-- = *src;
            src += EB_VIEWPORT_WIDTH;
        }
    }

    /* Stage 1: mirror the battle command menu. Skipped while the debug console
     * owns the bottom screen (different pixel format). */
    if (!platform_console_logging)
        mirror_window_to_bottom(WINDOW_COMMAND_MENU);

    blit_ticks_acc += svcGetSystemTick() - t0;
    if (++blit_frames >= 60 && !platform_console_logging) {
        uint32_t usec = (uint32_t)(blit_ticks_acc / blit_frames
                                   / (SYSCLOCK_ARM11 / 1000000));
        /* Home the cursor so the figure updates in place. */
        printf("\x1b[0;0Htranspose %3lu.%02lu ms/frame  (%dx%d)   ",
               (unsigned long)(usec / 1000), (unsigned long)((usec % 1000) / 10),
               EB_VIEWPORT_WIDTH, EB_VIEWPORT_HEIGHT);
        blit_ticks_acc = 0;
        blit_frames = 0;
    }

    /* Request the flip and return. The vblank wait lives in
     * platform_timer_frame_end() — keeping it here put a blocking sleep
     * inside the region game_main.c times as "PPU", so render cost and idle
     * headroom were indistinguishable. Hardware applies the flip at vblank,
     * so issuing it before the wait still presents a whole frame. */
    gfxFlushBuffers();
    gfxSwapBuffers();
}

void platform_video_set_vsync(bool enabled) {
    (void)enabled; /* always paced by the vblank wait in frame_end */
}
