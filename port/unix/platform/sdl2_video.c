#include "platform/platform.h"
#include "game/settings.h"
#include "core/log.h"
#include <SDL.h>
#include <math.h>
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
/* 2x-resolution texture for the Experimental Visuals Scale2x/EPX
 * antialiasing pass (see apply_aa_upscale()) -- fed via SDL_UpdateTexture
 * (not locked/streamed like `texture`), swapped in for the final blit
 * instead of `texture` only on frames where it ran. */
static SDL_Texture *aa_texture;

/* Overworld FOV/zoom cycle (R3) -- see platform_video_set_zoom() below and
 * AUX_ZOOM_TOGGLE in game_main.c. EB_ZOOM_OFF = default, matches every
 * scene's pre-existing EB_VIEWPORT_WIDTH x SNES_HEIGHT footprint. */
static EbZoomMode zoom_mode = EB_ZOOM_OFF;

/* Per-frame overrides set by platform_video_set_fx_suppressed()/
 * platform_video_set_dof_suppressed(); see platform.h. Read in
 * platform_video_end_frame(), same as zoom_mode. */
static bool fx_suppressed = false;
static bool dof_suppressed = false;

/* DoF's own faded-in/out intensity (0..1), separate from the hard
 * fx_suppressed/dof_suppressed booleans above. Those flip instantly (battle
 * entry, opening a window, Town Map, ...); reported as an "abrupt stop" --
 * the blur was fully present one frame and completely gone the next, a hard
 * cut rather than a taper, however smoothly the blend already ramps across
 * screen rows (see apply_dof()'s own doc comment for that, separate axis).
 * Eased every frame in platform_video_end_frame() toward 1.0 when DoF
 * should be showing, 0.0 when suppressed, at DOF_FADE_STEP per frame (a
 * ~20-frame/~0.33s linear ramp at 60fps) -- apply_dof() is now called
 * whenever this is above zero, not just when the hard gate is true, so the
 * fade-out tail actually renders for a few frames after suppression flips
 * on, and multiplies its per-row blend by this value. */
static float dof_intensity = 0.0f;
#define DOF_FADE_STEP (1.0f / 20.0f)

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

/* ---- Experimental Visuals post-processing (this port's own addition;
 * gated together by Config's single "Experimental Visuals" setting -- see
 * game/settings.h: Depth of Field, Color Grading). Two other effects
 * existed earlier in this feature's development and were removed outright
 * rather than folded into this toggle: Bloom (per feedback), and Light
 * Shafts (a "god ray" ray-march effect, removed after repeated attempts to
 * bring its per-frame cost down -- extract-bright/pre-blur running at full
 * resolution, then downsampling that, then trimming the ray-march sample
 * count -- still left it as the dominant cost in this whole pipeline;
 * reported as still causing a significant framerate dip after all of that,
 * so it's gone rather than continuing to chase it).
 *
 * Both remaining effects are applied directly to the locked texture buffer
 * in platform_video_end_frame(), after the PPU finishes writing all
 * EB_VIEWPORT_HEIGHT scanlines into it (see platform_video_send_scanline()
 * below) and before it's uploaded to the GPU. Entirely desktop-side: none
 * of this touches src/snes/ppu_render.c or any other shared game-lib code,
 * so the embedded ports (Pico/Game&Watch) are completely unaffected --
 * they simply never read engine_experimental_visuals.
 * EB_VIEWPORT_WIDTH x EB_VIEWPORT_HEIGHT is small (512x256 by default) --
 * a few million integer/float ops per frame for any one of these,
 * comfortably inside a 60fps desktop budget, before even accounting for
 * this being an opt-in setting most frames don't pay for at all. */

static pixel_t dof_src[EB_VIEWPORT_WIDTH * EB_VIEWPORT_HEIGHT];
/* apply_dof()'s horizontal-sum intermediate -- see that function's doc
 * comment. int16_t is plenty (max possible sum is (2*DOF_BLUR_RADIUS+1)
 * taps * 255 -- 1785 at the current radius of 3, nowhere near the
 * 32767 ceiling even if DOF_BLUR_RADIUS grows further later). */
static int16_t dof_hsum_r[EB_VIEWPORT_WIDTH * EB_VIEWPORT_HEIGHT];
static int16_t dof_hsum_g[EB_VIEWPORT_WIDTH * EB_VIEWPORT_HEIGHT];
static int16_t dof_hsum_b[EB_VIEWPORT_WIDTH * EB_VIEWPORT_HEIGHT];
static uint8_t dof_hcount[EB_VIEWPORT_WIDTH];
static bool    dof_needs_hsum[EB_VIEWPORT_HEIGHT];
static float   dof_blend_row[EB_VIEWPORT_HEIGHT];

