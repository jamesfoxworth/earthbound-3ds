/*
 * Audio stub — milestone 1 builds with EB_ENABLE_AUDIO=OFF, but platform.h
 * declares these unconditionally, so they must exist to link. A real ndsp
 * backend is a follow-up milestone.
 */
#include "platform/platform.h"

bool platform_audio_init(void) {
    return true;
}

void platform_audio_shutdown(void) {
}

void platform_audio_lock(void) {
}

void platform_audio_unlock(void) {
}
