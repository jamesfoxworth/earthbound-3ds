/*
 * Debug dumps — no-op for milestone 1. Desktop writes BMP/binary dumps to
 * debug/; a 3DS version could do the same to the SD card, but it's not
 * needed to get the port booting.
 */
#include "platform/platform.h"

void platform_debug_dump_ppu(const pixel_t *framebuffer) {
    (void)framebuffer;
}

void platform_debug_dump_vram_image(void) {
}