/* SDL_PIXELFORMAT_BGR565: bit 15..11 = B(5), bit 10..5 = G(6), bit 4..0 =
 * R(5) -- see the SDL_CreateTexture() call in platform_video_init() below.
 * Bit-replication (val << n | val >> (n2-n)) expands each channel to a
 * full 0-255 range instead of leaving the low bits always zero.
 *
 * Deliberately NOT routed through core/types.h's pixel_to_rgb888()/
 * PIXEL_RGB(), even though they cover the same BGR565<->RGB888 conversion
 * this file always uses (this TU is desktop-only and the texture format is
 * fixed, so there's no EB_PIXEL_RGB565 case to also handle here) --
 * verified those helpers use round(val * 255 / max) instead of bit-
 * replication, which disagree by +-1 on roughly 15-20% of possible 5-bit/
 * 6-bit inputs (e.g. a 5-bit 3 unpacks to 24 here, 25 there). That's a
 * real, if tiny, per-pixel color shift, and this file's whole Experimental
 * Visuals effect stack has already been tuned twice against exactly this
 * math (DoF/Color Grading's constants above) -- switching the
 * unpack/pack math out from under that tuning as a "reuse the existing
 * helper" cleanup would silently redo the tuning instead. Kept separate on
 * purpose; don't merge these without re-tuning the effects against
 * whichever expansion wins. */
static inline void fx_unpack(pixel_t p, uint8_t *r, uint8_t *g, uint8_t *b) {
    uint8_t r5 = (uint8_t)(p & 0x1F);
    uint8_t g6 = (uint8_t)((p >> 5) & 0x3F);
    uint8_t b5 = (uint8_t)((p >> 11) & 0x1F);
    *r = (uint8_t)((r5 << 3) | (r5 >> 2));
    *g = (uint8_t)((g6 << 2) | (g6 >> 4));
    *b = (uint8_t)((b5 << 3) | (b5 >> 2));
}

static inline pixel_t fx_pack(uint8_t r, uint8_t g, uint8_t b) {
    uint16_t r5 = (uint16_t)(r >> 3);
    uint16_t g6 = (uint16_t)(g >> 2);
    uint16_t b5 = (uint16_t)(b >> 3);
    return (pixel_t)((b5 << 11) | (g6 << 5) | r5);
}

/* Generic float -> clamped-byte helper, shared by DoF's blend and Color
 * Grading's tone curve. */
static inline uint8_t fx_clamp_byte(float v) {
    if (v < 0.0f) return 0;
    if (v > 255.0f) return 255;
    return (uint8_t)(v + 0.5f);
}

/* ---- Depth of Field / tilt-shift: a very subtle blur that increases
 * toward the top/bottom screen edges and stays sharp in a central band --
 * see the ExperimentalVisualsSetting doc comment (settings.h) for why this
 * is a screen-space fake (row position only) rather than real depth-based
 * blur. platform_video_set_dof_suppressed() additionally turns this off
 * entirely during battle/Town Map/any open window, so it never actually
 * runs anywhere near dialogue anyway.
 *
 * Blend-based, not radius-based: an earlier version varied the *blur
 * kernel's radius* per row (0 inside the band, ramping up to
 * DOF_MAX_BLUR_RADIUS outside it) via `(int)(t * radius + 0.5)`, which
 * meant the very first row outside the sharp band jumped straight from
 * radius 0 (no blur at all) to radius 1 -- a hard, visible seam right at
 * the band edge, reported as "abrupt" in testing, not a taper. Fixed by
 * keeping the blur kernel's radius FIXED (DOF_BLUR_RADIUS) and instead
 * ramping a continuous BLEND amount (lerp between the original sharp
 * pixel and the fixed-radius blurred one) via a smoothstep curve, which
 * is continuous in both value AND slope -- no seam anywhere, including at
 * the band edge itself, where the eased curve's slope is already zero.
 * DOF_MAX_BLEND caps the blend well under 100% even at the very screen
 * edge, and DOF_FOCUS_BAND_FRACTION now covers more of the screen (only
 * the outer edges taper at all) -- both narrowing per feedback that the
 * effect needed to be smaller/more subtle overall, not just smoother. */
/* Compared directly against `dist` below, which is already normalized
 * 0..1 across the center-to-edge half of the screen -- so this value IS
 * the fraction of the height that stays sharp, no further halving.
 *
 * An earlier version of this code computed `band_half = <this> / 2.0`
 * with the constant set to 0.75 and a comment claiming "central 75% of
 * height stays fully sharp" -- the extra `/ 2.0` meant the ACTUAL sharp
 * band was half that, ~37.5%. Tried correcting the math to match the
 * comment (dropping the `/ 2.0`, keeping 0.75) and shipped it to a live
 * test: the effect nearly vanished -- confined to a 16-row strip at the
 * very screen edge, peak blend cut from ~0.49 to ~0.26, reported back as
 * "no tilt shift effect at all". Whatever 0.75-with-the-bug produced
 * (effectively 0.375) was the actually-tuned, actually-liked look, even
 * though the comment never matched it. Set directly to that effective
 * value instead of re-deriving it through a stale "75%" framing --
 * restores the exact shipped behavior, just without the self-
 * contradicting math. If this ever needs retuning, treat 0.375 as the
 * current baseline "how much of the height stays sharp", not 0.75. */
