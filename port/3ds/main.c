#include "platform/platform.h"
#include "game_main.h"
#include <3ds.h>

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

    /* Hold SELECT at launch to boot straight to the overworld, the same path
     * the desktop port's --skip-intro takes. Useful while the intro sequence
     * is hard to get out of: attract mode only checks for a button in its
     * AT_MAIN phase, and each scene runs as a child mode that holds the
     * stack for the scene's duration. */
    hidScanInput();
    if (hidKeysHeld() & KEY_SELECT)
        platform_skip_intro = true;

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
