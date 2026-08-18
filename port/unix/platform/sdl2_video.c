#include "platform/platform.h"
#include <SDL.h>
#include <stdio.h>
#include <string.h>

/* Initial window scale factor (window is resizable) */
#ifndef EB_WINDOW_SCALE
#define EB_WINDOW_SCALE 3
#endif

/* Default (non-zoomed) width shown by platform_video_end_frame() --
 * today's shipped widescreen baseline, deliberately NOT SNES_WIDTH (256,
 * the original native SNES resolution) or EB_VIEWPORT_WIDTH (512, the new
 * always-compiled zoom-out max). See the CMakeLists.txt comment: the
 * compile-time canvas grew to fit the zoom, but the default on-screen
 * footprint stays exactly what every already-shipped build already showed. */
#define EB_DEFAULT_WIDTH 400

/* EB_ZOOM_OUT display size -- deliberately ONE TILE (8px) smaller than the
 * compiled max on each edge, not the full EB_VIEWPORT_WIDTH/HEIGHT.
 *
 * map_loader.c's fill_tilemaps()/fill_collision_tiles() center their
 * exactly-64-tile-wide (512px) / 32-tile-tall (256px) VRAM fill on
 * `(camera_pixel_position >> 3) - EB_VIEWPORT_CENTER_X/8` -- i.e. the
 * camera's tile position, FLOORED to the nearest tile boundary. The camera
 * itself scrolls smoothly in real pixels, not tile-aligned, so that floor
 * can sit up to 7px short of where a pixel-precise fill would need to
 * start. At the DEFAULT 400x224 crop this was completely invisible: the
 * fill's fixed 512x256 budget has 112px/32px of slack on every edge beyond
 * what 400x224 needs, easily absorbing a 7px floor error. Cropping to the
 * FULL 512x256 when zoomed out removes that slack entirely (0 margin left,
 * since 512/256 IS the fill's exact budget) -- the floor error becomes a
 * real, visible sliver of stale VRAM (leftover tile data from wherever the
 * camera was several frames ago, before the ring-buffer tilemap's `& 63`/
 * `& 31` addressing wrapped that slot's write) at the trailing edge,
 * confirmed live on a real display (screenshots showed a seam/corrupted
 * strip specifically at the right edge, exactly where this predicts it).
 * Backing off by one tile on every edge guarantees the crop always stays
 * inside the fill's actual coverage regardless of the camera's exact
 * sub-tile position at any given moment. */
#define EB_ZOOM_OUT_WIDTH  (EB_VIEWPORT_WIDTH  - 16)
#define EB_ZOOM_OUT_HEIGHT (EB_VIEWPORT_HEIGHT - 16)

/* EB_ZOOM_IN display size -- roughly 1.5x magnification (crop ~2/3 of the
 * default footprint's linear dimensions, matching its exact aspect ratio
 * so it needs no display-aspect adaptation the way EB_ZOOM_OUT does).
 * Unlike zoom-out, zoom-in has no VRAM/tile-fill safety-margin concern at
 * all: this crop sits entirely INSIDE the already-safe, already-centered
 * default 400x224 region (itself well inside the fill's 512x256 budget),
 * so there's no edge to approach in the first place -- just a smaller
 * source rect scaled up further than default. */
#define EB_ZOOM_IN_WIDTH  266
#define EB_ZOOM_IN_HEIGHT 149

static SDL_Window *window;
static SDL_Renderer *renderer;
static SDL_Texture *texture;

/* Overworld FOV/zoom cycle (R3) -- see platform_video_set_zoom() below and
 * AUX_ZOOM_TOGGLE in game_main.c. EB_ZOOM_OFF = default, matches every
 * scene's pre-existing EB_VIEWPORT_WIDTH x SNES_HEIGHT footprint. */
static EbZoomMode zoom_mode = EB_ZOOM_OFF;

/* Locked texture state (valid between begin_frame and end_frame) */
static pixel_t *locked_pixels;
static int locked_pitch;

/* Debug: dump the actual composited/scaled window output (not the raw
 * EB_VIEWPORT-sized framebuffer) to a BMP on the next present. Captures
 * whatever size the window currently is, so it also shows the smooth
 * (non-integer) scaling behavior, not just the game's internal render. */
static char pending_screenshot_path[256];

void platform_video_request_screenshot(const char *path) {
    snprintf(pending_screenshot_path, sizeof(pending_screenshot_path), "%s", path);
}

static void write_window_screenshot(const char *path) {
    int win_w, win_h;
    SDL_GetRendererOutputSize(renderer, &win_w, &win_h);

    SDL_Surface *surface = SDL_CreateRGBSurfaceWithFormat(0, win_w, win_h, 24, SDL_PIXELFORMAT_BGR24);
    if (!surface) return;
    if (SDL_RenderReadPixels(renderer, NULL, SDL_PIXELFORMAT_BGR24, surface->pixels, surface->pitch) == 0) {
        SDL_SaveBMP(surface, path);
    }
    SDL_FreeSurface(surface);
}