#define DOF_FOCUS_BAND_FRACTION 0.375
#define DOF_BLUR_RADIUS  3             /* fixed blur kernel radius -- only
                                        * the blend amount ramps, not this
                                        * (was 2 -- bumped for a moderate
                                        * strength increase after the
                                        * effect turned out to be barely
                                        * visible in practice at the old
                                        * radius/blend combo, even though
                                        * the band-size math was correct) */
#define DOF_MAX_BLEND    0.6f          /* even at the extreme edge, blend at
                                        * most 60% toward the blurred pixel
                                        * (was 0.8, reported as too intense
                                        * once the fade-in/out made the
                                        * effect easy to actually observe
                                        * end-to-end -- dropped by ~25%.
                                        * Before that: 0.55, before that
                                        * "up to 100%" -- 0.55 read as too
                                        * subtle to notice at all, 0.8
                                        * overcorrected once the fade made
                                        * it more noticeable in general) */

/* Separable rewrite (was a direct (2*DOF_BLUR_RADIUS+1)-square-per-pixel
 * box average, i.e. 25 taps/pixel back when DOF_BLUR_RADIUS was 2): the
 * blur's boundary clipping is rectangular and axis-independent -- how many
 * taps land in-bounds along X depends only on the column, and along Y only
 * on the row, never on both together -- so a horizontal sum pass followed
 * by a vertical sum pass, dividing once at the very end by (hcount[x] *
 * vcount(y)), produces the exact same integer sums (and therefore the
 * exact same truncated-division result) as the original single pass per
 * pixel; verified byte-identical against the original over 262M random
 * pixels at full resolution before this replaced it (at the radius in
 * effect then -- re-verified after DOF_BLUR_RADIUS later changed, same
 * technique applies at any radius since the separability argument doesn't
 * depend on the specific radius). The win: the horizontal pass only has to
 * run once per source row a blurred row's vertical window can reach
 * (tracked via dof_needs_hsum[]), not once per *blurred* row's full square
 * footprint -- (2*DOF_BLUR_RADIUS+1) taps/pixel each pass instead of
 * (2*DOF_BLUR_RADIUS+1)^2, for rows that need it. */
