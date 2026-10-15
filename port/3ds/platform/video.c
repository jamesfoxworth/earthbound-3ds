#include "platform/platform.h"
#include "game/window.h"
#include "game/overworld.h"
#include "game/text.h"
#include "snes/ppu.h"
#include "include/binary.h"
#include "game/game_state.h"
#include "core/mode_stack.h"
#include "touch.h"
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
        /* Both buffers: elements are redrawn individually afterwards, so
         * whatever is not covered has to start blank in each. */
        for (int i = 0; i < 2; i++) {
            u16 bw = 0, bh = 0;
            u8 *bfb = gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, &bw, &bh);
            if (bfb)
                memset(bfb, 0, (size_t)bw * bh * sizeof(pixel_t));
            gfxSwapBuffers();
        }
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


/* ---- Top-screen suppression ---------------------------------------------
 *
 * In battle the top screen should be the background and the enemies, with all
 * of the UI on the bottom. BG3 is the window layer, so switching it off on the
 * main screen for the duration of the render achieves that in one register
 * bit -- no VRAM writes, and nothing to keep in step with which windows happen
 * to be open.
 *
 * An earlier version blanked each routed window's tiles instead, which could
 * not keep up: battle has several windows open at once, so the action menu
 * reappeared once targeting took focus, the targeting window's rectangle
 * clipped a corner out of the action menu, and any window whose rectangle was
 * never computed (leftover overworld UI) stayed visible.
 *
 * The register is restored in end_frame, so the game only ever sees its own
 * value. The bottom screen reads the tilemap out of VRAM and is unaffected.
 *
 * The overworld needs none of this: nothing there is replaced, so its windows
 * are left exactly as the game draws them.
 */
static struct {
    bool    active;
    uint8_t tm;
    bool    hdma;
    uint8_t tm_line[EB_VIEWPORT_HEIGHT];
} suppress;

static void suppress_apply(void) {
    suppress.active = false;
    /* The swirl drives per-scanline layer control of its own while entering or
     * leaving battle; masking BG3 out from under it renders it glitchy. Leave
     * the register alone until the swirl is done. */
    if (platform_console_logging || !ow.battle_mode || ow.battle_swirl_countdown)
        return;

    suppress.active = true;
    suppress.tm = ppu.tm;
    ppu.tm &= (uint8_t)~0x04;   /* BG3 off on the main screen */

    /* A per-scanline TM table overrides the register when HDMA emulation is
     * live, so it has to lose BG3 as well. */
    suppress.hdma = ppu.tm_hdma_active;
    if (suppress.hdma) {
        memcpy(suppress.tm_line, ppu.tm_per_scanline, sizeof(suppress.tm_line));
        for (size_t i = 0; i < sizeof(suppress.tm_line); i++)
            ppu.tm_per_scanline[i] &= (uint8_t)~0x04;
    }
}

static void suppress_restore(void) {
    if (!suppress.active)
        return;
    ppu.tm = suppress.tm;
    if (suppress.hdma)
        memcpy(ppu.tm_per_scanline, suppress.tm_line, sizeof(suppress.tm_line));
    suppress.active = false;
}

void platform_video_begin_frame(void) {
    /* Scanlines land in the staging buffer. Take the window layer's routed
     * rectangles out before the PPU reads it; end_frame puts them back. */
    suppress_apply();
}

/* Set from the options panel; defined further down with the other options. */
extern int video_opt_view_width;   /* declared extern near send_scanline */

void platform_video_send_scanline(int y, const pixel_t *pixels) {
    if (y < 0 || y >= EB_VIEWPORT_HEIGHT)
        return;
    pixel_t *row = &frame[y * EB_VIEWPORT_WIDTH];
    memcpy(row, pixels, EB_VIEWPORT_WIDTH * sizeof(pixel_t));

    /* "Widescreen off": the viewport width is a build-time constant, so the
     * game still renders 400 wide and the extra columns are blanked here.
     * Content stays exactly where it is -- this crops rather than rescaling,
     * which is what makes it free. */
    int vw = video_opt_view_width;
    if (vw > 0 && vw < EB_VIEWPORT_WIDTH) {
        const int pad = (EB_VIEWPORT_WIDTH - vw) / 2;
        memset(row, 0, (size_t)pad * sizeof(pixel_t));
        memset(row + pad + vw, 0,
               (size_t)(EB_VIEWPORT_WIDTH - pad - vw) * sizeof(pixel_t));
    }
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

/* The bottom screen is composed into this image and then blitted whole, every
 * frame. Drawing straight into the framebuffer does not work with double
 * buffering unless every pixel is written every frame: updating only the
 * elements that changed left the two buffers holding different pictures, which
 * showed as a black wave across the panels, jitter, and the previous
 * context's UI (the overworld panel) surviving underneath the battle UI.
 * Keeping the image here also means an element whose source is mid-redraw can
 * simply be skipped, retaining its last good pixels. */
static pixel_t bottom_img[BOTTOM_WIDTH * BOTTOM_HEIGHT];

static inline void img_px(int x, int y, pixel_t c) {
    if (x < 0 || x >= BOTTOM_WIDTH || y < 0 || y >= BOTTOM_HEIGHT)
        return;
    bottom_img[(size_t)y * BOTTOM_WIDTH + x] = c;
}

static void present_bottom(void) {
    u16 fbw = 0, fbh = 0;
    u16 *fb = (u16 *)gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, &fbw, &fbh);
    if (!fb)
        return;
    for (int x = 0; x < BOTTOM_WIDTH; x++) {
        u16 *dst = &fb[(size_t)x * fbw + (fbw - 1)];
        const pixel_t *src = &bottom_img[x];
        for (int y = 0; y < BOTTOM_HEIGHT; y++) {
            *dst-- = *src;
            src += BOTTOM_WIDTH;
        }
    }
}