bool platform_video_init(void) {
    if (platform_headless)
        return true;  /* No window needed */

    /* Default: auto-fullscreen at the current desktop resolution (whatever
     * display/mode is active at launch -- a Steam Deck's panel, a TV over
     * HDMI, an ultrawide monitor, ...). SDL_WINDOW_FULLSCREEN_DESKTOP uses
     * the display's current mode rather than changing it (no mode-switch
     * flicker, plays nicely with compositors/Wayland). The width/height
     * passed here are only a fallback size if the window is ever taken out
     * of fullscreen. --windowed opts out, for development/debugging. */
    Uint32 window_flags = SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE;
    if (!platform_force_windowed)
        window_flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;

    window = SDL_CreateWindow(
        "EarthBound",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        EB_VIEWPORT_WIDTH * EB_WINDOW_SCALE, EB_VIEWPORT_HEIGHT * EB_WINDOW_SCALE,
        window_flags
    );
    if (!window) return false;

    renderer = SDL_CreateRenderer(window, -1,
        SDL_RENDERER_ACCELERATED);
    if (!renderer) return false;

    /* Enable VSync — use runtime call for reliable driver support.
     *
     * Windows exception: SDL2's VSync support there rides on whatever the
     * active D3D/GPU driver does with "application controlled" sync, which
     * is genuinely inconsistent across GPU vendors/driver versions/compositor
     * states -- when it doesn't engage, the *only* thing capping the frame
     * rate is platform_timer_frame_end()'s manual sleep-to-deadline pacer
     * (sdl2_timer.c), and every frame-counted animation in the game (the
     * battle HP/PP roll being the most visible one: hp_pp_roller() advances
     * it exactly once per game-logic frame, not per unit of wall-clock time)
     * plays back proportionally faster than intended if more frames run per
     * second than that pacer intends -- enough faster and a multi-second
     * roll reads as an instant snap. Rather than depend on Windows VSync
     * behaving consistently, skip it there entirely and let the manual
     * pacer (already made precise via timeBeginPeriod(1) in
     * platform_timer_init()) be the sole, fully-controlled source of
     * pacing. Linux/macOS VSync has shown no such inconsistency, so it
     * stays on there. */
#ifdef _WIN32
    SDL_RenderSetVSync(renderer, 0);
#else
    SDL_RenderSetVSync(renderer, 1);
#endif

    /* Scale the output to fill the window at any size (not just exact
     * integer multiples), preserving aspect ratio, via an explicitly
     * computed destination rect in platform_video_end_frame() -- NOT
     * SDL_RenderSetLogicalSize(). That call's automatic letterbox/pillarbox
     * centering turned out not to be reliable on this dev machine's SDL2
     * install (Homebrew's sdl2-compat, an SDL2-API shim on top of SDL3):
     * verified via a standalone repro that even the simplest case (texture
     * size == logical size, plain SDL_RenderCopy(tex, NULL, NULL)) renders
     * content anchored at the window's top-left instead of centered,
     * leaving all the letterbox slack at the bottom/right instead of split
     * evenly -- SDL_RenderLogicalToWindow() still *reports* the correct
     * centered transform via its query API, it just isn't what
     * SDL_RenderCopy actually draws. Computing and passing an explicit dst
     * rect sidesteps whatever that library-specific gap is entirely, and
     * doesn't depend on any particular SDL2 build/backend to get right.
     * Nearest-neighbor (scale quality "0") keeps pixel art crisp even at
     * non-integer scale factors. */
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");

    texture = SDL_CreateTexture(renderer,
        SDL_PIXELFORMAT_BGR565,
        SDL_TEXTUREACCESS_STREAMING,
        EB_VIEWPORT_WIDTH, EB_VIEWPORT_HEIGHT);
    if (!texture) return false;

    return true;
}

void platform_video_shutdown(void) {
    if (texture)  SDL_DestroyTexture(texture);
    if (renderer) SDL_DestroyRenderer(renderer);
    if (window)   SDL_DestroyWindow(window);
    texture = NULL;
    renderer = NULL;
    window = NULL;
}

void platform_video_begin_frame(void) {
    if (platform_headless) {
        locked_pixels = NULL;
        return;
    }
    void *pixels;
    int pitch;
    if (SDL_LockTexture(texture, NULL, &pixels, &pitch) == 0) {
        locked_pixels = (pixel_t *)pixels;
        locked_pitch = pitch;
    } else {
        locked_pixels = NULL;
    }
}

void platform_video_send_scanline(int y, const pixel_t *pixels) {
    if (!locked_pixels) return;
    uint8_t *dst = (uint8_t *)locked_pixels + y * locked_pitch;
    memcpy(dst, pixels, EB_VIEWPORT_WIDTH * sizeof(pixel_t));
}

pixel_t *platform_video_get_framebuffer(void) {
    return locked_pixels;
}

