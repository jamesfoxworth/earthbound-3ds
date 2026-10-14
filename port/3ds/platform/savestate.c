/*
 * 3DS savestate storage — file-backed ping-pong slots on the SD card, same
 * scheme as port/unix/platform/sdl2_savestate.c. EB_EMBEDDED is not defined
 * for this port (the 3DS has a real filesystem), so this is the backend —
 * the no-op stub in src/platform/savestate_backend_stub.c does not apply.
 */
#include "platform/platform.h"
#include <stdio.h>

static const char *slot_path(int slot) {
    return slot == 0 ? "savestate.bin.0" : "savestate.bin.1";
}

static FILE *slot_writer[SAVESTATE_SLOTS];

bool platform_savestate_begin(int slot) {
    if (slot < 0 || slot >= SAVESTATE_SLOTS)
        return false;
    if (slot_writer[slot]) {
        fclose(slot_writer[slot]);
        slot_writer[slot] = NULL;
    }
    FILE *f = fopen(slot_path(slot), "wb");
    if (!f)
        return false;
    slot_writer[slot] = f;
    return true;
}

bool platform_savestate_write(int slot, size_t offset, const void *src, size_t size) {
    if (slot < 0 || slot >= SAVESTATE_SLOTS || !slot_writer[slot])
        return false;
    if (fseek(slot_writer[slot], (long)offset, SEEK_SET) != 0)
        return false;
    return fwrite(src, 1, size, slot_writer[slot]) == size;
}

bool platform_savestate_commit(int slot) {
    if (slot < 0 || slot >= SAVESTATE_SLOTS || !slot_writer[slot])
        return false;
    FILE *f = slot_writer[slot];
    slot_writer[slot] = NULL;
    bool ok = (fflush(f) == 0);
    if (fclose(f) != 0)
        ok = false;
    return ok;
}

size_t platform_savestate_read(int slot, size_t offset, void *dst, size_t size) {
    if (slot < 0 || slot >= SAVESTATE_SLOTS)
        return 0;
    FILE *f = fopen(slot_path(slot), "rb");
    if (!f)
        return 0;
    size_t r = 0;
    if (fseek(f, (long)offset, SEEK_SET) == 0)
        r = fread(dst, 1, size, f);
    fclose(f);
    return r;
}

void *platform_savestate_scratch(size_t *out_bytes) {
    static unsigned char scratch[16 * 1024];
    if (out_bytes)
        *out_bytes = sizeof(scratch);
    return scratch;
}
