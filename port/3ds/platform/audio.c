/*
 * 3DS audio backend (ndsp).
 *
 * The SPC700/DSP emulator produces exactly SAMPLES_PER_FRAME stereo samples
 * per call, which must keep running at that rate for the emulated APU to stay
 * in time. A worker thread keeps ndsp's wave buffers fed, mirroring the
 * desktop port's callback-thread model, so platform_audio_lock/unlock guard
 * the same shared state they do there.
 *
 * ndspInit() fails on a console whose DSP firmware has never been dumped,
 * which is common. That is reported and treated as "no audio": the game runs
 * silently rather than refusing to start.
 */
#include "platform/platform.h"
#include "game_main.h"
#include "game/audio.h"
#include "core/log.h"

#ifdef EB_ENABLE_AUDIO

#include <3ds.h>
#include <string.h>

#define AUDIO_SAMPLE_RATE  32000
#define SAMPLES_PER_FRAME  534   /* 32040 / 60, matching port/unix */
#define AUDIO_CHANNEL      0

/* Four buffers give ~67ms of slack, so an over-budget frame (the widescreen
 * overworld spikes past its 16.7ms budget) does not starve the DSP. */
#define WAVE_BUF_COUNT     4

#define FRAME_SAMPLES      (SAMPLES_PER_FRAME * 2)             /* interleaved */
#define FRAME_BYTES        (FRAME_SAMPLES * sizeof(int16_t))

static ndspWaveBuf wave_buf[WAVE_BUF_COUNT];
static int16_t *audio_buf;          /* linear memory: DSP reads it directly */

/* Recursive, not a LightLock: audio_generate_samples() takes audio_lock()
 * itself, so the worker below locks the same mutex twice per buffer. The
 * desktop port gets away with the same nesting because SDL_CreateMutex()
 * returns a recursive mutex; a plain LightLock would deadlock on the first
 * refill. */
static RecursiveLock audio_mutex;
static Thread audio_thread;
static volatile bool audio_running;
static bool audio_active;           /* false when ndsp is unavailable */

static void audio_thread_main(void *arg) {
    (void)arg;
    while (audio_running) {
        bool filled = false;

        for (int i = 0; i < WAVE_BUF_COUNT; i++) {
            /* FREE is the initial state; DONE is a buffer the DSP finished. */
            if (wave_buf[i].status != NDSP_WBUF_FREE &&
                wave_buf[i].status != NDSP_WBUF_DONE)
                continue;

            int16_t *dst = (int16_t *)wave_buf[i].data_pcm16;

            if (game_is_fast_forward()) {
                /* Same as desktop: don't try to play back sped-up audio. */
                memset(dst, 0, FRAME_BYTES);
            } else {
                RecursiveLock_Lock(&audio_mutex);
                audio_generate_samples(dst, SAMPLES_PER_FRAME);
                RecursiveLock_Unlock(&audio_mutex);
            }

            DSP_FlushDataCache(dst, FRAME_BYTES);
            ndspChnWaveBufAdd(AUDIO_CHANNEL, &wave_buf[i]);
            filled = true;
        }

        /* All buffers are queued; wait rather than spin on the status words. */
        if (!filled)
            svcSleepThread(1000000ULL);  /* 1ms */
    }
    threadExit(0);
}

bool platform_audio_init(void) {
    RecursiveLock_Init(&audio_mutex);

    if (R_FAILED(ndspInit())) {
        LOG_WARN("audio: ndspInit failed (is the DSP firmware dumped?) — running silent\n");
        return false;
    }

    audio_buf = (int16_t *)linearAlloc(WAVE_BUF_COUNT * FRAME_BYTES);
    if (!audio_buf) {
        LOG_WARN("audio: could not allocate %u bytes of linear memory\n",
                 (unsigned)(WAVE_BUF_COUNT * FRAME_BYTES));
        ndspExit();
        return false;
    }
    memset(audio_buf, 0, WAVE_BUF_COUNT * FRAME_BYTES);

    ndspSetOutputMode(NDSP_OUTPUT_STEREO);
    ndspChnSetInterp(AUDIO_CHANNEL, NDSP_INTERP_LINEAR);
    ndspChnSetRate(AUDIO_CHANNEL, AUDIO_SAMPLE_RATE);
    ndspChnSetFormat(AUDIO_CHANNEL, NDSP_FORMAT_STEREO_PCM16);

    float mix[12];
    memset(mix, 0, sizeof(mix));
    mix[0] = 1.0f;  /* front left */
    mix[1] = 1.0f;  /* front right */
    ndspChnSetMix(AUDIO_CHANNEL, mix);

    audio_init();  /* engine side: SPC700/DSP emulator + audio packs */

    memset(wave_buf, 0, sizeof(wave_buf));
    for (int i = 0; i < WAVE_BUF_COUNT; i++) {
        wave_buf[i].data_vaddr = &audio_buf[i * FRAME_SAMPLES];
        wave_buf[i].nsamples = SAMPLES_PER_FRAME;
        /* status is NDSP_WBUF_FREE from the memset, so the worker's first
         * pass fills and queues every buffer. */
    }

    /* Run a notch above the game thread so refills win against a long frame,
     * on the application core (processor 2 needs an exheader flag homebrew
     * cannot rely on). The work is ~534 samples per 16ms, so sharing core 0
     * is cheap. */
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    prio -= 1;
    if (prio < 0x18) prio = 0x18;
    if (prio > 0x3F) prio = 0x3F;

    audio_running = true;
    audio_thread = threadCreate(audio_thread_main, NULL, 8 * 1024, prio, 0, false);
    if (!audio_thread) {
        LOG_WARN("audio: could not create the mixer thread — running silent\n");
        audio_running = false;
        audio_shutdown();
        linearFree(audio_buf);
        audio_buf = NULL;
        ndspExit();
        return false;
    }

    audio_active = true;
    return true;
}

void platform_audio_shutdown(void) {
    if (!audio_active)
        return;

    audio_running = false;
    if (audio_thread) {
        threadJoin(audio_thread, UINT64_MAX);
        threadFree(audio_thread);
        audio_thread = NULL;
    }

    ndspChnWaveBufClear(AUDIO_CHANNEL);
    audio_shutdown();
    ndspExit();

    if (audio_buf) {
        linearFree(audio_buf);
        audio_buf = NULL;
    }
    audio_active = false;
}

void platform_audio_lock(void) {
    if (audio_active) RecursiveLock_Lock(&audio_mutex);
}

void platform_audio_unlock(void) {
    if (audio_active) RecursiveLock_Unlock(&audio_mutex);
}

#else /* !EB_ENABLE_AUDIO */

bool platform_audio_init(void) { return true; }
void platform_audio_shutdown(void) {}
void platform_audio_lock(void) {}
void platform_audio_unlock(void) {}

#endif /* EB_ENABLE_AUDIO */
