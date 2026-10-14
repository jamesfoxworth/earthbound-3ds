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
    platform_video_init();
    platform_input_init();
    platform_save_init();
    platform_timer_init();

    /* Launch-time options, read once before the game starts:
     *   SELECT — boot straight to the overworld (the desktop --skip-intro path)
     *   L      — route the engine's LOG_WARN stream to the bottom-screen
     *            console. verbose_level is already 1, so those warnings have
     *            been firing into a stderr with no sink on this platform;
     *            they cover unhandled script callbacks, unknown banks and
     *            unimplemented opcodes, which is what to read when a scripted
     *            scene misbehaves. Off by default: it is chatty and costs a
     *            console write per warning. */
    hidScanInput();
    {
        u32 launch_keys = hidKeysHeld();
        if (launch_keys & KEY_SELECT)
            platform_skip_intro = true;
        if (launch_keys & KEY_L) {
            consoleDebugInit(debugDevice_CONSOLE);
            platform_console_logging = true;
            /* Leave verbose_level at its default 1 (LOG_WARN). Raising it to 2
             * enables LOG_TRACE, which includes per-entity, per-frame sprite
             * tracing in callbacks.c — that floods the console and drops the
             * frame rate to single digits. Targeted diagnostics get their own
             * compile-time gate instead (e.g. EB_DEBUG_DOORS).
             *
             * Confirm on screen that it armed: without this, "nothing printed"
             * is ambiguous between the key not being read at launch and the
             * log simply having nothing to say. */
            printf("log: on\n");
        }
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