static void apply_dof(pixel_t *pixels, int pitch, float intensity) {
    int w = EB_VIEWPORT_WIDTH, h = EB_VIEWPORT_HEIGHT;
    int stride = pitch / (int)sizeof(pixel_t);
#ifdef EB_FX_PROFILE
    static double acc_copy, acc_setup, acc_hpass, acc_vpass;
    static int prof_frames3;
    uint64_t r0 = platform_timer_ticks(), r1;
#endif

    /* Snapshot the source once; every row blurs/blends by reading only
     * from this untouched copy, so an already-modified row never feeds
     * into another row's average. */
    for (int y = 0; y < h; y++)
        memcpy(&dof_src[y * w], pixels + (size_t)y * stride, w * sizeof(pixel_t));
#ifdef EB_FX_PROFILE
    r1 = platform_timer_ticks(); acc_copy += (double)(r1-r0)*1000.0/platform_timer_ticks_per_sec(); r0 = r1;
#endif

    double center = (h - 1) / 2.0;
    double half_h = h / 2.0;
    /* Threshold against `dist` directly -- see DOF_FOCUS_BAND_FRACTION's
     * doc comment above for why this must NOT be halved again. */
    double sharp_threshold = DOF_FOCUS_BAND_FRACTION;

    /* Per-row blend factor (0 = sharp band, untouched; >0 = taper toward
     * the blurred pixel) and which source rows a blurred row's vertical
     * window could reach -- computed once up front instead of inline, both
     * because the vertical pass below needs blend_row[y+ky] where y+ky
     * isn't necessarily itself a "blurred" row (rare edge case very near
     * the sharp/blur boundary) and because dof_needs_hsum[] has to be
     * fully known before the horizontal pass runs. */
    for (int y = 0; y < h; y++) {
        double dist = fabs(y - center) / half_h; /* 0 at center, 1 at edge */
        if (dist <= sharp_threshold) {
            dof_blend_row[y] = 0.0f; /* sharp band -- untouched */
        } else {
            double t = (dist - sharp_threshold) / (1.0 - sharp_threshold);
            if (t > 1.0) t = 1.0;
            t = t * t * (3.0 - 2.0 * t); /* smoothstep: eased in AND out --
                                           * this is what actually removes
                                           * the seam, not just the lerp. */
            dof_blend_row[y] = (float)(t * DOF_MAX_BLEND) * intensity;
        }
    }

    for (int x = 0; x < w; x++) {
        int lo = x - DOF_BLUR_RADIUS; if (lo < 0) lo = 0;
        int hi = x + DOF_BLUR_RADIUS; if (hi >= w) hi = w - 1;
        dof_hcount[x] = (uint8_t)(hi - lo + 1);
    }

    memset(dof_needs_hsum, 0, sizeof(dof_needs_hsum));
    for (int y = 0; y < h; y++) {
        if (dof_blend_row[y] < 0.01f) continue; /* negligible -- row stays untouched */
        int lo = y - DOF_BLUR_RADIUS; if (lo < 0) lo = 0;
        int hi = y + DOF_BLUR_RADIUS; if (hi >= h) hi = h - 1;
        for (int yy = lo; yy <= hi; yy++)
            dof_needs_hsum[yy] = true;
    }
#ifdef EB_FX_PROFILE
    r1 = platform_timer_ticks(); acc_setup += (double)(r1-r0)*1000.0/platform_timer_ticks_per_sec(); r0 = r1;
#endif

    /* Horizontal pass: per-row 5-tap sums (not yet divided -- see the doc
     * comment above for why the division has to wait until after both
     * passes) into dof_hsum_*, only for rows the vertical pass can reach. */
    for (int y = 0; y < h; y++) {
        if (!dof_needs_hsum[y]) continue;
        const pixel_t *src_row = &dof_src[y * w];
        for (int x = 0; x < w; x++) {
            int lo = x - DOF_BLUR_RADIUS; if (lo < 0) lo = 0;
            int hi = x + DOF_BLUR_RADIUS; if (hi >= w) hi = w - 1;
            int sum_r = 0, sum_g = 0, sum_b = 0;
            for (int xx = lo; xx <= hi; xx++) {
                uint8_t r, g, b;
                fx_unpack(src_row[xx], &r, &g, &b);
                sum_r += r; sum_g += g; sum_b += b;
            }
            int idx = y * w + x;
            dof_hsum_r[idx] = (int16_t)sum_r;
            dof_hsum_g[idx] = (int16_t)sum_g;
            dof_hsum_b[idx] = (int16_t)sum_b;
        }
    }
#ifdef EB_FX_PROFILE
    r1 = platform_timer_ticks(); acc_hpass += (double)(r1-r0)*1000.0/platform_timer_ticks_per_sec(); r0 = r1;
#endif

    /* Vertical pass + blend: sum the horizontal sums over each blurred
     * row's vertical window, divide once by the combined tap count, then
     * lerp toward the original sharp pixel same as before. */
    for (int y = 0; y < h; y++) {
        float blend = dof_blend_row[y];
        if (blend < 0.01f) continue;

        int ylo = y - DOF_BLUR_RADIUS; if (ylo < 0) ylo = 0;
        int yhi = y + DOF_BLUR_RADIUS; if (yhi >= h) yhi = h - 1;
        int vcount = yhi - ylo + 1;

        pixel_t *row = pixels + (size_t)y * stride;
        for (int x = 0; x < w; x++) {
            int sum_r = 0, sum_g = 0, sum_b = 0;
            for (int yy = ylo; yy <= yhi; yy++) {
                int idx = yy * w + x;
                sum_r += dof_hsum_r[idx];
                sum_g += dof_hsum_g[idx];
                sum_b += dof_hsum_b[idx];
            }
            /* One reciprocal + 3 multiplies instead of 3 integer divisions
             * -- this is the single most expensive part of the vertical
             * pass (measured; integer division is one of the pricier basic
             * ALU ops on most CPUs, and this runs 3x per touched pixel).
             * The +1e-4f nudge corrects for float32 not exactly
             * representing every count/sum ratio (e.g. 147/49 can compute
             * as 2.999997 instead of 3.0, truncating a step low) --
             * verified byte-identical against integer division for every
             * (count, sum) pair the shipped constants can actually produce
             * (count in [9,49], sum in [0, 7*255]) before shipping this. */
            int count = dof_hcount[x] * vcount;
            float inv_count = 1.0f / count;
            uint8_t blur_r = (uint8_t)(sum_r * inv_count + 1e-4f);
            uint8_t blur_g = (uint8_t)(sum_g * inv_count + 1e-4f);
            uint8_t blur_b = (uint8_t)(sum_b * inv_count + 1e-4f);

            uint8_t orig_r, orig_g, orig_b;
            fx_unpack(dof_src[y * w + x], &orig_r, &orig_g, &orig_b);

            float out_r = orig_r + ((int)blur_r - (int)orig_r) * blend;
            float out_g = orig_g + ((int)blur_g - (int)orig_g) * blend;
            float out_b = orig_b + ((int)blur_b - (int)orig_b) * blend;
            row[x] = fx_pack(fx_clamp_byte(out_r), fx_clamp_byte(out_g), fx_clamp_byte(out_b));
        }
    }
#ifdef EB_FX_PROFILE
    r1 = platform_timer_ticks(); acc_vpass += (double)(r1-r0)*1000.0/platform_timer_ticks_per_sec();
    if (++prof_frames3 >= 120) {
        LOG_WARN("dof_profile: copy=%.3fms setup=%.3fms hpass=%.3fms vpass=%.3fms (avg/frame over %d frames)\n",
                 acc_copy/prof_frames3, acc_setup/prof_frames3, acc_hpass/prof_frames3, acc_vpass/prof_frames3, prof_frames3);
        acc_copy = acc_setup = acc_hpass = acc_vpass = 0;
        prof_frames3 = 0;
    }
#endif
}

