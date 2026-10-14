#include "platform/platform.h"
#include "game_main.h"

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
