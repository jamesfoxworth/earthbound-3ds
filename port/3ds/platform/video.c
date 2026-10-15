#include "platform/platform.h"
#include "game/window.h"
#include "game/overworld.h"
#include "game/text.h"
#include "snes/ppu.h"
#include "include/binary.h"
#include "game/game_state.h"
#include "core/mode_stack.h"
#include "touch.h"
#include "include/pad.h"
#include "game/inventory.h"

#define WINDOW_IN_BATTLE_TEXT 0x0E

#include "game/battle.h"
#include "game/display_text.h"
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
    /* Tell the game where the visible area ends so right-anchored overlays
     * (the FPS readout) land inside it rather than in the blanked gutter. */
    platform_visible_width = (vw > 0 && vw <= EB_VIEWPORT_WIDTH)
                             ? vw : EB_VIEWPORT_WIDTH;
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
static void draw_tilemap_rect_src(const uint8_t *shadow, int shadow_w,
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
                size_t so = ((size_t)((tile_y + ty) * shadow_w
                                      + (tile_x + tx))) * 2;
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
    draw_tilemap_rect_src(NULL, 32, tile_x, tile_y, tiles_w, tiles_h,
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
/* The dialogue font, which is also the menu font and very nearly all text in
 * the game. FONT_ID_BATTLE is what the HP/PP cards composite their party-name
 * tiles with, so it matches the cards -- but not anything else, which is why
 * it looked wrong. Window TITLES are a separate case: the game renders those
 * with FONT_ID_TINY into the top border row (render_title_to_vram ->
 * RENDER_TINY_FONT_STRING), so EB_TITLE_FONT is used for them. */
#define EB_PANEL_FONT FONT_ID_NORMAL
#define EB_TITLE_FONT FONT_ID_TINY

static int font_id_cur = FONT_ID_TINY;

/* The game's text tiles are opaque: convert_1bpp_to_2bpp() (text.c) maps
 * glyph pixels to palette colour 1 and the background to colour 3, so in-game
 * text is a solid block of near-black carrying light glyphs -- not ink over
 * whatever is behind it. Drawing bare ink is what made the options text read
 * as a different font. With font_cell_bg set, each glyph cell is filled first,
 * and both colours come from live CGRAM so they follow the player's chosen
 * window flavour. */
static bool    font_cell_bg;
static pixel_t font_cell_bg_c;

static pixel_t game_text_ink(void)  { return bgr555_to_pixel(ppu.cgram[1]); }
static pixel_t game_text_back(void) { return bgr555_to_pixel(ppu.cgram[3]); }
/* Inactive / unavailable text: the ink and the cell background mixed half
 * and half, computed per channel rather than taken from a palette slot.
 *
 * Colour 3 is the cell background, so drawing in it is invisible. Colour 2 is
 * a flavour-dependent hue -- green in some flavours -- so it read as emphasis
 * rather than as greyed out. A 50% dither was legible at a glance only in
 * theory: these glyphs are a few pixels across, so dropping every other pixel
 * destroys the letterforms and reads as a rendering fault. A true mid tone
 * keeps every pixel and just lowers the contrast. */
static pixel_t game_text_dim(void) {
    uint16_t a = ppu.cgram[1], b = ppu.cgram[3];
    uint16_t mix = (uint16_t)((((a & 0x1F) + (b & 0x1F)) / 2) |
                   (((((a >> 5) & 0x1F) + ((b >> 5) & 0x1F)) / 2) << 5) |
                   (((((a >> 10) & 0x1F) + ((b >> 10) & 0x1F)) / 2) << 10));
    return bgr555_to_pixel(mix);
}

static bool font_dither;   /* retained for the scroll arrows only */

static int draw_glyphs(int x, int y, const uint8_t *idx,
                       int count, int scale, pixel_t color) {
    uint8_t h = font_get_height(font_id_cur);
    int cx = x;
    for (int i = 0; i < count; i++) {
        const uint8_t *glyph = font_get_glyph(font_id_cur, idx[i]);
        uint8_t w = font_get_width(font_id_cur, idx[i]);
        if (glyph) {
            /* The cell spans the advance, not just the glyph: the game's
             * text tiles are opaque across the whole tile, so leaving the
             * character_padding column unfilled left a 1px transparent gap
             * between every character. */
            const int cell_w = w + character_padding;
            for (int row = 0; row < h; row++) {
                uint8_t bits = glyph[row];
                for (int col = 0; col < cell_w; col++) {
                    bool ink = (col < w && col < 8) &&
                               !(bits & (0x80 >> col));   /* clear bit = ink */
                    if (!ink && !font_cell_bg)
                        continue;
                    pixel_t c = ink ? color : font_cell_bg_c;
                    for (int sy = 0; sy < scale; sy++)
                        for (int sx = 0; sx < scale; sx++) {
                            int px = cx + col * scale + sx;
                            int py = y + row * scale + sy;
                            if (ink && font_dither && ((px + py) & 1)) {
                                if (font_cell_bg)
                                    img_px(px, py, font_cell_bg_c);
                                continue;
                            }
                            img_px(px, py, c);
                        }
                }
            }
        }
        /* The game advances by width + character_padding (text.c: every
         * measure and every VWF blit adds it). Advancing by width alone
         * crammed the glyphs a pixel tighter per character, which is part of
         * why this read as a different face than the game's own text. */
        cx += (w + character_padding) * scale;
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

/* The bottom screen is 40 tiles across, so this scratch map is 40 wide --
 * the game's own 32-wide tilemap cannot describe a full-width box, and
 * sampling it with a 32 wrap is what drew a second left border partway
 * across the options panel. */
#define FS_W 40
#define FS_H 30
static uint8_t frame_scratch[FS_W * FS_H * 2];

static void draw_window_box(int tiles_w, int tiles_h, int dest_x, int dest_y) {
    if (tiles_w > FS_W) tiles_w = FS_W;
    if (tiles_h > FS_H) tiles_h = FS_H;
    memset(frame_scratch, 0, sizeof(frame_scratch));
    for (int ty = 0; ty < tiles_h; ty++) {
        for (int tx = 0; tx < tiles_w; tx++) {
            bool t = (ty == 0), b = (ty == tiles_h - 1);
            bool l = (tx == 0), r = (tx == tiles_w - 1);
            uint16_t e = t && l ? WFRAME_TL : t && r ? WFRAME_TR
                       : t      ? WFRAME_TOP
                       : b && l ? WFRAME_BL : b && r ? WFRAME_BR
                       : b      ? WFRAME_BOTTOM
                       : l      ? WFRAME_LEFT
                       : r      ? WFRAME_RIGHT : WFRAME_FILL;
            size_t o = ((size_t)ty * FS_W + tx) * 2;
            frame_scratch[o]     = (uint8_t)(e & 0xFF);
            frame_scratch[o + 1] = (uint8_t)(e >> 8);
        }
    }
    draw_tilemap_rect_src(frame_scratch, FS_W, 0, 0, tiles_w, tiles_h,
                          dest_x, dest_y, 1);
}

static int measure_ascii(const char *str) {
    int px = 0;
    for (const char *c = str; *c; c++)
        px += font_get_width(font_id_cur,
                             (uint8_t)(ascii_to_eb_char(*c) - 0x50))
              + character_padding;
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
    font_id_cur = EB_PANEL_FONT;
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
    font_cell_bg = true;
    font_cell_bg_c = game_text_back();
    draw_ascii(tx, ty, buf, 1, game_text_ink());
    font_cell_bg = false;
    font_id_cur = prev;
    return MONEY_BOX_W;
}

/* A labelled button in the same frame style as the money box, used for the
 * Goods/PSI tabs in the status row. The active one is drawn in the text ink,
 * the inactive one in the text background colour so it reads as unselected
 * while still using the game's own palette. */
static int draw_label_box(const char *label, bool active, int x, int dest_y) {
    int prev = font_id_cur;
    font_id_cur = EB_PANEL_FONT;
    if (!font_get_glyph(font_id_cur, 0)) { font_id_cur = prev; return 0; }

    int text_px = measure_ascii(label);
    int tiles_w = 2 + (text_px + 7) / 8;
    int box_w = tiles_w * 8;
    draw_window_box(tiles_w, MONEY_TILES_H, x, dest_y);

    const int inner_y = dest_y + 8, inner_h = (MONEY_TILES_H - 2) * 8;
    int ty = inner_y + (inner_h - font_get_height(font_id_cur)) / 2;
    font_cell_bg = true;
    font_cell_bg_c = game_text_back();
    draw_ascii(x + 8, ty, label, 1,
               active ? game_text_ink() : game_text_dim());
    font_cell_bg = false;
    font_id_cur = prev;
    return box_w;
}

/* Settings entry. Not interactive yet -- touch input still has to be put
 * back -- so this is the placeholder the hub will hang off. */
static int settings_hit_x, settings_hit_y, settings_hit_w, settings_hit_h;
static int sstate_hit_x, sstate_hit_y, sstate_hit_w, sstate_hit_h;

/* Quick save-state button. Tap writes a state through the same AUX bit the
 * desktop port's F6 raises; Load state lives in the options panel rather than
 * on an icon, because a mis-tap there would throw away progress. */
static int draw_savestate_box(int right_x, int dest_y) {
    const int tiles_w = 4;
    const int box_w = tiles_w * 8;
    int x = right_x - box_w;
    sstate_hit_x = x; sstate_hit_y = dest_y;
    sstate_hit_w = box_w; sstate_hit_h = MONEY_BOX_H;
    draw_window_box(tiles_w, MONEY_TILES_H, x, dest_y);

    const pixel_t c = game_text_ink();
    int cx = x + box_w / 2, cy = dest_y + MONEY_BOX_H / 2;
    for (int dy = -6; dy <= 6; dy++)
        for (int dx = -6; dx <= 6; dx++) {
            bool body  = (dy == -6 || dy == 6 || dx == -6 || dx == 6);
            bool label = (dy >= 1 && dy <= 5 && dx >= -3 && dx <= 3);
            bool shut  = (dy >= -5 && dy <= -2 && dx >= -2 && dx <= 2);
            if (body || label || shut)
                img_px(cx + dx, cy + dy, c);
        }
    return box_w;
}

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

static const char *const view_width_names[]  = { "Wide", "4:3", "Pure" };
static const int         view_width_values[] = { EB_VIEWPORT_WIDTH, 320, SNES_WIDTH };
#define VIEW_WIDTH_COUNT ((int)(sizeof(view_width_values) / sizeof(int)))

typedef struct {
    const char *label;
    bool       *flag;    /* bool row, or NULL */
    int        *cycle;   /* cycle row value, or NULL */
    int         action;  /* OPT_ACT_* for a row that does something on tap */
} OptionRow;

bool video_opt_fps;
/* Shown as the player sees it -- Letterbox On means bars present -- and
 * inverted when handed to the game, whose flag is a disable. */
static bool opt_letterbox_on = true;

/* Rows with no value are actions, taken on tap. Save states already work on
 * this port -- platform/savestate.c is a real SD-card backend -- and the
 * trigger is the same AUX bit the desktop port's F6/F7 raise, so these only
 * have to pulse it. */
enum { OPT_ACT_NONE = 0, OPT_ACT_SAVESTATE, OPT_ACT_LOADSTATE };

static const OptionRow option_rows[] = {
    { "Screen",      NULL,              &video_opt_view_width, OPT_ACT_NONE },
    { "Letterbox",   &opt_letterbox_on, NULL, OPT_ACT_NONE },
    { "FPS meter",   &video_opt_fps,    NULL, OPT_ACT_NONE },
    { "Save state",  NULL,              NULL, OPT_ACT_SAVESTATE },
    { "Load state",  NULL,              NULL, OPT_ACT_LOADSTATE },
};
#define OPTION_ROW_COUNT ((int)(sizeof(option_rows) / sizeof(option_rows[0])))

/* Options share the panel's window, so they share its geometry. Two columns,
 * because five rows down one side of a 120px interior does not fit. */
#define OPT_COLS   2
#define OPT_ROWS   ((OPTION_ROW_COUNT + OPT_COLS - 1) / OPT_COLS)

/* Option hit rects, filled in as the rows are drawn. */
static struct { int x, y, w, h; int row; } opt_hit[OPTION_ROW_COUNT];
static int opt_hit_count;

/* Draw the option rows into the panel interior. The caller has already drawn
 * the window and its title, exactly as it does for Goods and PSI. */
static void draw_options_rows(int ox, int oy, int inner_w, int line_h,
                              int rows_fit) {
    const pixel_t ink = game_text_ink();
    const pixel_t dim = game_text_dim();
    const int col_w = inner_w / OPT_COLS;

    opt_hit_count = 0;
    for (int i = 0; i < OPTION_ROW_COUNT; i++) {
        int col = i / rows_fit, r_in = i % rows_fit;
        if (col >= OPT_COLS) break;
        const OptionRow *r = &option_rows[i];
        int x = ox + col * col_w;
        int y = oy + r_in * line_h;

        const char *v = NULL;
        bool on = true;
        if (r->flag) {
            on = *r->flag;
            v = on ? "On" : "Off";
        } else if (r->cycle) {
            for (int k = 0; k < VIEW_WIDTH_COUNT; k++)
                if (view_width_values[k] == *r->cycle)
                    v = view_width_names[k];
        }
        draw_ascii(x, y, r->label, 1, ink);
        if (v)
            draw_ascii(x + col_w - 14 - measure_ascii(v), y, v, 1,
                       on ? ink : dim);

        opt_hit[opt_hit_count].x = x - 4;
        opt_hit[opt_hit_count].y = y - 2;
        opt_hit[opt_hit_count].w = col_w;
        opt_hit[opt_hit_count].h = line_h;
        opt_hit[opt_hit_count].row = i;
        opt_hit_count++;
    }
}

static bool options_handle_touch(const TouchState *t) {
    for (int i = 0; i < opt_hit_count; i++) {
        if (!touch_in(t, opt_hit[i].x, opt_hit[i].y,
                      opt_hit[i].w, opt_hit[i].h))
            continue;
        const OptionRow *r = &option_rows[opt_hit[i].row];
        if (r->flag) {
            *r->flag = !*r->flag;
            if (r->flag == &video_opt_fps)
                input_pulse_aux(AUX_FPS_TOGGLE);
            else if (r->flag == &opt_letterbox_on)
                host_opt_no_battle_letterbox = !opt_letterbox_on;
        } else if (r->cycle) {
            int k = 0;
            for (int j = 0; j < VIEW_WIDTH_COUNT; j++)
                if (view_width_values[j] == *r->cycle) k = j;
            *r->cycle = view_width_values[(k + 1) % VIEW_WIDTH_COUNT];
        } else if (r->action == OPT_ACT_SAVESTATE) {
            input_pulse_aux(AUX_SAVESTATE);
        } else if (r->action == OPT_ACT_LOADSTATE) {
            input_pulse_aux(AUX_LOAD_STATE);
        }
        return true;
    }
    return false;
}

/* The cards sit at the same Y in both contexts -- draw_battle_bottom() uses
 * 6, and a different overworld Y made them jump on every battle transition.
 * The money/settings row therefore goes to the BOTTOM right: with four cards
 * centred the top-right corner is only ~48px wide, too narrow for the money
 * box's fixed 64. */
#define HPPP_CARDS_Y      6
#define HPPP_STATUSROW_Y  (BOTTOM_HEIGHT - MONEY_BOX_H - 6)

/* ---- Goods panel --------------------------------------------------------
 *
 * A persistent list of the selected character's Goods, in one wide window
 * under the cards -- the bottom screen showing something the game otherwise
 * keeps behind two menu levels, rather than buttons that only replay a
 * keypress. Tap a card to switch character; Ness (party slot 0) is the
 * default since he is always in the party.
 *
 * The list is composed here from game data (party_characters[].items and
 * get_item_entry()) using the game's font and window frame, because unlike
 * the HP/PP cards there is no resident window to copy -- the real Goods
 * window only exists while its menu is open. */
#define GOODS_TILES_W   38
#define GOODS_TILES_H   15
#define GOODS_BOX_W     (GOODS_TILES_W * 8)
#define GOODS_X         ((BOTTOM_WIDTH - GOODS_BOX_W) / 2)
#define GOODS_Y         (HPPP_CARDS_Y + HPPP_WINDOW_HEIGHT * 8 + 6)
#define GOODS_COLS      2
#define GOODS_ITEM_MAX  14      /* CharStruct.items[14] */

static int goods_character;   /* party slot, not member id */
/* Which content the panel is showing. Options used to be a separate floating
 * window; it is a panel mode now, so there is one window on the bottom screen
 * and its title row names whatever is in it -- the character for Goods/PSI,
 * "Options" for the settings. */
enum { PANEL_GOODS = 0, PANEL_PSI = 1, PANEL_OPTIONS = 2 };
static int panel_mode;

/* Item slot hit rects, filled in as the list is drawn, so a tap maps back to
 * the inventory slot the game expects. */
static struct { int x, y, w, h; uint8_t slot; } item_hit[GOODS_ITEM_MAX];
static int item_hit_count;
/* Tab buttons live in the status row with the money and settings boxes, not
 * in the window's title. In the title they rendered through the tiny
 * title font -- which is why they came out as blanks -- and read as part of
 * the name rather than as controls. */
static struct { int x, y, w, h; } tab_hit[2];

/* Nested item action menu, like WINDOW_INVENTORY_MENU in the real inventory
 * screen (config 0x03: 6 tiles wide, 10 tall). Opened by tapping an item
 * instead of using it outright -- a tap used to eat the Cookie with no
 * confirmation. Use and Help! are wired; Give and Drop are driven inline by
 * the pause menu's own window and character-select machinery rather than by
 * a pushable mode, so they are shown dimmed rather than half-working. */
#define IMENU_TILES_W 6
#define IMENU_TILES_H 10
#define IMENU_W       (IMENU_TILES_W * 8)
#define IMENU_H       (IMENU_TILES_H * 8)

/* One scroll offset per tab, in rows. A late-game Poo knows more PSI than
 * fits, and the list used to just stop at the last visible row with no
 * indication the rest existed. */
static int  goods_scroll[2];
static struct { int x, y, w, h, dir; } scroll_hit[2];
static int  scroll_hit_count;

static int  item_menu_slot = -1;   /* 0-based inventory slot, -1 = closed */

/* Armed PSI ability (table index), waiting for a target card tap. 0 = none.
 * Tap the ability, then tap whose card to cast it on -- the two taps supply
 * exactly the caster and target the PSI menu's own steps would have asked
 * for. */
static uint16_t psi_armed;
static struct { int x, y, w, h; uint16_t id; bool usable; } psi_hit[28];
static int psi_hit_count;
static struct { int x, y, w, h; int action; } imenu_hit[4];
static int imenu_hit_count;

/* Card hit rects, filled in as the cards are drawn. */
static struct { int x, y, w, h; } card_hit[HPPP_MAX_PARTY];
static int card_hit_count;

static void draw_goods_panel(void) {
    int prev = font_id_cur;
    font_id_cur = EB_PANEL_FONT;
    if (!font_get_glyph(font_id_cur, 0)) { font_id_cur = prev; return; }

    draw_window_box(GOODS_TILES_W, GOODS_TILES_H, GOODS_X, GOODS_Y);

    int count = game_state.player_controlled_party_count;
    if (count > HPPP_MAX_PARTY) count = HPPP_MAX_PARTY;
    if (goods_character >= count) goods_character = 0;

    uint8_t member_id = game_state.party_members[goods_character];
    if (member_id == 0 || member_id > TOTAL_PARTY_COUNT) {
        font_id_cur = prev;
        return;
    }
    const CharStruct *ch = &party_characters[member_id - 1];

    font_cell_bg = true;
    font_cell_bg_c = game_text_back();
    const pixel_t ink = game_text_ink();

    /* The character's name goes in the top border row, the way the game's
     * own menu windows carry their title -- not as a first list line, which
     * read like an inventory entry. The game uses the tiny font for titles
     * and the border row is 8px, so the tiny font is also what fits. */
    font_id_cur = EB_TITLE_FONT;
    int titlex = GOODS_X + 8;
    if (panel_mode == PANEL_OPTIONS)
        titlex += draw_ascii(titlex, GOODS_Y, "Options", 1, ink);
    else
        titlex += draw_eb(titlex, GOODS_Y, ch->name, 5, 1, ink);
    /* Tab marker sits in the title row too, so the list keeps the whole
     * interior. Tapping it swaps Goods <-> PSI. */
    (void)titlex;
    font_id_cur = EB_PANEL_FONT;

    /* Derive the row count from the real font height rather than assuming
     * one -- a fixed 7 rows overflowed the window's bottom border. Whatever
     * fits, fits; if the font is tall enough that all 14 do not, the list is
     * simply shorter rather than drawn outside the frame. */
    const int line_h = font_get_height(font_id_cur) + 1;
    const int interior_h = (GOODS_TILES_H - 2) * 8;
    int rows = interior_h / line_h;
    if (rows < 1) rows = 1;
    if (rows > (GOODS_ITEM_MAX + GOODS_COLS - 1) / GOODS_COLS)
        rows = (GOODS_ITEM_MAX + GOODS_COLS - 1) / GOODS_COLS;

    const int hy = GOODS_Y + 8;
    const int col_w = (GOODS_BOX_W - 16) / GOODS_COLS;
    item_hit_count = 0;
    psi_hit_count = 0;
    scroll_hit_count = 0;
    int list_total_rows = 0;

    if (panel_mode == PANEL_OPTIONS) {
        draw_options_rows(GOODS_X + 8, hy, GOODS_BOX_W - 16, line_h, rows);
        font_cell_bg = false;
        font_id_cur = prev;
        return;
    }

    if (panel_mode == PANEL_PSI) {
        /* Overworld-usable PSI for this character, filtered the same way
         * generate_psi_list_items() does: a non-zero learn level for this
         * party member, reached by their current level, and PSI_USE_OVERWORLD
         * in the ability's usability mask. PP cost comes from the ability's
         * battle action. Display only -- the two-tap cast still needs the PSI
         * menu's execution phases checked for the window state they assume. */
        if (ensure_battle_psi_table() && battle_psi_table) {
            /* Collect first, so the list has a length: that is what makes
             * scrolling and the arrow affordance possible. */
            uint16_t ids[PSI_MAX_ENTRIES];
            int total = 0;
            for (uint16_t i = 1; i < PSI_MAX_ENTRIES; i++) {
                const PsiAbility *psi = &battle_psi_table[i];
                if (psi->name == 0) break;
                uint8_t learn = (member_id - 1 == 0) ? psi->ness_level
                              : (member_id - 1 == 1) ? psi->paula_level
                              : (member_id - 1 == 3) ? psi->poo_level : 0;
                if (learn == 0 || learn > ch->level) continue;
                if (!(psi->usability & PSI_USE_OVERWORLD)) continue;
                ids[total++] = i;
            }
            list_total_rows = (total + GOODS_COLS - 1) / GOODS_COLS;
            int skip = goods_scroll[1] * GOODS_COLS;
            if (skip >= total) skip = 0;

            int shown = 0;
            for (int n = skip; n < total && shown < GOODS_COLS * rows; n++) {
                uint16_t i = ids[n];
                const PsiAbility *psi = &battle_psi_table[i];

                int cx = GOODS_X + 8 + (shown / rows) * col_w;
                int cy = hy + (shown % rows) * line_h;
                int ax = cx;

                /* Unaffordable PSI, and Teleport (PSI_CAT_OTHER, which needs a
                 * destination rather than a target), are shown but not
                 * castable. PS_HANDLE deducts PP without re-checking it, so
                 * the check has to happen before the request. */
                uint8_t cost_pp = battle_action_table
                    ? (battle_action_table[psi->battle_action].pp_cost & 0xFF)
                    : 0;
                bool usable = (psi->category != PSI_CAT_OTHER) &&
                              (cost_pp <= ch->current_pp);
                const pixel_t row_c = usable ? ink : game_text_dim();
                if (psi_hit_count < (int)(sizeof(psi_hit) / sizeof(psi_hit[0]))) {
                    psi_hit[psi_hit_count].x = cx;
                    psi_hit[psi_hit_count].y = cy - 2;
                    psi_hit[psi_hit_count].w = col_w;
                    psi_hit[psi_hit_count].h = line_h;
                    psi_hit[psi_hit_count].id = i;
                    psi_hit[psi_hit_count].usable = usable;
                    psi_hit_count++;
                }
                const uint8_t *nm = psi_name_entry(psi->name);
                if (nm) {
                    ax += draw_eb(ax, cy, nm, 24, 1, row_c);
                } else if (psi->name == 1) {
                    /* PSI Rockin: the name is the player's favourite thing. */
                    ax += draw_eb(ax, cy, game_state.favourite_thing,
                                  (int)sizeof(game_state.favourite_thing),
                                  1, row_c);
                }
                /* The game's own suffix label (a/b/g/S/O glyphs): several
                 * table entries share one name and differ only by level, so
                 * without it a learned Lifeup a and b read identically. A
                 * space before it keeps "Lifeup a" from running together. */
                char suffix[4];
                get_psi_suffix_label(i, suffix, sizeof(suffix));
                if (suffix[0]) {
                    ax += draw_ascii(ax, cy, " ", 1, row_c);
                    draw_ascii(ax, cy, suffix, 1, row_c);
                }

                /* PP cost right-aligned at the column edge rather than
                 * trailing the name, so the numbers line up in a readable
                 * column instead of crowding whatever the name ended on. */
                char cost[12];
                snprintf(cost, sizeof(cost), "%u",
                         (unsigned)(battle_action_table
                                    ? (battle_action_table[psi->battle_action]
                                       .pp_cost & 0xFF) : 0));
                int cw = measure_ascii(cost);
                draw_ascii(cx + col_w - 14 - cw, cy, cost, 1, row_c);
                /* The armed ability is marked, and the window title row says
                 * what to do next, so an armed cast is never a silent state. */
                if (psi_armed == i)
                    draw_ascii(cx - 7, cy, ">", 1, ink);
                shown++;
            }
        }
    } else {
        int used = 0;
        for (int i = 0; i < GOODS_ITEM_MAX; i++)
            if (ch->items[i]) used = i + 1;
        list_total_rows = (used + GOODS_COLS - 1) / GOODS_COLS;
        int gskip = goods_scroll[0] * GOODS_COLS;
        if (gskip >= GOODS_ITEM_MAX) gskip = 0;

        for (int k = 0; k + gskip < GOODS_ITEM_MAX &&
                        k < GOODS_COLS * rows; k++) {
            int i = k + gskip;
            uint8_t id = ch->items[i];
            if (id == 0)
                continue;
            const ItemConfig *it = get_item_entry(id);
            if (!it)
                continue;
            int cx = GOODS_X + 8 + (k / rows) * col_w;
            int cy = hy + (k % rows) * line_h;
            draw_eb(cx, cy, it->name, ITEM_NAME_LEN, 1, ink);
            item_hit[item_hit_count].x = cx;
            item_hit[item_hit_count].y = cy;
            item_hit[item_hit_count].w = col_w;
            item_hit[item_hit_count].h = line_h;
            item_hit[item_hit_count].slot = (uint8_t)i;
            item_hit_count++;
        }
    }

    /* Scroll affordance, only when there is something out of view. Drawn in
     * the bottom-right of the window so it never covers a list entry. */
    const int tab = (panel_mode == PANEL_PSI) ? 1 : 0;
    if (list_total_rows > rows) {
        const int bw = 22, bh = 16;
        int bx = GOODS_X + GOODS_BOX_W - 8 - bw;
        int by = GOODS_Y + GOODS_TILES_H * 8 - 8 - bh;
        for (int d = 0; d < 2; d++) {
            int dir = d ? 1 : -1;
            int ox = bx - d * (bw + 4);
            bool can = d ? (goods_scroll[tab] + rows < list_total_rows)
                         : (goods_scroll[tab] > 0);
            /* Simple triangle, so it needs no glyph that the font may lack. */
            pixel_t c = game_text_ink();
            for (int yy = 0; yy < 7; yy++) {
                int half = dir > 0 ? (6 - yy) : yy;
                for (int xx = -half; xx <= half; xx++) {
                    if (!can && ((xx + yy) & 1)) continue;
                    img_px(ox + bw / 2 + xx, by + 4 + yy, c);
                }
            }
            if (can) {
                scroll_hit[scroll_hit_count].x = ox;
                scroll_hit[scroll_hit_count].y = by;
                scroll_hit[scroll_hit_count].w = bw;
                scroll_hit[scroll_hit_count].h = bh;
                scroll_hit[scroll_hit_count].dir = dir;
                scroll_hit_count++;
            }
        }
    } else {
        goods_scroll[tab] = 0;
    }

    font_cell_bg = false;
    font_id_cur = prev;
}

static void draw_item_menu(void) {
    if (item_menu_slot < 0)
        return;
    int prev = font_id_cur;
    font_id_cur = EB_PANEL_FONT;
    if (!font_get_glyph(font_id_cur, 0)) { font_id_cur = prev; return; }

    const int x = GOODS_X + GOODS_BOX_W - IMENU_W - 6;
    const int y = GOODS_Y + 6;
    draw_window_box(IMENU_TILES_W, IMENU_TILES_H, x, y);

    static const char *const names[4] = { "Use", "Give", "Drop", "Help!" };
    const bool wired[4] = { true, false, false, true };
    const int line_h = font_get_height(font_id_cur) + 1;

    font_cell_bg = true;
    font_cell_bg_c = game_text_back();
    imenu_hit_count = 0;
    for (int i = 0; i < 4; i++) {
        int ly = y + 8 + i * line_h;
        draw_ascii(x + 8, ly, names[i], 1,
                   wired[i] ? game_text_ink() : game_text_dim());
        if (wired[i]) {
            imenu_hit[imenu_hit_count].x = x;
            imenu_hit[imenu_hit_count].y = ly - 2;
            imenu_hit[imenu_hit_count].w = IMENU_W;
            imenu_hit[imenu_hit_count].h = line_h;
            imenu_hit[imenu_hit_count].action = i;
            imenu_hit_count++;
        }
    }
    font_cell_bg = false;
    font_id_cur = prev;
}

/* Tapping a card selects whose Goods are listed. */
static bool goods_handle_touch(const TouchState *t) {
    if (!t->pressed)
        return false;

    if (touch_in(t, sstate_hit_x, sstate_hit_y, sstate_hit_w, sstate_hit_h)) {
        input_pulse_aux(AUX_SAVESTATE);
        return true;
    }
    if (touch_in(t, settings_hit_x, settings_hit_y,
                 settings_hit_w, settings_hit_h)) {
        panel_mode = (panel_mode == PANEL_OPTIONS) ? PANEL_GOODS
                                                   : PANEL_OPTIONS;
        item_menu_slot = -1;
        psi_armed = 0;
        return true;
    }
    if (panel_mode == PANEL_OPTIONS && options_handle_touch(t))
        return true;

    for (int i = 0; i < card_hit_count; i++) {
        if (touch_in(t, card_hit[i].x, card_hit[i].y,
                     card_hit[i].w, card_hit[i].h)) {
            if (psi_armed) {
                int count = game_state.player_controlled_party_count;
                uint8_t caster = (goods_character < count)
                    ? game_state.party_members[goods_character] : 0;
                uint8_t target = (i < count) ? game_state.party_members[i] : 0;
                uint16_t ability = psi_armed;
                psi_armed = 0;
                if (caster >= 1 && caster <= TOTAL_PARTY_COUNT &&
                    target >= 1 && target <= TOTAL_PARTY_COUNT) {
                    host_mode_req_a = caster;
                    host_mode_req_b = ability;
                    host_mode_req_c = target;
                    host_mode_request = HOST_MODE_REQ_PSI;
                }
                return true;
            }
            if (goods_character != i) {
                goods_character = i;
                goods_scroll[0] = goods_scroll[1] = 0;
                item_menu_slot = -1;
                psi_armed = 0;
            }
            return true;
        }
    }
    for (int i = 0; i < scroll_hit_count; i++) {
        if (touch_in(t, scroll_hit[i].x, scroll_hit[i].y,
                     scroll_hit[i].w, scroll_hit[i].h)) {
            int tab = (panel_mode == PANEL_PSI) ? 1 : 0;
            goods_scroll[tab] += scroll_hit[i].dir;
            if (goods_scroll[tab] < 0) goods_scroll[tab] = 0;
            item_menu_slot = -1;
            return true;
        }
    }
    /* Each half of the tab selects its own side rather than toggling, so a
     * tap on the label you can already see is a no-op instead of a surprise. */
    /* The action menu owns taps while it is open. */
    if (item_menu_slot >= 0) {
        for (int i = 0; i < imenu_hit_count; i++) {
            if (!touch_in(t, imenu_hit[i].x, imenu_hit[i].y,
                          imenu_hit[i].w, imenu_hit[i].h))
                continue;
            int slot = item_menu_slot;
            int count = game_state.player_controlled_party_count;
            uint8_t mid = (goods_character < count)
                          ? game_state.party_members[goods_character] : 0;
            item_menu_slot = -1;
            if (mid < 1 || mid > TOTAL_PARTY_COUNT)
                return true;

            /* char_id is a MEMBER id and item_slot is 1-based: the game reads
             * party_characters[char_id - 1].items[slot - 1]. */
            if (imenu_hit[i].action == 0) {
                host_mode_req_a = mid;
                host_mode_req_b = (uint16_t)(slot + 1);
                host_mode_request = HOST_MODE_REQ_USE_ITEM;
            } else {
                uint8_t id = party_characters[mid - 1].items[slot];
                const ItemConfig *it = id ? get_item_entry(id) : NULL;
                if (it) {
                    host_mode_req_addr = it->help_text & 0xFFFFFF;
                    host_mode_request = HOST_MODE_REQ_SHOW_TEXT;
                }
            }
            return true;
        }
        item_menu_slot = -1;   /* tap elsewhere closes it */
        return true;
    }

    for (int i = 0; i < 2; i++) {
        if (tab_hit[i].w && touch_in(t, tab_hit[i].x, tab_hit[i].y,
                                     tab_hit[i].w, tab_hit[i].h)) {
            panel_mode = (i == 1) ? PANEL_PSI : PANEL_GOODS;
            item_menu_slot = -1;
            return true;
        }
    }
    /* Tapping an item hands the game the same inputs the Goods menu would:
     * GAME_MODE_USE_ITEM classifies usability, shows the check/description
     * text and runs targeting itself, so Cookies heal, Cold Remedy cures and
     * an unusable item just reports why -- all the game's own rules. */
    for (int i = 0; i < psi_hit_count; i++) {
        if (touch_in(t, psi_hit[i].x, psi_hit[i].y,
                     psi_hit[i].w, psi_hit[i].h)) {
            psi_armed = psi_hit[i].usable
                        ? (psi_armed == psi_hit[i].id ? 0 : psi_hit[i].id)
                        : 0;
            return true;
        }
    }
    for (int i = 0; i < item_hit_count; i++) {
        if (touch_in(t, item_hit[i].x, item_hit[i].y,
                     item_hit[i].w, item_hit[i].h)) {
            item_menu_slot = item_hit[i].slot;
            return true;
        }
    }
    return false;
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
    /* Flat black behind the panels. Mirroring the battle scene here was tried
     * and dropped: a port only receives composited scanlines, so the copy
     * carried the enemy sprites along with the background, and a sprite
     * sitting behind the UI is more distracting than a plain backdrop.
     * Isolating the BG layers would need a second full PPU pass. */
    fill_rect(0, 0, BOTTOM_WIDTH, BOTTOM_HEIGHT, PIXEL_RGB(0, 0, 0));

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
        if (tilemap_rect_blank(tx, ty, HPPP_WINDOW_WIDTH, HPPP_WINDOW_HEIGHT))
            continue;   /* mid-redraw: keep what is already shown */
        int dx = ox + i * panel_px_w;
        draw_tilemap_rect(tx, ty, HPPP_WINDOW_WIDTH, HPPP_WINDOW_HEIGHT,
                          dx, 6, 1);
    }

    int my = 6 + HPPP_WINDOW_HEIGHT * 8 + 10;

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
        card_hit_count = count;
        for (int i = 0; i < count; i++) {
            card_hit[i].x = ox0 + i * panel_px_w0;
            card_hit[i].y = dest_y;
            card_hit[i].w = panel_px_w0;
            card_hit[i].h = HPPP_WINDOW_HEIGHT * 8;
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
        draw_tilemap_rect_src(win.bg2_buffer, 32, tx, ty,
                              HPPP_WINDOW_WIDTH, HPPP_WINDOW_HEIGHT,
                              ox + i * panel_px_w, dest_y, 1);
        card_hit[i].x = ox + i * panel_px_w;
        card_hit[i].y = dest_y;
        card_hit[i].w = panel_px_w;
        card_hit[i].h = HPPP_WINDOW_HEIGHT * 8;
        drawn++;
    }

    card_hit_count = drawn;
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
    right -= draw_savestate_box(right, HPPP_STATUSROW_Y);
    right -= 4;
    right -= draw_settings_box(right, HPPP_STATUSROW_Y);
    right -= 4;
    draw_money_box(right, HPPP_STATUSROW_Y);

    /* Goods / PSI tabs at the left of the same row. */
    int lx = HPPP_OW_MARGIN_X;
    static const char *const tab_names[2] = { "Goods", "PSI" };
    for (int i = 0; i < 2; i++) {
        int w = draw_label_box(tab_names[i], panel_mode == (i == 1 ? PANEL_PSI : PANEL_GOODS),
                               lx, HPPP_STATUSROW_Y);
        tab_hit[i].x = lx;
        tab_hit[i].y = HPPP_STATUSROW_Y;
        tab_hit[i].w = w;
        tab_hit[i].h = MONEY_BOX_H;
        lx += w + 4;
    }

    /* The game's own HP/PP panels, identical to the battle ones, centred. */
    bool have_cards = draw_overworld_hppp(HPPP_CARDS_Y) > 0;
    if (have_cards) {
        draw_goods_panel();
        draw_item_menu();
        /* An armed PSI marks the cards rather than printing a prompt in the
         * panel: the question is "which of these", so asking it on the
         * things being chosen is both clearer and keeps the panel clean. */
        if (psi_armed) {
            const pixel_t c = game_text_ink();
            for (int i = 0; i < card_hit_count; i++) {
                int mx = card_hit[i].x + card_hit[i].w / 2;
                int my = card_hit[i].y + card_hit[i].h + 2;
                for (int yy = 0; yy < 6; yy++)
                    for (int xx = -(5 - yy); xx <= (5 - yy); xx++)
                        img_px(mx + xx, my + yy, c);
            }
        }
    }

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
            if (goods_handle_touch(t))
                touch_dirty = true;
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
            sig = (sig ^ (uint32_t)goods_character) * 16777619u;
            sig = (sig ^ (uint32_t)panel_mode) * 16777619u;
            sig = (sig ^ (uint32_t)(item_menu_slot + 1)) * 16777619u;
            sig = (sig ^ (uint32_t)goods_scroll[0]) * 16777619u;
            sig = (sig ^ (uint32_t)goods_scroll[1]) * 16777619u;
            sig = (sig ^ (uint32_t)psi_armed) * 16777619u;
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
                for (int k = 0; k < 14; k++)
                    sig = (sig ^ ch->items[k]) * 16777619u;
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