/* True when every tilemap entry in the rectangle is zero, i.e. the window code
 * has cleared it and not yet redrawn. Redrawing the bottom screen from that
 * would show one black frame, which reads as a flicker on a panel that is
 * always on screen, so those frames are skipped instead. */
static bool tilemap_rect_blank(int tile_x, int tile_y, int tiles_w, int tiles_h) {
    const uint32_t map_base = (uint32_t)(ppu.bg_sc[2] & 0xFC) << 9;
    for (int ty = 0; ty < tiles_h; ty++) {
        for (int tx = 0; tx < tiles_w; tx++) {
            uint32_t a = map_base +
                ((size_t)((((tile_y + ty) & 31) * 32) + ((tile_x + tx) & 31))) * 2;
            if (a + 1 >= VRAM_SIZE)
                continue;
            if (read_u16_le(&ppu.vram[a]) != 0)
                return false;
        }
    }
    return true;
}

/* Fill a destination rectangle, used before redrawing one element so stale
 * pixels do not show through. */
static void fill_rect(int x, int y, int w, int h, pixel_t c) {
    for (int iy = 0; iy < h; iy++)
        for (int ix = 0; ix < w; ix++)
            img_px(x + ix, y + iy, c);
}

/* Rasterise a rectangle of the BG3 tilemap to the bottom screen. BG3 is the
 * window layer -- 2bpp, 8x8 tiles, 32x32 tilemap -- so any window the game has
 * drawn can be redrawn here from the game's own tiles. Base addresses and the
 * index-0-is-transparent rule follow ppu_render.c. Brightness is deliberately
 * not applied so the panel stays readable while the world fades. */
static void draw_tilemap_rect_src(const uint8_t *shadow,
                                  int tile_x, int tile_y,
                                  int tiles_w, int tiles_h,
                                  int dest_x, int dest_y, int scale) {
    const uint32_t map_base  = (uint32_t)(ppu.bg_sc[2] & 0xFC) << 9;
    const uint32_t tile_base = (uint32_t)(ppu.bg_nba[1] & 0x0F) * 0x2000;

    for (int ty = 0; ty < tiles_h; ty++) {
        for (int tx = 0; tx < tiles_w; tx++) {
            uint16_t entry;
            if (shadow) {
                /* win.bg2_buffer is the game's RAM shadow of this same 32x32
                 * tilemap, 2 bytes per entry. Tile graphics still come from
                 * VRAM -- only the entries differ. */
                size_t so = ((size_t)(((tile_y + ty) & 31) * 32
                                      + ((tile_x + tx) & 31))) * 2;
                entry = read_u16_le(&shadow[so]);
            } else {
            uint32_t entry_addr = map_base +
                ((size_t)(((tile_y + ty) & 31) * 32 + ((tile_x + tx) & 31))) * 2;
            if (entry_addr + 1 >= VRAM_SIZE)
                continue;

            entry = read_u16_le(&ppu.vram[entry_addr]);
            }
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
                            img_px(bx + sx, by + sy, c);
                }
            }
        }
    }
}