/* ---- Color Grading: a mild global contrast/saturation/warmth adjustment,
 * applied after DoF (Modern) or scanlines (Classic) so it grades the whole
 * post-blur/post-scanline composited look rather than just the base scene.
 * Two tunings share this one function rather than duplicating it:
 *   - Modern: the original Experimental Visuals constants. All four knobs
 *     dialed back once already after feedback that it looked too
 *     saturated, then further per "make it more subtle" -- the current
 *     values are meant to be barely perceptible on their own, only
 *     additive over a whole scene.
 *   - Classic: a lower-saturation, slightly warmer "phosphor" tuning for
 *     the CRT-style look -- old CRT/phosphor displays characteristically
 *     ran a touch warm and never reproduced fully saturated color, so this
 *     leans the opposite direction from Modern's saturation boost. */
typedef enum {
    GRADE_PROFILE_MODERN,
    GRADE_PROFILE_CLASSIC,
} GradeProfile;

static void apply_color_grade(pixel_t *pixels, int pitch, GradeProfile profile) {
    float contrast, saturation, warm_r, warm_b;
    if (profile == GRADE_PROFILE_CLASSIC) {
        contrast   = 1.02f;  /* barely more contrast */
        saturation = 0.94f;  /* CRT phosphor: noticeably less saturated than source */
        warm_r     = 1.015f; /* slightly warmer than Modern's push */
        warm_b     = 0.985f;
    } else {
        contrast   = 1.03f;  /* >1 = more contrast around mid-gray (was 1.06) */
        saturation = 1.015f; /* >1 = more saturated (was 1.08, then 1.03 --
                               * reported too saturated both times) */
        warm_r     = 1.01f;  /* slight warm push: a touch more red... (was 1.02) */
        warm_b     = 0.99f;  /* ...and a touch less blue (was 0.98) */
    }

    int w = EB_VIEWPORT_WIDTH, h = EB_VIEWPORT_HEIGHT;
    int stride = pitch / (int)sizeof(pixel_t);
    for (int y = 0; y < h; y++) {
        pixel_t *row = pixels + (size_t)y * stride;
        for (int x = 0; x < w; x++) {
            uint8_t r0, g0, b0;
            fx_unpack(row[x], &r0, &g0, &b0);
            float r = (r0 - 128.0f) * contrast + 128.0f;
            float g = (g0 - 128.0f) * contrast + 128.0f;
            float b = (b0 - 128.0f) * contrast + 128.0f;
            float luma = r * 0.299f + g * 0.587f + b * 0.114f;
            r = luma + (r - luma) * saturation;
            g = luma + (g - luma) * saturation;
            b = luma + (b - luma) * saturation;
            r *= warm_r;
            b *= warm_b;
            row[x] = fx_pack(fx_clamp_byte(r), fx_clamp_byte(g), fx_clamp_byte(b));
        }
    }
}

/* ---- CRT-style scanlines: darkens every other row of the native low-res
 * buffer by a fixed, subtle amount, before the nearest-neighbor upscale to
 * the display -- this is what makes the darkened rows read as visible
 * horizontal bands once magnified, the same trick pixel-art emulator CRT
 * filters use. Classic-only: deliberately not offered under Modern, which
 * keeps the AA-smoothed look scanline-free. Applied directly to
 * locked_pixels at EB_VIEWPORT_WIDTH x EB_VIEWPORT_HEIGHT like DoF/Color
 * Grading, even though Classic's final crop is only ever SNES_WIDTH x
 * SNES_HEIGHT (256x224) of that buffer -- the crop happens later in
 * platform_video_end_frame(), so darkening full rows here is simplest and
 * costs nothing extra (the cropped-out rows' work is wasted but this whole
 * pass is cheap). */
#define SCANLINE_DARKEN 0.85f /* multiply darkened rows' RGB by this (15% darker) */

static void apply_scanlines(pixel_t *pixels, int pitch) {
    int w = EB_VIEWPORT_WIDTH, h = EB_VIEWPORT_HEIGHT;
    int stride = pitch / (int)sizeof(pixel_t);
    for (int y = 1; y < h; y += 2) {
        pixel_t *row = pixels + (size_t)y * stride;
        for (int x = 0; x < w; x++) {
            uint8_t r, g, b;
            fx_unpack(row[x], &r, &g, &b);
            row[x] = fx_pack(fx_clamp_byte(r * SCANLINE_DARKEN),
                              fx_clamp_byte(g * SCANLINE_DARKEN),
                              fx_clamp_byte(b * SCANLINE_DARKEN));
        }
    }
}

