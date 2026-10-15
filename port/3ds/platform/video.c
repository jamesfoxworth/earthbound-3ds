#include "platform/platform.h"
#include "game/window.h"
#include "game/overworld.h"
#include "game/text.h"
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

/* Geometry of the window last drawn to the bottom screen. Kept so touch input
 * can map a tap back to a tile inside it without duplicating the layout. */
static struct {
    bool    valid;
    uint8_t window_id;
    int     ox, oy, scale;
    int     tiles_w, tiles_h;
} bottom_layout;

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

static inline void put_px(u16 *fb, int fbw, int x, int y, pixel_t c) {
    if (x < 0 || x >= BOTTOM_WIDTH || y < 0 || y >= BOTTOM_HEIGHT)
        return;
    fb[(size_t)x * fbw + (fbw - 1 - y)] = c;
}

/* Rasterise a rectangle of the BG3 tilemap to the bottom screen. BG3 is the
 * window layer -- 2bpp, 8x8 tiles, 32x32 tilemap -- so any window the game has
 * drawn can be redrawn here from the game's own tiles. Base addresses and the
 * index-0-is-transparent rule follow ppu_render.c. Brightness is deliberately
 * not applied so the panel stays readable while the world fades. */
static void draw_tilemap_rect(u16 *fb, int fbw, int tile_x, int tile_y,
                              int tiles_w, int tiles_h,
                              int dest_x, int dest_y, int scale) {
    const uint32_t map_base  = (uint32_t)(ppu.bg_sc[2] & 0xFC) << 9;
    const uint32_t tile_base = (uint32_t)(ppu.bg_nba[1] & 0x0F) * 0x2000;

    for (int ty = 0; ty < tiles_h; ty++) {
        for (int tx = 0; tx < tiles_w; tx++) {
            uint32_t entry_addr = map_base +
                ((size_t)(((tile_y + ty) & 31) * 32 + ((tile_x + tx) & 31))) * 2;
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
                const uint8_t *planes =
                    &ppu.vram[tile_addr + (vflip ? 7 - row : row) * 2];
                for (int col = 0; col < 8; col++) {
                    int bit = 7 - (hflip ? 7 - col : col);
                    uint8_t idx = (uint8_t)(((planes[0] >> bit) & 1) |
                                            (((planes[1] >> bit) & 1) << 1));
                    if (idx == 0)
                        continue;

                    pixel_t c = bgr555_to_pixel(ppu.cgram[palette * 4 + idx]);
                    int bx = dest_x + (tx * 8 + col) * scale;
                    int by = dest_y + (ty * 8 + row) * scale;
                    for (int sy = 0; sy < scale; sy++)
                        for (int sx = 0; sx < scale; sx++)
                            put_px(fb, fbw, bx + sx, by + sy, c);
                }
            }
        }
    }
}

/* ---- Bottom-screen panel -------------------------------------------------
 *
 * Always-present party status, composed here rather than mirrored from a game
 * window: the HP/PP windows are only drawn when the game decides to show them
 * (B on the overworld, or in battle), so there is nothing to copy the rest of
 * the time. Text uses the game's own TINY font via font_get_glyph(), the same
 * way the profiling overlay stamps its rows, so it matches the game's look
 * without needing the window system.
 */


/* Draw one glyph run. Glyph rows are bitmasks where a CLEAR bit is ink (the
 * convention font_get_glyph uses). Returns the advance in pixels. */
static int draw_glyphs(u16 *fb, int fbw, int x, int y, const uint8_t *idx,
                       int count, int scale, pixel_t color) {
    uint8_t h = font_get_height(FONT_ID_TINY);
    int cx = x;
    for (int i = 0; i < count; i++) {
        const uint8_t *glyph = font_get_glyph(FONT_ID_TINY, idx[i]);
        uint8_t w = font_get_width(FONT_ID_TINY, idx[i]);
        if (glyph) {
            for (int row = 0; row < h; row++) {
                uint8_t bits = glyph[row];
                for (int col = 0; col < w && col < 8; col++) {
                    if (bits & (0x80 >> col))
                        continue;          /* set bit = background */
                    for (int sy = 0; sy < scale; sy++)
                        for (int sx = 0; sx < scale; sx++)
                            put_px(fb, fbw, cx + col * scale + sx,
                                   y + row * scale + sy, color);
                }
            }
        }
        cx += w * scale;
    }
    return cx - x;
}

static int draw_ascii(u16 *fb, int fbw, int x, int y, const char *str,
                      int scale, pixel_t color) {
    uint8_t idx[48];
    int n = 0;
    for (const char *s = str; *s && n < (int)sizeof(idx); s++)
        idx[n++] = (uint8_t)(ascii_to_eb_char(*s) - 0x50);
    return draw_glyphs(fb, fbw, x, y, idx, n, scale, color);
}

/* Names are stored EB-encoded, so they index the font directly. */
static int draw_eb(u16 *fb, int fbw, int x, int y, const uint8_t *eb, int max,
                   int scale, pixel_t color) {
    uint8_t idx[16];
    int n = 0;
    for (int i = 0; i < max && i < (int)sizeof(idx); i++) {
        if (eb[i] == 0x00)
            break;
        idx[n++] = (uint8_t)(eb[i] - 0x50);
    }
    return draw_glyphs(fb, fbw, x, y, idx, n, scale, color);
}

