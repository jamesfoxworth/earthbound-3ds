/*
 * 3DS save data — file-backed, same scheme as port/unix/platform/sdl2_save.c.
 * Relative paths resolve against the .3dsx's launch directory on the SD
 * card, same as any devkitARM homebrew app's working directory.
 */
#include "platform/platform.h"
#include <stdio.h>

static const char *save_file_path = "earthbound.srm";

bool platform_save_init(void) {
    return true;
}

size_t platform_save_read(void *dst, size_t offset, size_t size) {
    FILE *f = fopen(save_file_path, "rb");
    if (!f) return 0;

    if (fseek(f, (long)offset, SEEK_SET) != 0) {
        fclose(f);
        return 0;
    }

    size_t read = fread(dst, 1, size, f);
    fclose(f);
    return read;
}

bool platform_save_write(const void *src, size_t offset, size_t size) {
    FILE *f = fopen(save_file_path, "r+b");
    if (!f) {
        f = fopen(save_file_path, "w+b");
        if (!f) return false;
    }

    if (fseek(f, (long)offset, SEEK_SET) != 0) {
        fclose(f);
        return false;
    }

    size_t written = fwrite(src, 1, size, f);
    fclose(f);
    return written == size;
}
