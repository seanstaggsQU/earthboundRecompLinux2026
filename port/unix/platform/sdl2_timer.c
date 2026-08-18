#include "platform/platform.h"
#include "game_main.h"
#include <SDL.h>
#ifdef _WIN32
#include <windows.h>
#include <mmsystem.h>  /* timeBeginPeriod/timeEndPeriod (winmm) */
#endif

static uint64_t frame_start_ticks;
static uint64_t ticks_per_sec;

/*
 * IIR smoothing with 4 extra bits of fractional precision.
 * Update:  acc = acc - (acc >> SHIFT) + sample
 * Read:    acc >> SHIFT
 * At steady state with constant input S: output converges to S.
 */
#define IIR_SHIFT 4
static uint32_t fps_tenths_acc;    /* FPS * 10, shifted by IIR_SHIFT */

bool platform_timer_init(void) {
    ticks_per_sec = SDL_GetPerformanceFrequency();
    fps_tenths_acc = (uint32_t)TARGET_FPS * 10 << IIR_SHIFT;
#ifdef _WIN32
    /* platform_timer_frame_end() paces frames with a manual SDL_Delay()
     * sleep-to-deadline, running alongside the renderer's blocking VSync
     * (SDL_RenderSetVSync in sdl2_video.c) -- two independent pacers on
     * the same loop. That's fine when the sleep is precise, but Windows'
     * *default* system timer resolution is ~15.6ms (vs ~1ms typical on
     * Linux/macOS), so SDL_Delay(1) there can genuinely block for up to
     * 15ms: the manual pacer routinely overshoots its 1/60s budget by an
     * amount comparable to a whole frame, which then collides with
     * VSync's own blocking wait unpredictably from frame to frame. That's
     * exactly the shape of "slight desync"/jitter a player would notice
     * on Windows and nowhere else. timeBeginPeriod(1) raises the *process*
     * timer resolution to 1ms, which is the standard, well-known fix for
     * this class of Sleep()-imprecision issue in Windows games. Paired
     * with timeEndPeriod(1) in platform_timer_shutdown(). */
    timeBeginPeriod(1);
#endif
    return true;
}

void platform_timer_shutdown(void) {
#ifdef _WIN32
    timeEndPeriod(1);
#endif
}

uint64_t platform_timer_ticks(void) {
    return SDL_GetPerformanceCounter();
}

uint64_t platform_timer_ticks_per_sec(void) {
    return ticks_per_sec;
}

void platform_timer_frame_start(void) {
    frame_start_ticks = platform_timer_ticks();
}

void platform_timer_frame_end(void) {
    if (platform_headless)
        return;
    int fps = game_is_fast_forward() ? TARGET_FPS * EB_FAST_FORWARD_MULTIPLIER : TARGET_FPS;
    uint64_t target_ticks = ticks_per_sec / fps;

    /* Sleep until the frame budget is reached. */
    uint64_t deadline = frame_start_ticks + target_ticks;
    platform_timer_sleep_until(deadline);

    /* Update FPS filter */
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
        uint32_t remaining_ms = (uint32_t)((deadline - now) * 1000 / ticks_per_sec);
        if (remaining_ms > 0)
            SDL_Delay(remaining_ms);
    }
}

uint32_t platform_timer_get_fps_tenths(void) {
    return fps_tenths_acc >> IIR_SHIFT;
}