void platform_video_end_frame(void) {
    if (platform_headless)
        return;
    if (locked_pixels) {
        SDL_UnlockTexture(texture);
        locked_pixels = NULL;
    }
    SDL_RenderClear(renderer);

    int out_w, out_h;
    SDL_GetRendererOutputSize(renderer, &out_w, &out_h);

    /* content_w/content_h are the only things that change with zoom (see
     * platform_video_set_zoom() below).
     *
     * EB_ZOOM_OFF (default) stays a fixed 400x224 -- today's shipped
     * widescreen baseline, already close enough to 16:9 that it doesn't
     * need the adaptive treatment below (see EB_DEFAULT_WIDTH).
     *
     * EB_ZOOM_OUT adapts to whatever display it's actually running on,
     * instead of using a fixed EB_ZOOM_OUT_WIDTH x EB_ZOOM_OUT_HEIGHT crop:
     * a fixed crop's own aspect ratio (496x240 =~ 2.07:1) is narrower than
     * a 16:9 display (1.78:1), which meant the zoomed content was width-
     * constrained and always left an unwanted top/bottom letterbox even
     * though there was still safe zoom budget (in width) that could have
     * filled it. Instead, pick whichever axis the display's own aspect
     * ratio constrains and use that axis's full safe budget
     * (EB_ZOOM_OUT_WIDTH/HEIGHT -- see that comment for why those, not the
     * raw compiled max, are the safe ceiling), then size the OTHER axis to
     * exactly match the display's aspect ratio. Both results are always <=
     * EB_ZOOM_OUT_WIDTH/HEIGHT, so this can only ever crop *more*
     * conservatively than the already-verified-safe fixed crop, never less
     * -- it just stops wasting zoom budget on an axis the display doesn't
     * need filled, on any display shape (narrower than 2.07:1 like a 16:9
     * TV, wider like an ultrawide monitor, or matching it exactly).
     *
     * EB_ZOOM_IN is a fixed crop, no adaptation needed -- it already shares
     * the default footprint's exact aspect ratio (see EB_ZOOM_IN_WIDTH's
     * comment), so it looks just as good on any display the default
     * already does. */
    int content_w, content_h;
    switch (zoom_mode) {
    case EB_ZOOM_OUT: {
        double display_ar = (double)out_w / out_h;
        double max_zoom_ar = (double)EB_ZOOM_OUT_WIDTH / EB_ZOOM_OUT_HEIGHT;
        if (display_ar <= max_zoom_ar) {
            content_h = EB_ZOOM_OUT_HEIGHT;
            content_w = (int)(EB_ZOOM_OUT_HEIGHT * display_ar);
        } else {
            content_w = EB_ZOOM_OUT_WIDTH;
            content_h = (int)(EB_ZOOM_OUT_WIDTH / display_ar);
        }
        break;
    }
    case EB_ZOOM_IN:
        content_w = EB_ZOOM_IN_WIDTH;
        content_h = EB_ZOOM_IN_HEIGHT;
        break;
    case EB_ZOOM_OFF:
    default:
        content_w = EB_DEFAULT_WIDTH;
        content_h = SNES_HEIGHT;
        break;
    }
    SDL_Rect crop = {
        (EB_VIEWPORT_WIDTH  - content_w) / 2,
        (EB_VIEWPORT_HEIGHT - content_h) / 2,
        content_w, content_h
    };

    /* Explicitly compute the aspect-preserving, centered destination rect
     * rather than relying on SDL_RenderSetLogicalSize's automatic
     * letterbox/pillarbox centering -- see the long comment in
     * platform_video_init() for why. Since content_w/content_h are already
     * chosen to match the display's aspect ratio when zoomed, this should
     * land on dst_w == out_w and dst_h == out_h (no bars) in that case --
     * still computed generically rather than assumed, so the default
     * (non-adaptive) case and any rounding slop are handled the same way. */
    double scale = (double)out_w / content_w;
    double scale_h = (double)out_h / content_h;
    if (scale_h < scale) scale = scale_h;
    int dst_w = (int)(content_w * scale);
    int dst_h = (int)(content_h * scale);
    SDL_Rect dst = { (out_w - dst_w) / 2, (out_h - dst_h) / 2, dst_w, dst_h };

    SDL_RenderCopy(renderer, texture, &crop, &dst);

    if (pending_screenshot_path[0]) {
        write_window_screenshot(pending_screenshot_path);
        pending_screenshot_path[0] = '\0';
    }

    SDL_RenderPresent(renderer);
}

void platform_video_set_zoom(EbZoomMode mode) {
    /* platform_video_end_frame() reads zoom_mode fresh every frame to
     * compute the crop/dst rects (see the manual-centering comment in
     * platform_video_init()), so there's nothing else to update here. */
    zoom_mode = mode;
}

void platform_video_set_vsync(bool enabled) {
    /* Windows never has VSync engaged in the first place (see the _WIN32
     * branch in platform_video_init()) -- the fast-forward toggle
     * (game_main.c) calling this to turn VSync back on when fast-forward
     * ends would undo that deliberately, so it's a no-op there. The manual
     * frame pacer already handles both normal and fast-forward speeds
     * (EB_FAST_FORWARD_MULTIPLIER, sdl2_timer.c) without VSync's help. */
#ifndef _WIN32
    if (renderer)
        SDL_RenderSetVSync(renderer, enabled ? 1 : 0);
#else
    (void)enabled;
#endif
}