/* Battle: the four HP/PP panels across the top, the focused menu window
 * beneath them -- the game has already drawn both into the window layer, so
 * these are its own panels, not a reconstruction. */
static void draw_battle_bottom(u16 *fb, int fbw) {
    int count = game_state.player_controlled_party_count;
    if (count > HPPP_MAX_PARTY) count = HPPP_MAX_PARTY;
    if (count < 0) count = 0;

    const int panel_px_w = HPPP_WINDOW_WIDTH * 8;
    int row_w = count * panel_px_w;
    int ox = (BOTTOM_WIDTH - row_w) / 2;

    for (int i = 0; i < count; i++) {
        int tx = HPPP_CENTER_TILE - (count * HPPP_WINDOW_WIDTH) / 2
                 + i * HPPP_WINDOW_WIDTH;
        int ty = (win.battle_menu_current_character_id == (int16_t)i)
                 ? ACTIVE_HPPP_WINDOW_Y_OFFSET : NORMAL_HPPP_WINDOW_Y_OFFSET;
        draw_tilemap_rect(fb, fbw, tx, ty, HPPP_WINDOW_WIDTH, HPPP_WINDOW_HEIGHT,
                          ox + i * panel_px_w, 6, 1);
    }

    const WindowInfo *w = focused_window();
    if (!w || w->width == 0 || w->height == 0)
        return;

    int menu_w = w->width * 8;
    int menu_h = w->height * 8;
    int my = 6 + HPPP_WINDOW_HEIGHT * 8 + 10;
    if (menu_h > BOTTOM_HEIGHT - my)
        return;
    draw_tilemap_rect(fb, fbw, w->x, w->y, w->width, w->height,
                      (BOTTOM_WIDTH - menu_w) / 2, my, 1);
}

static void draw_bottom_panel(void) {
    u16 fbw = 0, fbh = 0;
    u16 *fb = (u16 *)gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, &fbw, &fbh);
    if (!fb)
        return;

    const pixel_t bg    = PIXEL_RGB(16, 16, 32);
    const pixel_t label = PIXEL_RGB(160, 160, 176);
    const pixel_t value = PIXEL_RGB(255, 255, 255);

    for (int x = 0; x < BOTTOM_WIDTH; x++)
        for (int y = 0; y < BOTTOM_HEIGHT; y++)
            put_px(fb, fbw, x, y, bg);

    /* font_get_glyph returns NULL until the font tiles are loaded. */
    if (!font_get_glyph(FONT_ID_TINY, 0))
        return;

    const int scale = 2;
    const int row_h = (font_get_height(FONT_ID_TINY) + 3) * scale;
    int y = 10;

    int count = game_state.player_controlled_party_count;
    if (count > 4)
        count = 4;

    for (int i = 0; i < count; i++) {
        uint8_t id = game_state.player_controlled_party_members[i];
        if (id >= TOTAL_PARTY_COUNT)
            continue;
        const CharStruct *ch = &party_characters[id];

        char buf[32];
        int x = 12;
        x += draw_eb(fb, fbw, x, y, ch->name, 5, scale, value);

        x = 110;
        x += draw_ascii(fb, fbw, x, y, "HP", scale, label);
        snprintf(buf, sizeof(buf), " %u/%u",
                 (unsigned)ch->current_hp, (unsigned)ch->max_hp);
        x += draw_ascii(fb, fbw, x, y, buf, scale, value);

        x = 215;
        x += draw_ascii(fb, fbw, x, y, "PP", scale, label);
        snprintf(buf, sizeof(buf), " %u/%u",
                 (unsigned)ch->current_pp, (unsigned)ch->max_pp);
        draw_ascii(fb, fbw, x, y, buf, scale, value);

        y += row_h;
    }
}

/* Map a bottom-screen touch to a tile inside the window drawn there. Returns
 * false when nothing is drawn or the point falls outside it. */
bool platform_bottom_hit_test(int touch_x, int touch_y, uint8_t *out_window_id,
                              int *out_tile_col, int *out_tile_row) {
    if (!bottom_layout.valid)
        return false;

    int lx = touch_x - bottom_layout.ox;
    int ly = touch_y - bottom_layout.oy;
    if (lx < 0 || ly < 0)
        return false;

    int col = lx / (8 * bottom_layout.scale);
    int row = ly / (8 * bottom_layout.scale);
    if (col >= bottom_layout.tiles_w || row >= bottom_layout.tiles_h)
        return false;

    if (out_window_id) *out_window_id = bottom_layout.window_id;
    if (out_tile_col)  *out_tile_col  = col;
    if (out_tile_row)  *out_tile_row  = row;
    return true;
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

    /* Bottom screen: in battle show the game's own menu window, which is the
     * UI that belongs down here; elsewhere show the composed status panel.
     * Skipped while the debug console owns the screen. */
    if (!platform_console_logging) {
        if (ow.battle_mode) {
            u16 bw = 0, bh = 0;
            u16 *bfb = (u16 *)gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, &bw, &bh);
            if (bfb) {
                memset(bfb, 0, (size_t)bw * bh * sizeof(pixel_t));
                draw_battle_bottom(bfb, bw);
            }
        } else {
            draw_bottom_panel();
        }
    }

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
