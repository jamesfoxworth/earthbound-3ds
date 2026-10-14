#include "platform/platform.h"
#include "game_main.h"
#include <3ds.h>

static uint64_t frame_start_ticks;
static const uint64_t ticks_per_sec = SYSCLOCK_ARM11;

/* Same IIR-smoothed FPS filter as port/unix/platform/sdl2_timer.c */
#define IIR_SHIFT 4
static uint32_t fps_tenths_acc;

bool platform_timer_init(void) {
    fps_tenths_acc = (uint32_t)TARGET_FPS * 10 << IIR_SHIFT;
    return true;
}

void platform_timer_shutdown(void) {
}

uint64_t platform_timer_ticks(void) {
    return svcGetSystemTick();
}

uint64_t platform_timer_ticks_per_sec(void) {
    return ticks_per_sec;
}

void platform_timer_frame_start(void) {
    frame_start_ticks = platform_timer_ticks();
}

void platform_timer_frame_end(void) {
    int fps = game_is_fast_forward() ? TARGET_FPS * EB_FAST_FORWARD_MULTIPLIER : TARGET_FPS;
    uint64_t target_ticks = ticks_per_sec / (uint64_t)fps;

    uint64_t deadline = frame_start_ticks + target_ticks;
    platform_timer_sleep_until(deadline);

    platform_timer_update_fps();
}

void platform_timer_update_fps(void) {
    uint64_t elapsed = platform_timer_ticks() - frame_start_ticks;
    if (elapsed > 0) {
        uint32_t instant_fps_tenths = (uint32_t)(ticks_per_sec * 10 / elapsed);
        fps_tenths_acc = fps_tenths_acc - (fps_tenths_acc >> IIR_SHIFT) + instant_fps_tenths;
    }
}

void platform_timer_sleep_until(uint64_t deadline) {
    uint64_t now = platform_timer_ticks();
    if ((int64_t)(deadline - now) > 0) {
        uint64_t remaining_ticks = deadline - now;
        int64_t ns = (int64_t)(remaining_ticks * 1000000000ULL / ticks_per_sec);
        if (ns > 0)
            svcSleepThread(ns);
    }
}

uint32_t platform_timer_get_fps_tenths(void) {
    return fps_tenths_acc >> IIR_SHIFT;
}
