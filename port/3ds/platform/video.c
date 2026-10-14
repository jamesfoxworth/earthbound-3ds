#include "platform/platform.h"
#include <3ds.h>

/*
 * Milestone 1: native SNES viewport (EB_VIEWPORT_WIDTH/HEIGHT = 256x224),
 * centered on the physical 400x240 top screen. No scaling, no widescreen
 * fill, no stereo 3D, no bottom-screen use yet.
 */
#define PHYS_WIDTH  400
#define PHYS_HEIGHT 240
#define OFFSET_X ((PHYS_WIDTH  - EB_VIEWPORT_WIDTH)  / 2)
#define OFFSET_Y ((PHYS_HEIGHT - EB_VIEWPORT_HEIGHT) / 2)

bool platform_video_init(void) {
    gfxInitDefault();
    gfxSetScreenFormat(GFX_TOP, GSP_RGB565_OES);
    gfxSetDoubleBuffering(GFX_TOP, true);
    gfxSetDoubleBuffering(GFX_BOTTOM, false);
    return true;
}

void platform_video_shutdown(void) {
    gfxExit();
}

void platform_video_begin_frame(void) {
    /* Nothing to prepare — scanlines are written directly into the current
     * back buffer by send_scanline() below. */
}

/*
 * The 3DS top-screen framebuffer is physically rotated: the LCD panel is
 * mounted in portrait orientation, so buffer memory is column-major with Y
 * flipped relative to normal screen coordinates. Pixel (x, y) lives at:
 *   offset = x * PHYS_HEIGHT + (PHYS_HEIGHT - 1 - y)
 * This is the standard libctru direct-framebuffer convention (see
 * devkitPro's camera/video example for the same transform).
 */
void platform_video_send_scanline(int y, const pixel_t *pixels) {
    u16 *fb = (u16 *)gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL);
    int row = PHYS_HEIGHT - 1 - (OFFSET_Y + y);
    for (int x = 0; x < EB_VIEWPORT_WIDTH; x++) {
        int col = OFFSET_X + x;
        fb[col * PHYS_HEIGHT + row] = pixels[x];
    }
}

pixel_t *platform_video_get_framebuffer(void) {
    return NULL; /* streaming target: no full-frame pointer to hand back */
}

void platform_video_end_frame(void) {
    gfxFlushBuffers();
    gspWaitForVBlank();
    gfxSwapBuffers();
}

void platform_video_set_vsync(bool enabled) {
    (void)enabled; /* always paced by gspWaitForVBlank() above */
}