static void draw_tilemap_rect(int tile_x, int tile_y,
                              int tiles_w, int tiles_h,
                              int dest_x, int dest_y, int scale) {
    draw_tilemap_rect_src(NULL, tile_x, tile_y, tiles_w, tiles_h,
                          dest_x, dest_y, scale);
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
static int font_id_cur = FONT_ID_TINY;

static int draw_glyphs(int x, int y, const uint8_t *idx,
                       int count, int scale, pixel_t color) {
    uint8_t h = font_get_height(font_id_cur);
    int cx = x;
    for (int i = 0; i < count; i++) {
        const uint8_t *glyph = font_get_glyph(font_id_cur, idx[i]);
        uint8_t w = font_get_width(font_id_cur, idx[i]);
        if (glyph) {
            for (int row = 0; row < h; row++) {
                uint8_t bits = glyph[row];
                for (int col = 0; col < w && col < 8; col++) {
                    if (bits & (0x80 >> col))
                        continue;          /* set bit = background */
                    for (int sy = 0; sy < scale; sy++)
                        for (int sx = 0; sx < scale; sx++)
                            img_px(cx + col * scale + sx,
                                   y + row * scale + sy, color);
                }
            }
        }
        cx += w * scale;
    }
    return cx - x;
}

static int draw_ascii(int x, int y, const char *str,
                      int scale, pixel_t color) {
    uint8_t idx[48];
    int n = 0;
    for (const char *s = str; *s && n < (int)sizeof(idx); s++)
        idx[n++] = (uint8_t)(ascii_to_eb_char(*s) - 0x50);
    return draw_glyphs(x, y, idx, n, scale, color);
}

/* Names are stored EB-encoded, so they index the font directly. */
static int draw_eb(int x, int y, const uint8_t *eb, int max,
                   int scale, pixel_t color) {
    uint8_t idx[16];
    int n = 0;
    for (int i = 0; i < max && i < (int)sizeof(idx); i++) {
        if (eb[i] == 0x00)
            break;
        idx[n++] = (uint8_t)(eb[i] - 0x50);
    }
    return draw_glyphs(x, y, idx, n, scale, color);
}

/* The window shown on the bottom screen, or NULL. With the window layer off
 * on the top screen, battle dialogue comes down here too, otherwise it would
 * not be visible at all. */
/* The game's own window frame, drawn into a scratch tilemap and rasterised
 * with the game's tiles -- the same border entries render_all_windows() uses,
 * so the box matches every other window on screen including the player's
 * chosen flavour palette.
 *
 * This is assembled here rather than by creating a real window: there is no
 * per-window render entry point (only render_all_windows), and create_window()
 * mid-frame would take over win.current_focus_window while a dialogue may be
 * using it. */
#define WFRAME_TL     0x3C10
#define WFRAME_TOP    0x3C11
#define WFRAME_LEFT   0x3C12
#define WFRAME_TR     0x7C10
#define WFRAME_RIGHT  0x7C12
#define WFRAME_BL     0xBC10
#define WFRAME_BOTTOM 0xBC11
#define WFRAME_BR     0xFC10
#define WFRAME_FILL   (0x2000 | 0x40)

static uint8_t frame_scratch[2048];

static void draw_window_box(int tiles_w, int tiles_h, int dest_x, int dest_y) {
    memset(frame_scratch, 0, sizeof(frame_scratch));
    for (int ty = 0; ty < tiles_h && ty < 32; ty++) {
        for (int tx = 0; tx < tiles_w && tx < 32; tx++) {
            bool t = (ty == 0), b = (ty == tiles_h - 1);
            bool l = (tx == 0), r = (tx == tiles_w - 1);
            uint16_t e = t && l ? WFRAME_TL : t && r ? WFRAME_TR
                       : t      ? WFRAME_TOP
                       : b && l ? WFRAME_BL : b && r ? WFRAME_BR
                       : b      ? WFRAME_BOTTOM
                       : l      ? WFRAME_LEFT
                       : r      ? WFRAME_RIGHT : WFRAME_FILL;
            size_t o = ((size_t)ty * 32 + tx) * 2;
            frame_scratch[o]     = (uint8_t)(e & 0xFF);
            frame_scratch[o + 1] = (uint8_t)(e >> 8);
        }
    }
    draw_tilemap_rect_src(frame_scratch, 0, 0, tiles_w, tiles_h,
                          dest_x, dest_y, 1);
}

static int measure_ascii(const char *str) {
    int px = 0;
    for (const char *c = str; *c; c++)
        px += font_get_width(font_id_cur,
                             (uint8_t)(ascii_to_eb_char(*c) - 0x50));
    return px;
}


/* Carried money, in the same box the B menu uses: window_configs[0x0A] is
 * 8x4 tiles with number_padding 5, so the box is a FIXED size and the value
 * is right-aligned inside it. Sizing it to the text made it grow and shrink,
 * and under-measured so anything past two digits overflowed. */
#define MONEY_TILES_W 8
#define MONEY_TILES_H 4
#define MONEY_BOX_W   (MONEY_TILES_W * 8)
#define MONEY_BOX_H   (MONEY_TILES_H * 8)

static int draw_money_box(int right_x, int dest_y) {
    char buf[16];
    snprintf(buf, sizeof(buf), "$%lu", (unsigned long)game_state.money_carried);

    int prev = font_id_cur;
    font_id_cur = FONT_ID_NORMAL;
    if (!font_get_glyph(font_id_cur, 0)) {   /* main font not resident yet */
        font_id_cur = prev;
        return 0;
    }

    int x = right_x - MONEY_BOX_W;
    draw_window_box(MONEY_TILES_W, MONEY_TILES_H, x, dest_y);

    const int inner_x = x + 8, inner_w = (MONEY_TILES_W - 2) * 8;
    const int inner_y = dest_y + 8, inner_h = (MONEY_TILES_H - 2) * 8;
    int text_px = measure_ascii(buf);
    int tx = inner_x + (text_px < inner_w ? inner_w - text_px : 0);
    int ty = inner_y + (inner_h - font_get_height(font_id_cur)) / 2;
    draw_ascii(tx, ty, buf, 1, PIXEL_RGB(255, 255, 255));
    font_id_cur = prev;
    return MONEY_BOX_W;
}

/* Settings entry. Not interactive yet -- touch input still has to be put
 * back -- so this is the placeholder the hub will hang off. */
static int settings_hit_x, settings_hit_y, settings_hit_w, settings_hit_h;

static int draw_settings_box(int right_x, int dest_y) {
    const int tiles_w = 4;
    const int box_w = tiles_w * 8;
    int x = right_x - box_w;
    settings_hit_x = x;
    settings_hit_y = dest_y;
    settings_hit_w = box_w;
    settings_hit_h = MONEY_BOX_H;
    draw_window_box(tiles_w, MONEY_TILES_H, x, dest_y);

    const pixel_t ink = PIXEL_RGB(255, 255, 255);
    int cx = x + box_w / 2;
    int cy = dest_y + MONEY_BOX_H / 2;
    for (int dy = -7; dy <= 7; dy++) {
        for (int dx = -7; dx <= 7; dx++) {
            int d2 = dx * dx + dy * dy;
            bool ring = (d2 <= 36 && d2 >= 12);
            bool tooth = (d2 <= 49 && d2 > 36) &&
                         ((dx > -2 && dx < 2) || (dy > -2 && dy < 2) ||
                          (dx == dy) || (dx == -dy));
            if (ring || tooth)
                img_px(cx + dx, cy + dy, ink);
        }
    }
    return box_w;
}

/* ---- Options panel -------------------------------------------------------
 *
 * Opened from the settings box. Runtime-adjustable things only: the viewport
 * width is a build-time constant (it sizes the staging buffer, the PPU's
 * render width and the window scaling), so "widescreen off" is a render-time
 * pillarbox to the native 256 rather than a resolution change. */
/* Cropped presentation width. The viewport is a build-time 400; these are
 * render-time crops, so content never moves or rescales.
 *
 *   400  full widescreen
 *   320  true 4:3 on the 3DS's square pixels -- still 64 columns more than
 *        the SNES drew, but the correct geometry for a 240-row field
 *   256  the SNES's own column count. Against our 240 rows that is 16:15,
 *        very nearly square, which is why it reads as 1:1 and not 4:3.
 *
 * A real SNES stretched 256 columns across a 4:3 CRT, so matching the native
 * field of view AND 4:3 geometry at once would need horizontal resampling.
 * These crops avoid that; the choice is left to the player. */
int video_opt_view_width = EB_VIEWPORT_WIDTH;

static const char *const view_width_names[]  = { "WIDE", "4:3", "PURE" };
static const int         view_width_values[] = { EB_VIEWPORT_WIDTH, 320, SNES_WIDTH };
#define VIEW_WIDTH_COUNT ((int)(sizeof(view_width_values) / sizeof(int)))

static bool options_open;

typedef struct {
    const char *label;
    bool       *flag;    /* bool row, or NULL for a cycle row */
    int        *cycle;   /* cycle row value */
} OptionRow;

bool video_opt_fps;

static const OptionRow option_rows[] = {
    { "SCREEN",     NULL,            &video_opt_view_width },
    { "FPS METER",  &video_opt_fps,  NULL },
};
#define OPTION_ROW_COUNT ((int)(sizeof(option_rows) / sizeof(option_rows[0])))

/* The scratch tilemap draw_window_box() writes is a 32x32 map and
 * draw_tilemap_rect_src() wraps its coordinates with & 31, so a box wider
 * than 32 tiles wraps around and draws a second left border partway across
 * -- which is the stray window edge that appeared at the right. Cap the
 * width, and derive the height from the laid-out content so the last line
 * cannot land past the bottom border. */
#define OPT_TILES_W  30
#define OPT_W        (OPT_TILES_W * 8)
#define OPT_X        ((BOTTOM_WIDTH - OPT_W) / 2)
#define OPT_ROW_H    22
#define OPT_LINES    (OPTION_ROW_COUNT + 2)      /* title + options + close */
#define OPT_PAD_Y    6
#define OPT_TILES_H  (2 + (OPT_LINES * OPT_ROW_H + 2 * OPT_PAD_Y + 7) / 8)
#define OPT_H        (OPT_TILES_H * 8)
#define OPT_Y        ((BOTTOM_HEIGHT - OPT_H) / 2)
#define OPT_LINE_Y(i) (OPT_Y + 8 + OPT_PAD_Y + (i) * OPT_ROW_H)

static void draw_options_panel(void) {
    int prev = font_id_cur;
    font_id_cur = FONT_ID_NORMAL;
    if (!font_get_glyph(font_id_cur, 0)) { font_id_cur = prev; return; }

    draw_window_box(OPT_TILES_W, OPT_TILES_H, OPT_X, OPT_Y);

    const pixel_t ink = PIXEL_RGB(255, 255, 255);
    const pixel_t dim = PIXEL_RGB(150, 150, 165);
    draw_ascii(OPT_X + 10, OPT_LINE_Y(0), "OPTIONS", 1, ink);

    for (int i = 0; i < OPTION_ROW_COUNT; i++) {
        const OptionRow *r = &option_rows[i];
        int y = OPT_LINE_Y(i + 1);
        bool on = r->flag ? *r->flag : (*r->cycle != EB_VIEWPORT_WIDTH);
        const char *v = "OFF";
        if (r->flag) {
            v = on ? "ON" : "OFF";
        } else {
            for (int k = 0; k < VIEW_WIDTH_COUNT; k++)
                if (view_width_values[k] == *r->cycle)
                    v = view_width_names[k];
        }
        draw_ascii(OPT_X + 10, y, r->label, 1, on ? ink : dim);
        draw_ascii(OPT_X + OPT_W - 10 - measure_ascii(v), y, v, 1,
                   on ? ink : dim);
    }
    draw_ascii(OPT_X + 10, OPT_LINE_Y(OPTION_ROW_COUNT + 1), "CLOSE", 1, dim);
    font_id_cur = prev;
}

/* Returns true when the tap was consumed (so it does not also reach the
 * game or the FPS corner). */
static bool options_handle_touch(const TouchState *t) {
    if (!t->pressed)
        return false;

    if (!touch_in(t, OPT_X, OPT_Y, OPT_W, OPT_H)) {
        options_open = false;   /* tap outside closes */
        return true;
    }
    for (int i = 0; i < OPTION_ROW_COUNT; i++) {
        if (touch_in(t, OPT_X, OPT_LINE_Y(i + 1) - 4, OPT_W, OPT_ROW_H)) {
            const OptionRow *r = &option_rows[i];
            if (r->flag) {
                *r->flag = !*r->flag;
                if (r->flag == &video_opt_fps)
                    input_pulse_fps_toggle();
            } else {
                int k = 0;
                for (int j = 0; j < VIEW_WIDTH_COUNT; j++)
                    if (view_width_values[j] == *r->cycle) k = j;
                *r->cycle = view_width_values[(k + 1) % VIEW_WIDTH_COUNT];
            }
            return true;
        }
    }
    if (touch_in(t, OPT_X, OPT_LINE_Y(OPTION_ROW_COUNT + 1) - 4,
                 OPT_W, OPT_ROW_H))
        options_open = false;
    return true;   /* swallow taps inside the panel */
}

static const WindowInfo *routed_menu_window(void) {
    const WindowInfo *w = focused_window();
    if (!w || w->width == 0 || w->height == 0)
        return NULL;
    return w;
}

/* Battle: the four HP/PP panels across the top, the focused menu window
 * beneath them -- the game has already drawn both into the window layer, so
 * these are its own panels, not a reconstruction. */
static void draw_battle_bottom(void) {
    const pixel_t bg = PIXEL_RGB(0, 0, 0);

    fill_rect(0, 0, BOTTOM_WIDTH, BOTTOM_HEIGHT, bg);

    int count = game_state.player_controlled_party_count;
    if (count > HPPP_MAX_PARTY) count = HPPP_MAX_PARTY;
    if (count < 0) count = 0;

    const int panel_px_w = HPPP_WINDOW_WIDTH * 8;
    const int panel_px_h = HPPP_WINDOW_HEIGHT * 8;
    int row_w = count * panel_px_w;
    int ox = (BOTTOM_WIDTH - row_w) / 2;

    for (int i = 0; i < count; i++) {
        int tx = HPPP_CENTER_TILE - (count * HPPP_WINDOW_WIDTH) / 2
                 + i * HPPP_WINDOW_WIDTH;
        int ty = (win.battle_menu_current_character_id == (int16_t)i)
                 ? ACTIVE_HPPP_WINDOW_Y_OFFSET : NORMAL_HPPP_WINDOW_Y_OFFSET;
        if (tilemap_rect_blank(tx, ty, HPPP_WINDOW_WIDTH, HPPP_WINDOW_HEIGHT))
            continue;   /* mid-redraw: keep what is already shown */
        int dx = ox + i * panel_px_w;
        draw_tilemap_rect(tx, ty, HPPP_WINDOW_WIDTH, HPPP_WINDOW_HEIGHT,
                          dx, 6, 1);
    }

    int my = 6 + panel_px_h + 10;
    const WindowInfo *w = routed_menu_window();
    if (!w)
        return;

    int menu_w = w->width * 8;
    int menu_h = w->height * 8;
    if (menu_h > BOTTOM_HEIGHT - my)
        return;
    if (tilemap_rect_blank(w->x, w->y, w->width, w->height))
        return;

    draw_tilemap_rect(w->x, w->y, w->width, w->height,
                      (BOTTOM_WIDTH - menu_w) / 2, my, 1);
}

/* Rasterise the four HP/PP panels at the top of the bottom screen, the same
 * way draw_battle_bottom() does, and report the pixel row they end at.
 *
 * In battle the game has already drawn its panels into the window layer, so
 * that path just copies them out of VRAM. On the overworld it only draws them
 * when it decides to (the B menu), so here we drive the game's own
 * draw_hp_pp_window() into a SCRATCH COPY of win.bg2_buffer -- its RAM shadow
 * of the window tilemap -- and rasterise from that. The real shadow is
 * restored immediately and VRAM is never touched, so the top screen is
 * unaffected and an open dialogue window cannot overwrite the panel. The
 * tiles themselves are the game's, so the result is pixel-identical to the
 * battle panels rather than a reconstruction.
 *
 * Returns 0 if nothing could be drawn (no party yet, or the window tiles are
 * not resident), so the caller can fall back to the composed text panel. */
#define HPPP_OW_MARGIN_X  6
/* The cards sit at the same Y in both contexts -- draw_battle_bottom() uses
 * 6, and a different overworld Y made them jump on every battle transition.
 * The money/settings row therefore goes to the BOTTOM right: with four cards
 * centred the top-right corner is only ~48px wide, too narrow for the money
 * box's fixed 64. */
#define HPPP_CARDS_Y      6
#define HPPP_STATUSROW_Y  (BOTTOM_HEIGHT - MONEY_BOX_H - 6)
#define HPPP_SHADOW_ROW0  ACTIVE_HPPP_WINDOW_Y_OFFSET
#define HPPP_SHADOW_ROWS  (HPPP_WINDOW_HEIGHT + 2)

static int draw_overworld_hppp(int dest_y) {
    int count = game_state.player_controlled_party_count;
    if (count > HPPP_MAX_PARTY) count = HPPP_MAX_PARTY;
    if (count <= 0)
        return 0;

    /* Outside battle the cards sit left, leaving the top right for the money
     * box and (later) the options entry. Battle keeps its centred row. */
    const int panel_px_w0 = HPPP_WINDOW_WIDTH * 8;
    const int ox0 = (BOTTOM_WIDTH - count * panel_px_w0) / 2;

    /* The game is already rendering its own HP/PP windows (the B menu), so
     * they are live in VRAM. Read those rather than running a second draw --
     * draw_hp_pp_window() also fills win.hppp_window_buffer, which the real
     * path owns while it is active. */
    if (ow.render_hppp_windows & 0xFF) {
        for (int i = 0; i < count; i++) {
            int tx = HPPP_CENTER_TILE - (count * HPPP_WINDOW_WIDTH) / 2
                     + i * HPPP_WINDOW_WIDTH;
            int ty = (win.battle_menu_current_character_id == (int16_t)i)
                     ? ACTIVE_HPPP_WINDOW_Y_OFFSET : NORMAL_HPPP_WINDOW_Y_OFFSET;
            if (tilemap_rect_blank(tx, ty, HPPP_WINDOW_WIDTH, HPPP_WINDOW_HEIGHT))
                continue;
            draw_tilemap_rect(tx, ty, HPPP_WINDOW_WIDTH, HPPP_WINDOW_HEIGHT,
                              ox0 + i * panel_px_w0, dest_y, 1);
        }
        return dest_y + HPPP_WINDOW_HEIGHT * 8;
    }

    static uint8_t saved[HPPP_SHADOW_ROWS * 64];
    const size_t off = (size_t)HPPP_SHADOW_ROW0 * 64;
    memcpy(saved, &win.bg2_buffer[off], sizeof(saved));

    uint8_t save_render = ow.render_hppp_windows;
    uint8_t save_drawn  = ow.currently_drawn_hppp_windows;
    int16_t save_sel    = win.battle_menu_current_character_id;

    /* No battle-menu highlight outside battle: every panel sits on the
     * NORMAL row, so they line up as one strip. */
    win.battle_menu_current_character_id = -1;
    ow.render_hppp_windows = 1;
    ow.currently_drawn_hppp_windows = 0xFF;

    memset(&win.bg2_buffer[off], 0, sizeof(saved));
    for (int i = 0; i < count; i++)
        draw_hp_pp_window((uint16_t)i);

    const int panel_px_w = HPPP_WINDOW_WIDTH * 8;
    const int ox = (BOTTOM_WIDTH - count * panel_px_w) / 2;
    const int ty = NORMAL_HPPP_WINDOW_Y_OFFSET;

    int drawn = 0;
    for (int i = 0; i < count; i++) {
        int tx = HPPP_CENTER_TILE - (count * HPPP_WINDOW_WIDTH) / 2
                 + i * HPPP_WINDOW_WIDTH;
        draw_tilemap_rect_src(win.bg2_buffer, tx, ty,
                              HPPP_WINDOW_WIDTH, HPPP_WINDOW_HEIGHT,
                              ox + i * panel_px_w, dest_y, 1);
        drawn++;
    }

    memcpy(&win.bg2_buffer[off], saved, sizeof(saved));
    ow.render_hppp_windows = save_render;
    ow.currently_drawn_hppp_windows = save_drawn;
    win.battle_menu_current_character_id = save_sel;

    return drawn ? dest_y + HPPP_WINDOW_HEIGHT * 8 : 0;
}

static void draw_bottom_panel(void) {
    const pixel_t bg    = PIXEL_RGB(16, 16, 32);
    const pixel_t label = PIXEL_RGB(160, 160, 176);
    const pixel_t value = PIXEL_RGB(255, 255, 255);

    fill_rect(0, 0, BOTTOM_WIDTH, BOTTOM_HEIGHT, bg);

    /* Top-right status row: settings at the corner, money to its left.
     * Kept on its own row above the cards so it never sits beside a card. */
    int right = BOTTOM_WIDTH - HPPP_OW_MARGIN_X;
    right -= draw_settings_box(right, HPPP_STATUSROW_Y);
    right -= 4;
    draw_money_box(right, HPPP_STATUSROW_Y);

    /* The game's own HP/PP panels, identical to the battle ones, centred. */
    bool have_cards = draw_overworld_hppp(HPPP_CARDS_Y) > 0;
    if (options_open)
        draw_options_panel();
    if (have_cards)
        return;

    /* Fallback -- composed text, used before the window tiles are resident
     * (title screen, file select, the first frames of a load). */

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
        x += draw_eb(x, y, ch->name, 5, scale, value);

        x = 110;
        x += draw_ascii(x, y, "HP", scale, label);
        snprintf(buf, sizeof(buf), " %u/%u",
                 (unsigned)ch->current_hp, (unsigned)ch->max_hp);
        x += draw_ascii(x, y, buf, scale, value);

        x = 215;
        x += draw_ascii(x, y, "PP", scale, label);
        snprintf(buf, sizeof(buf), " %u/%u",
                 (unsigned)ch->current_pp, (unsigned)ch->max_pp);
        draw_ascii(x, y, buf, scale, value);

        y += row_h;
    }
}