/* ---- Anti-Aliasing: an Eagle 2x pixel-art upscale, folded into Modern
 * Alternative Visuals per request rather than a separate Config row. Runs
 * last, after DoF/Color Grading have already modified locked_pixels, so it
 * smooths the fully composited frame.
 *
 * Unlike DoF/Color Grading, this does NOT respect fx_suppressed/
 * dof_suppressed (title screen, file-select, battle, Town Map, open
 * windows): those suppressions exist because those effects are
 * stylistic choices that specific screens weren't art-directed for.
 * Antialiasing isn't a style choice -- smoothing jagged diagonal edges
 * looks equally correct everywhere, including the title screen, so it
 * simply runs whenever Modern is active, full stop.
 *
 * Eagle (not a blur, same family as the Scale2x/EPX this replaced): for
 * each source pixel, expands it to a 2x2 output block where each corner is
 * replaced by the diagonal neighbor in that direction only when BOTH
 * orthogonal neighbors adjacent to that corner AND the diagonal neighbor
 * itself all agree with each other -- e.g. the top-left output pixel
 * becomes the NW neighbor's color only if West, North, and NW all match;
 * otherwise it stays the center color. This is a strictly larger check
 * than Scale2x/EPX's (which only ever compares the 4 orthogonal neighbors
 * against each other, never looking at the diagonals at all), so Eagle
 * catches and smooths some diagonal staircase edges Scale2x leaves jagged
 * -- verified against the same synthetic staircase test pattern used to
 * verify Scale2x originally (finer steps than Scale2x's output on the same
 * input). Like Scale2x/EPX, Eagle only ever *replicates* an existing
 * source color into the output -- it never blends/interpolates a new
 * in-between color -- which is what keeps EarthBound's checkerboard-
 * dithered shading and flat outline colors intact instead of smearing
 * them into muddy gradients. Interpolating scalers (hqx, xBRZ) don't have
 * that property and risk exactly that kind of smearing on this game's
 * dithered art style; xBRZ specifically was considered (it has more
 * sophisticated edge/corner detection than either Scale2x or Eagle) and
 * deferred for that reason -- worth adding later as an explicit opt-in
 * second AA choice if the user wants to compare it visually, rather than
 * as the default.
 *
 * Produces a full 2x-resolution frame (aa_upscaled/aa_texture below), not
 * an in-place effect on the EB_VIEWPORT-sized buffer like DoF/Color Grading
 * -- platform_video_end_frame() swaps in aa_texture (with a correspondingly
 * 2x'd crop rect) for the final blit instead of the normal texture when
 * this ran, rather than writing back into locked_pixels.
 *
 * Tolerance-based neighbor comparison, not exact equality: reported (and
 * root-caused, back when this was Scale2x/EPX) as making text look
 * "wonky"/like it's toggling between pixels frame to frame. A
 * corner-replacement decision that's a hard function of exact pixel
 * equality is perfectly stable for genuinely static source pixels (same
 * input, same output, always), but this engine has legitimate, intentional
 * per-frame ambient palette animation (see update_map_palette_animation(),
 * overworld_palette.c -- used for water shimmer and similar SNES-era
 * cosmetic effects), which shifts affected pixels by a color step or two
 * every frame. Imperceptible on its own, but exact-equality comparison
 * turns that subtle wobble into a full binary flip of which corner gets
 * replaced -- and text sitting next to an animating background is exactly
 * where a stable neighbor relationship (background == background) can
 * intermittently stop holding, producing a visibly flickering edge each
 * time the animation ticks. aa_similar() treats two colors as equivalent
 * for this decision if they're within AA_COLOR_TOLERANCE per channel
 * (roughly one BGR565 quantization step), absorbing that kind of small
 * ambient wobble while still telling genuinely different colors (an actual
 * sprite/text edge) apart. Verified with a synthetic test before shipping
 * Scale2x: a background wobbling by one full quantization step between two
 * synthetic "frames" produced zero text-edge-shape disagreements with
 * tolerance, versus real disagreements without it -- still applies
 * unchanged to Eagle's corner checks, which use the same aa_similar(). */
#define AA_SCALE 2
#define AA_COLOR_TOLERANCE 10  /* max per-8-bit-channel delta to treat two
                                 * colors as the same for this decision */

static pixel_t aa_upscaled[EB_VIEWPORT_WIDTH * AA_SCALE * EB_VIEWPORT_HEIGHT * AA_SCALE];

static inline pixel_t aa_at(const pixel_t *src, int stride, int w, int h, int x, int y) {
    if (x < 0) x = 0;
    if (x >= w) x = w - 1;
    if (y < 0) y = 0;
    if (y >= h) y = h - 1;
    return src[y * stride + x];
}

static inline bool aa_similar(pixel_t a, pixel_t b) {
    if (a == b) return true;
    uint8_t ar, ag, ab, br, bg, bb;
    fx_unpack(a, &ar, &ag, &ab);
    fx_unpack(b, &br, &bg, &bb);
    int dr = ar - br; if (dr < 0) dr = -dr;
    int dg = ag - bg; if (dg < 0) dg = -dg;
    int db = ab - bb; if (db < 0) db = -db;
    return dr <= AA_COLOR_TOLERANCE && dg <= AA_COLOR_TOLERANCE && db <= AA_COLOR_TOLERANCE;
}

