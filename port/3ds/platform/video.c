#include "platform/platform.h"
#include "game/window.h"
#include "snes/ppu.h"
#include "include/binary.h"
#include "game/game_state.h"
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

    /* Bottom screen: either a debug console (launched with L held) or the UI
     * mirror, which needs the same 16-bit format as the staging frame it
     * copies from. stderr is swallowed unless main.c routes it to the console.
     * main.c reads the launch keys before calling this so the choice is known
     * here. */
    if (platform_console_logging) {
        gfxSetDoubleBuffering(GFX_BOTTOM, false);
        consoleInit(GFX_BOTTOM, NULL);
    } else {
        gfxSetScreenFormat(GFX_BOTTOM, GSP_RGB565_OES);
        /* Double-buffered: the mirror rewrites the whole screen each frame,
         * and writing into the buffer being scanned out shows as a band
         * tearing down the image. */
        gfxSetDoubleBuffering(GFX_BOTTOM, true);
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
 * Bottom-screen UI: draw the focused window on the bottom screen.
 *
 * BG3 is the window layer -- 2bpp, 8x8 tiles, a 32x32 tilemap -- so this
 * decodes that tile rectangle straight out of VRAM rather than copying the
 * composited pixels out of frame[]. Copying was fine for proving the geometry,
 * but it only works while the window is also drawn on top, which is precisely
 * what stage 2 removes. Rendering from VRAM keeps the bottom screen
 * independent of the top-screen composite, so suppression becomes a separate,
 * small change.
 *
 * Base addresses and the index-0-is-transparent rule follow ppu_render.c.
 * Brightness (INIDISP) is deliberately not applied: the window should stay
 * readable while the world fades.
 */
static const WindowInfo *focused_window(void) {
    uint8_t id = win.current_focus_window;
    if (id == WINDOW_ID_NONE)
        return NULL;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (win.windows[i].active && win.windows[i].id == id)
            return &win.windows[i];
    }
    return NULL;
}

static void draw_focused_window_to_bottom(void) {
    u16 fbw = 0, fbh = 0;
    u16 *fb = (u16 *)gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, &fbw, &fbh);
    if (!fb)
        return;

    /* Whichever window has focus is the one the player is interacting with,
     * so this follows the overworld menu, the battle menus and their
     * sub-menus without naming any of them. */
    const WindowInfo *w = focused_window();

    /* Focus drops for a frame or two across menu transitions (one window
     * closing before the next takes focus), and blanking on each of those
     * showed as a black flicker. Hold the previous frame instead until focus
     * has been gone long enough to mean it. Nothing is redrawn while held, so
     * the double-buffered pair keeps showing the last good image. */
    static int idle_frames;
    if (!w || w->width == 0 || w->height == 0) {
        if (++idle_frames < 10)
            return;
        memset(fb, 0, (size_t)fbw * fbh * sizeof(pixel_t));
        return;
    }
    idle_frames = 0;

    memset(fb, 0, (size_t)fbw * fbh * sizeof(pixel_t));

    const uint32_t map_base  = (uint32_t)(ppu.bg_sc[2] & 0xFC) << 9;
    const uint32_t tile_base = (uint32_t)(ppu.bg_nba[1] & 0x0F) * 0x2000;

    int px_w = w->width * 8;
    int px_h = w->height * 8;
    if (px_w <= 0 || px_h <= 0)
        return;

    /* Integer scale only, so it stays pixel-exact. Doubling also makes the
     * menus big enough to touch. */
    int scale = (px_w * 2 <= BOTTOM_WIDTH && px_h * 2 <= BOTTOM_HEIGHT) ? 2 : 1;
    int out_w = px_w * scale;
    int out_h = px_h * scale;
    if (out_w > BOTTOM_WIDTH || out_h > BOTTOM_HEIGHT)
        return;

    int ox = (BOTTOM_WIDTH  - out_w) / 2;
    int oy = (BOTTOM_HEIGHT - out_h) / 2;

    for (int ty = 0; ty < w->height; ty++) {
        for (int tx = 0; tx < w->width; tx++) {
            uint32_t entry_addr = map_base +
                ((size_t)(((w->y + ty) & 31) * 32 + ((w->x + tx) & 31))) * 2;
            if (entry_addr + 1 >= VRAM_SIZE)
                continue;

            uint16_t entry    = read_u16_le(&ppu.vram[entry_addr]);
            uint16_t tile_num = entry & 0x3FF;
            uint8_t  palette  = (entry >> 10) & 0x07;
            bool     hflip    = (entry >> 14) & 1;
            bool     vflip    = (entry >> 15) & 1;

            uint32_t tile_addr = tile_base + (uint32_t)tile_num * 16;
            if (tile_addr + 15 >= VRAM_SIZE)
                continue;

            for (int row = 0; row < 8; row++) {
                const uint8_t *planes = &ppu.vram[tile_addr + (vflip ? 7 - row : row) * 2];
                for (int col = 0; col < 8; col++) {
                    int bit = 7 - (hflip ? 7 - col : col);
                    uint8_t idx = (uint8_t)(((planes[0] >> bit) & 1) |
                                            (((planes[1] >> bit) & 1) << 1));
                    if (idx == 0)
                        continue;  /* transparent */

                    pixel_t c = bgr555_to_pixel(ppu.cgram[palette * 4 + idx]);

                    int bx = ox + (tx * 8 + col) * scale;
                    int by = oy + (ty * 8 + row) * scale;
                    for (int sy = 0; sy < scale; sy++) {
                        int yy = fbw - 1 - (by + sy);
                        for (int sx = 0; sx < scale; sx++)
                            fb[(size_t)(bx + sx) * fbw + yy] = c;
                    }
                }
            }
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

    /* Draw the focused window on the bottom screen. Skipped while the debug
     * console owns it (different pixel format). */
    if (!platform_console_logging)
        draw_focused_window_to_bottom();

    /* Palette readout, console mode only: the BG sub-palettes the PPU actually
     * renders from, next to the flavour that selected them. Compare against
     * text_window_flavour_palettes.pal to tell a wrong palette index from
     * wrong palette contents from a tile/attribute problem. */
    blit_ticks_acc += svcGetSystemTick() - t0;

    if (++blit_frames >= 60) {
        if (platform_console_logging) {
            printf("\x1b[2;0Hflavour=%u  hppp_opt=%04X\n",
                   (unsigned)game_state.text_flavour,
                   (unsigned)party_characters[0].hp_pp_window_options);
            for (int sp = 0; sp < 8; sp++) {
                printf("cg%d: %04X %04X %04X %04X\n", sp,
                       ppu.cgram[sp * 4 + 0], ppu.cgram[sp * 4 + 1],
                       ppu.cgram[sp * 4 + 2], ppu.cgram[sp * 4 + 3]);
            }
        } else {
            uint32_t usec = (uint32_t)(blit_ticks_acc / blit_frames
                                       / (SYSCLOCK_ARM11 / 1000000));
            /* Home the cursor so the figure updates in place. */
            printf("\x1b[0;0Htranspose %3lu.%02lu ms/frame  (%dx%d)   ",
                   (unsigned long)(usec / 1000), (unsigned long)((usec % 1000) / 10),
                   EB_VIEWPORT_WIDTH, EB_VIEWPORT_HEIGHT);
        }
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