/* Map a bottom-screen touch to a tile inside the window drawn there. Returns
 * false when nothing is drawn or the point falls outside it. */
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
    suppress_restore();

    /* The entry swirl lasts a few frames, which is not something a button
     * press can catch, so capture it automatically while the console build is
     * running. A handful of frames spaced through the animation is enough to
     * see what the window mask is doing. */
    if (platform_console_logging) {
        static int swirl_shots;
        static int swirl_gap;
        if (ow.battle_swirl_countdown) {
            if (swirl_shots < 4 && --swirl_gap <= 0) {
                platform_debug_dump_ppu(frame);
                swirl_shots++;
                swirl_gap = 6;
            }
        } else if (swirl_shots) {
            swirl_shots = 0;
            swirl_gap = 0;
        }
    }

    if (!platform_console_logging) {
        static int last_context = -1;
        static uint32_t last_sig;
        static uint32_t last_res;
        static int res_wait;
        static int present_left;

        /* Nothing to report until a save is actually running, or the party
         * struct still holds whatever the last previewed file-select slot
         * left behind.
         *
         * The root mode is NOT the signal: GAME_MODE_OVERWORLD is pushed in
         * game_init() and never popped, so it is the root from the first
         * frame and the intro, title and file-select cascade all run inside
         * it as its OWP_BOOT_* phases. The root's phase is the signal -- it
         * leaves the boot phases only at the handoff to actual play, and a
         * game-over "Continue" resets it to OWP_BOOT_SETUP, which correctly
         * blanks the panel through the reboot. */
        uint8_t root_phase = g_mode_stack.state[0].overworld.phase;
        bool in_game = (g_mode_stack.depth > 0 &&
                        g_mode_stack.mode[0] == GAME_MODE_OVERWORLD &&
                        root_phase != OWP_BOOT_SETUP &&
                        root_phase != OWP_BOOT_AWAIT &&
                        root_phase != OWP_BOOT_FLUSH);

        int context = !in_game ? 2 : (ow.battle_mode ? 1 : 0);

        /* Touch: the options panel takes taps while it is open, otherwise
         * the settings box opens it. Overworld only -- battle owns the
         * bottom screen for its command menu. */
        bool touch_dirty = false;
        if (context == 0) {
            const TouchState *t = touch_get();
            if (options_open) {
                if (options_handle_touch(t))
                    touch_dirty = true;
            } else if (t->pressed &&
                       touch_in(t, settings_hit_x, settings_hit_y,
                                settings_hit_w, settings_hit_h)) {
                options_open = true;
                touch_dirty = true;
            }
        } else if (options_open) {
            options_open = false;
        }
        if (context != last_context) {
            /* Elements only clear their own rectangles, so the previous
             * context's pixels would otherwise survive underneath. */
            memset(bottom_img, 0, sizeof(bottom_img));
            last_context = context;
            last_sig = 0;
            last_res = 0;
            res_wait = 0;
            present_left = 2;
        }

        if (context == 2) {
            /* Pre-game: the context switch above already cleared the image;
             * just push it to both buffers and leave the screen blank. */
        } else if (context == 1) {
            /* The battle backdrop and panels animate, so rebuild every frame. */
            draw_battle_bottom();
            present_left = 1;
        } else {
            /* The overworld panel is static between stat changes. Composing
             * and blitting 320x240 twice over every frame cost more than the
             * overworld's whole headroom -- it dropped the frame rate into the
             * mid 40s -- so only redraw when something it shows has changed.
             * Both buffers need the new image, hence two presents. */
            /* Mirror exactly what draw_hp_pp_window() reads, including the
             * party_members/party_characters indexing it uses -- a signature
             * over different fields can hold a stale panel on screen. The
             * fractions are in here because they drive the rolling digits, so
             * the panel animates a change instead of snapping to the result. */
            uint32_t sig = 2166136261u;
            int count = game_state.player_controlled_party_count;
            sig = (sig ^ (uint32_t)count) * 16777619u;
            sig = (sig ^ game_state.money_carried) * 16777619u;
            for (int i = 0; i < count && i < HPPP_MAX_PARTY; i++) {
                uint8_t member_id = game_state.party_members[i];
                if (member_id == 0 || member_id > TOTAL_PARTY_COUNT)
                    continue;
                const CharStruct *ch = &party_characters[member_id - 1];
                uint32_t vals[7] = { ch->current_hp, ch->max_hp,
                                     ch->current_pp, ch->max_pp,
                                     ch->current_hp_fraction,
                                     ch->current_pp_fraction,
                                     ch->hp_pp_window_options };
                for (int v = 0; v < 7; v++)
                    sig = (sig ^ vals[v]) * 16777619u;
                for (int c = 0; c < 5; c++)
                    sig = (sig ^ ch->name[c]) * 16777619u;
                for (int a = 0; a < AFFLICTION_GROUP_COUNT; a++)
                    sig = (sig ^ ch->afflictions[a]) * 16777619u;
            }

            /* The panel is rasterised from live CGRAM and the game's window
             * tiles, so those are inputs too -- not just the stats. Without
             * them the panel composed once just after the save loaded (before
             * load_window_gfx had composited the party-name tiles and the
             * HP/PP sub-palette was installed) and then froze, because the
             * stats never changed again. It only came right after a battle,
             * whose HP change forced a recompose.
             *
             * These are hashed separately from the stats and act on a timer:
             * CGRAM is rewritten every frame during a palette fade, and
             * recomposing on each of those would cost what composing every
             * frame used to. A stat change still redraws immediately; a
             * resource change redraws at most every RES_REDRAW_GAP frames,
             * which converges within a few frames of the fade settling.
             *
             * CGRAM 0-19 covers the five 2bpp sub-palettes the panel's
             * tilemap entries select; the 32 tiles from 0x2A0 are the
             * party-name tile pair rows. */
            #define RES_REDRAW_GAP 8
            uint32_t res = 2166136261u;
            for (int c = 0; c < 20; c++)
                res = (res ^ ppu.cgram[c]) * 16777619u;
            {
                uint32_t nt = (uint32_t)(ppu.bg_nba[1] & 0x0F) * 0x2000
                              + 0x2A0u * 16u;
                if (nt + 512u <= VRAM_SIZE)
                    for (uint32_t b = 0; b < 512u; b += 4)
                        res = (res ^ read_u16_le(&ppu.vram[nt + b])) * 16777619u;
            }
            if (res != last_res && ++res_wait >= RES_REDRAW_GAP) {
                last_res = res;
                res_wait = 0;
                last_sig = 0;   /* force the redraw below */
            } else if (res == last_res) {
                res_wait = 0;
            }
            if (sig != last_sig || touch_dirty) {
                last_sig = sig;
                draw_bottom_panel();
                present_left = 2;
            }
        }

        if (present_left > 0) {
            present_bottom();
            present_left--;
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