static void apply_aa_upscale(const pixel_t *src, int src_pitch, pixel_t *dst, int dst_stride) {
    int w = EB_VIEWPORT_WIDTH, h = EB_VIEWPORT_HEIGHT;
    int src_stride = src_pitch / (int)sizeof(pixel_t);

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            pixel_t e  = aa_at(src, src_stride, w, h, x,     y);     /* center */
            pixel_t n  = aa_at(src, src_stride, w, h, x,     y - 1); /* north */
            pixel_t s  = aa_at(src, src_stride, w, h, x,     y + 1); /* south */
            pixel_t we = aa_at(src, src_stride, w, h, x - 1, y);     /* west */
            pixel_t ea = aa_at(src, src_stride, w, h, x + 1, y);     /* east */
            pixel_t nw = aa_at(src, src_stride, w, h, x - 1, y - 1); /* north-west */
            pixel_t ne = aa_at(src, src_stride, w, h, x + 1, y - 1); /* north-east */
            pixel_t sw = aa_at(src, src_stride, w, h, x - 1, y + 1); /* south-west */
            pixel_t se = aa_at(src, src_stride, w, h, x + 1, y + 1); /* south-east */

            /* Eagle: each output corner takes its diagonal neighbor's color
             * only when that neighbor and both orthogonal neighbors
             * adjacent to it all agree with each other -- checked as 3
             * pairwise comparisons rather than relying on transitivity,
             * since aa_similar()'s tolerance-based comparison isn't
             * guaranteed transitive (a~b and b~c doesn't imply a~c near
             * the tolerance boundary) the way exact equality would be. */
            pixel_t p1 = (aa_similar(we, n) && aa_similar(n, nw) && aa_similar(we, nw)) ? nw : e;
            pixel_t p2 = (aa_similar(n, ea) && aa_similar(ea, ne) && aa_similar(n, ne)) ? ne : e;
            pixel_t p3 = (aa_similar(we, s) && aa_similar(s, sw) && aa_similar(we, sw)) ? sw : e;
            pixel_t p4 = (aa_similar(s, ea) && aa_similar(ea, se) && aa_similar(s, se)) ? se : e;

            int ox = x * AA_SCALE, oy = y * AA_SCALE;
            dst[oy * dst_stride + ox]           = p1;
            dst[oy * dst_stride + ox + 1]       = p2;
            dst[(oy + 1) * dst_stride + ox]     = p3;
            dst[(oy + 1) * dst_stride + ox + 1] = p4;
        }
    }
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

    aa_texture = SDL_CreateTexture(renderer,
        SDL_PIXELFORMAT_BGR565,
        SDL_TEXTUREACCESS_STREAMING,
        EB_VIEWPORT_WIDTH * AA_SCALE, EB_VIEWPORT_HEIGHT * AA_SCALE);
    if (!aa_texture) return false;

    return true;
}

