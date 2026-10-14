#include "platform/platform.h"
#include "game_main.h"
#include "core/log.h"
#include <3ds.h>

/* Defined in platform/video.c */
extern bool platform_console_logging;

/*
 * Single top-level game loop: advance the game one frame (game_loop_step),
 * then perform the one per-frame host yield (host_process_frame), then let
 * the capture-safety gate run any pending savestate request. Same shape as
 * port/unix/main.c, minus CLI args and the verify/selftest paths.
 */
int main(void) {
    /* Launch-time options, read before video init because the bottom screen is
     * set up differently for each:
     *   SELECT — boot straight to the overworld (the desktop --skip-intro path)
     *   L      — give the bottom screen to a console carrying the engine's
     *            LOG_WARN stream, instead of the UI mirror. verbose_level is
     *            already 1, so those warnings have been firing into a stderr
     *            with no sink on this platform; they cover unhandled script
     *            callbacks, unknown banks and unimplemented opcodes, which is
     *            what to read when a scripted scene misbehaves.
     *
     * Not raising verbose_level to 2 here: that enables LOG_TRACE, which
     * includes per-entity, per-frame sprite tracing in callbacks.c — it floods
     * the console and drops the frame rate to single digits. Targeted
     * diagnostics get their own compile-time gate (e.g. EB_DEBUG_DOORS). */
    hidScanInput();
    u32 launch_keys = hidKeysHeld();
    if (launch_keys & KEY_SELECT)
        platform_skip_intro = true;
    if (launch_keys & KEY_L)
        platform_console_logging = true;

    platform_video_init();
    platform_input_init();
    platform_save_init();
    platform_timer_init();

    if (platform_console_logging) {
        consoleDebugInit(debugDevice_CONSOLE);
        /* Confirm on screen that it armed: without this, "nothing printed" is
         * ambiguous between the key not being read at launch and the log
         * simply having nothing to say. */
        printf("log: on\n");
    }

    game_init();

    platform_audio_init();

    platform_timer_frame_start();

    while (!platform_input_quit_requested()) {
        game_loop_step();
        host_process_frame();
        host_root_boundary();
    }

    platform_timer_shutdown();
    platform_audio_shutdown();
    platform_input_shutdown();
    platform_video_shutdown();

    return 0;
}