void platform_video_shutdown(void) {
    if (texture)    SDL_DestroyTexture(texture);
    if (aa_texture) SDL_DestroyTexture(aa_texture);
    if (renderer)   SDL_DestroyRenderer(renderer);
    if (window)     SDL_DestroyWindow(window);
    texture = NULL;
    aa_texture = NULL;
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

    /* Alternative Visuals: Off/Classic/Modern (settings.h). Modern is the
     * old "Experimental Visuals: On" look (DoF/grade/AA); Classic is a new
     * true-4:3 CRT-ish look (scanlines/grade, no DoF/AA) -- see settings.h
     * for the full mode writeup. */
    uint8_t alt_mode = engine_alternative_visuals;
    bool want_modern = alt_mode == ALT_VISUALS_MODERN;
    bool want_classic = alt_mode == ALT_VISUALS_CLASSIC;
    /* Set true only if the AA upscale actually ran this frame (i.e.
     * locked_pixels was valid) -- guards against swapping to aa_texture
     * on a frame where SDL_LockTexture failed in platform_video_begin_frame()
     * and it would otherwise hold stale content from a previous frame. */
    bool aa_ran = false;

    if (locked_pixels) {
        /* Modern's DoF/Color Grading share one Config toggle
         * (engine_alternative_visuals == MODERN) but keep independent
         * suppression: DoF has its own extra battle/Town-Map/window-open
         * condition on top of the shared title/file-select one (see
         * platform_video_set_dof_suppressed()/host_process_frame()).
         * Classic's scanlines/grade run unconditionally whenever Classic
         * is active -- unlike Modern's DoF, they're not a "focus effect"
         * that needs hiding for menu readability, they're meant to be the
         * look of the whole game, consistently, everywhere. Fixed
         * pipeline order: DoF/scanlines modify the base scene first, Color
         * Grading runs on the result before antialiasing so it grades the
         * base scene, not the upscaled one (grading is resolution-
         * independent, so this order doesn't matter for correctness, just
         * consistency with "grade the composited scene" already
         * established). AA runs last and unconditionally when Modern is
         * active -- see apply_aa_upscale()'s doc comment for why it skips
         * fx_suppressed/dof_suppressed unlike DoF/Color Grading. */
        bool want_fx = want_modern && !fx_suppressed;
#ifdef EB_FX_PROFILE
        static double acc_dof, acc_grade, acc_aa;
        static int prof_frames;
        uint64_t p0, p1;
        p0 = platform_timer_ticks();
#endif
        /* Ease dof_intensity toward whether DoF should be showing this
         * frame (see its doc comment above) instead of gating apply_dof()
         * on the hard boolean directly -- that's what produced the
         * "abrupt stop" (reported live): fully present one frame, fully
         * gone the next the instant dof_suppressed flips (battle entry,
         * opening a window, Town Map, ...). Called every frame
         * unconditionally (not just when the target is true) so the
         * step is always exactly one DOF_FADE_STEP regardless of which
         * direction it's easing, and apply_dof() itself is only called
         * while there's still something to show. Naturally eases to 0 when
         * Classic or Off is active too (want_modern false), so switching
         * away from Modern mid-game fades DoF out instead of cutting it. */
        float dof_target = (want_modern && !dof_suppressed) ? 1.0f : 0.0f;
        if (dof_intensity < dof_target) {
            dof_intensity += DOF_FADE_STEP;
            if (dof_intensity > dof_target) dof_intensity = dof_target;
        } else if (dof_intensity > dof_target) {
            dof_intensity -= DOF_FADE_STEP;
            if (dof_intensity < dof_target) dof_intensity = dof_target;
        }
        if (dof_intensity > 0.0f)
            apply_dof(locked_pixels, locked_pitch, dof_intensity);
#ifdef EB_FX_PROFILE
        p1 = platform_timer_ticks(); acc_dof += (double)(p1-p0)*1000.0/platform_timer_ticks_per_sec(); p0 = p1;
#endif
        if (want_classic)
            apply_scanlines(locked_pixels, locked_pitch);
        if (want_fx)
            apply_color_grade(locked_pixels, locked_pitch, GRADE_PROFILE_MODERN);
        else if (want_classic)
            apply_color_grade(locked_pixels, locked_pitch, GRADE_PROFILE_CLASSIC);
#ifdef EB_FX_PROFILE
        p1 = platform_timer_ticks(); acc_grade += (double)(p1-p0)*1000.0/platform_timer_ticks_per_sec(); p0 = p1;
#endif
        if (want_modern) {
            apply_aa_upscale(locked_pixels, locked_pitch,
                              aa_upscaled, EB_VIEWPORT_WIDTH * AA_SCALE);
            aa_ran = true;
        }
#ifdef EB_FX_PROFILE
        p1 = platform_timer_ticks(); acc_aa += (double)(p1-p0)*1000.0/platform_timer_ticks_per_sec();
        if (++prof_frames >= 120) {
            LOG_WARN("fx_profile: dof=%.3fms grade=%.3fms aa=%.3fms (avg/frame over %d frames)\n",
                     acc_dof/prof_frames, acc_grade/prof_frames, acc_aa/prof_frames, prof_frames);
            acc_dof = acc_grade = acc_aa = 0;
            prof_frames = 0;
        }
#endif

        SDL_UnlockTexture(texture);
        locked_pixels = NULL;

        if (aa_ran) {
            SDL_UpdateTexture(aa_texture, NULL, aa_upscaled,
                               EB_VIEWPORT_WIDTH * AA_SCALE * (int)sizeof(pixel_t));
        }
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
    if (want_classic) {
        /* Classic: always true 4:3 (the original SNES resolution), zoom
         * ignored entirely -- defense in depth alongside the R3/zoom-cycle
         * gating in game_main.c, which already keeps zoom_mode at
         * EB_ZOOM_OFF while Classic is active; forcing it here too means
         * this crop stays correct even if that gate is ever bypassed
         * (e.g. a leftover zoom_mode from switching modes mid-frame). */
        content_w = SNES_WIDTH;
        content_h = SNES_HEIGHT;
    } else
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
    /* crop is expressed in whichever texture is about to be blitted:
     * aa_texture (AA_SCALE resolution) on a frame the Scale2x upscale ran,
     * `texture` (native EB_VIEWPORT resolution) otherwise -- same
     * proportions either way, just scaled up by AA_SCALE when sourcing
     * from the upscaled texture, so every zoom mode's crop math above
     * still applies unchanged. */
    int crop_scale = aa_ran ? AA_SCALE : 1;
    SDL_Rect crop = {
        ((EB_VIEWPORT_WIDTH  - content_w) / 2) * crop_scale,
        ((EB_VIEWPORT_HEIGHT - content_h) / 2) * crop_scale,
        content_w * crop_scale, content_h * crop_scale
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

    SDL_RenderCopy(renderer, aa_ran ? aa_texture : texture, &crop, &dst);

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

void platform_video_set_fx_suppressed(bool suppressed) {
    /* Read fresh every frame in platform_video_end_frame(), same as
     * zoom_mode just above -- nothing else to update here. */
    fx_suppressed = suppressed;
}

void platform_video_set_dof_suppressed(bool suppressed) {
    dof_suppressed = suppressed;
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
