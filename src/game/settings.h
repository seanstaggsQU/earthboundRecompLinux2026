#ifndef GAME_SETTINGS_H
#define GAME_SETTINGS_H

#include "core/types.h"

/*
 * Engine settings, this port's own preferences, not part of the original
 * ROM. Backed by platform_settings_read/write (platform.h); see settings.c
 * for the on-disk format. Reachable in-game via the command menu's
 * "Config" item (build_command_menu()/mode_step_settings_menu(), text.c).
 */

/* Sprint speed levels (see position_buffer.c's adjust_position(), which
 * reads engine_sprint_speed each frame). Off disables the sprint button
 * entirely (holding it has no effect); Medium/Stinky are the +50%/+100%
 * multipliers requested for the sprint feature added this session. */
typedef enum {
    SPRINT_SPEED_OFF    = 0,
    SPRINT_SPEED_MEDIUM = 1,
    SPRINT_SPEED_STINKY = 2,
    SPRINT_SPEED_COUNT,
} SprintSpeedSetting;

extern uint8_t engine_sprint_speed; /* current value, one of SprintSpeedSetting */

/* High Quality Audio: On lets an MSU1 pack (if one is configured via
 * --msu-dir/--msu-name, see port/unix/main.c) replace the SPC700 chiptune
 * per-track where the pack has a matching file; Off always uses the
 * original SPC700 music, even with a pack configured. See change_music()
 * in audio.c, which reads this directly. Purely a playback preference,
 * independent of whether a pack is actually present, harmless either way
 * when none is. */
typedef enum {
    HQ_AUDIO_OFF = 0,
    HQ_AUDIO_ON  = 1,
    HQ_AUDIO_COUNT,
} HqAudioSetting;

extern uint8_t engine_hq_audio; /* current value, one of HqAudioSetting */

/* "Alt Controls": swaps the A<->B and X<->Y face-button *mapping outputs*.
 * SDL's SDL_CONTROLLER_BUTTON_* enum names buttons by their position on an
 * Xbox-style pad (A=south/bottom, B=east/right, X=west/left, Y=north/top),
 * regardless of what's physically printed on the button, see
 * controller_button_to_pad() in port/unix/platform/sdl2_input.c, which maps
 * by that same Xbox-style position (matching the README's South/East/
 * North/West phrasing). On a Nintendo-style pad (Switch Pro, SNES,
 * GameCube-adapter, ...) the labels are rotated relative to Xbox: the
 * physically-labeled "A" sits at the east/right position (Xbox's "B"), "B"
 * at south (Xbox's "A"), and X/Y are likewise swapped -- so a Nintendo
 * player pressing what they see as "A" (expecting confirm, per SNES/Switch
 * convention) gets Xbox-B's mapped function instead. Alt Controls ON swaps
 * the two mapping outputs (not the physical positions, which SDL already
 * reports consistently) so a Nintendo-labeled pad's buttons produce the
 * function a Nintendo player expects. Controller only, keyboard bindings
 * are arbitrary key choices with no "which convention" question to
 * resolve. */
typedef enum {
    ALT_CONTROLS_OFF = 0,
    ALT_CONTROLS_ON  = 1,
    ALT_CONTROLS_COUNT,
} AltControlsSetting;

extern uint8_t engine_alt_controls; /* current value, one of AltControlsSetting */

/* Visual FX toggles: independent screen-space effects, each applied in
 * platform_video_end_frame() (sdl2_video.c) after the PPU finishes
 * rendering, entirely on the SDL2/desktop side, desktop-only, there's no
 * equivalent hook on the embedded ports (Pico/Game&Watch), which just
 * never read any of these.
 *
 * Formerly a single 3-way "Alternative Visuals" mode (Off/Classic/Modern,
 * each bundling several effects plus an aspect-ratio/zoom lock together) --
 * split into independent toggles per feedback that the bundling made it
 * impossible to, say, want scanlines without also losing zoom, or want
 * antialiasing without tilt-shift. See the SETTINGS_VERSION 4->5 bump
 * comment in settings.c for how an old Off/Classic/Modern preference maps
 * onto these on first load. Each toggle:
 *
 *   - Scanlines: alternating-row darkening (apply_scanlines(), sdl2_video.c)
 *     for a CRT-ish look. A pure overlay -- unlike the old Classic mode,
 *     turning this on does NOT force 4:3 or disable zoom; combine it with
 *     whatever Aspect Ratio/FOV you want.
 *   - Antialiasing: a Scale2x/EPX 2x upscale (replication-only, see
 *     apply_aa_upscale()'s doc comment, sdl2_video.c, for why an
 *     interpolating scaler like hqx/xBRZ risks smearing this game's
 *     dithered shading, and for the Eagle variant that was tried and
 *     reverted after it made text noticeably less readable).
 *   - Tilt Shift: Depth of Field, a very subtle blur that increases toward
 *     the top/bottom screen edges and stays sharp in a central band,
 *     approximating the "miniature diorama" look of HD-2D-style games.
 *     There's no real depth buffer here (the PPU has no concept of scene
 *     depth beyond BG layer priority), this is a screen-space fake keyed
 *     purely on row position. Fully suppressed (regardless of this
 *     setting) during battle, the Town Map, and any time a text/menu
 *     window is open, see platform_video_set_dof_suppressed()/
 *     host_process_frame().
 *   - Wide FOV: defaults gameplay to the zoomed-out FOV (EB_ZOOM_OUT,
 *     src/platform/platform.h's EbZoomMode) the moment the player leaves
 *     title/file-select, and changes what the R3 zoom-cycle toggles
 *     between (Wide<->Zoom In when this is on, Off<->Zoom In when it's
 *     off) -- see src/game_main.c's R3/zoom-cycle handling. Also
 *     suppresses the original ROM's per-encounter battle letterbox bars
 *     (battle_ui.c), which read as an unwanted bar rather than a
 *     deliberate cinematic effect once the wider FOV is already in play.
 *   - Color Grading: a mild global contrast/saturation/warmth adjustment
 *     applied to the whole frame as the last post-process step, after
 *     DoF/scanlines are already applied.
 *
 * A third post-process, Light Shafts (a "god ray" ray-march effect), and
 * before that a fourth, Bloom, both existed earlier in this feature's
 * development and were removed outright rather than folded into any
 * toggle -- Bloom per feedback that it looked washed out, Light Shafts
 * after repeated attempts to bring its per-frame cost down still left it
 * as the single largest expense in this whole pipeline and a reported
 * framerate dip. All toggles default Off, purely a visual preference,
 * shouldn't change any existing screenshot/recording's look without the
 * player opting in. */
typedef enum {
    FX_TOGGLE_OFF = 0,
    FX_TOGGLE_ON  = 1,
    FX_TOGGLE_COUNT,
} FxToggleSetting;

extern uint8_t engine_fx_scanlines;     /* current value, one of FxToggleSetting */
extern uint8_t engine_fx_antialiasing;  /* current value, one of FxToggleSetting */
extern uint8_t engine_fx_tiltshift;     /* current value, one of FxToggleSetting */
extern uint8_t engine_fx_wide_fov;      /* current value, one of FxToggleSetting */
extern uint8_t engine_fx_color_grading; /* current value, one of FxToggleSetting */

/* Aspect Ratio: decoupled from Scanlines (formerly Classic mode forced
 * this to 4:3 as a package deal, see the FxToggleSetting comment above).
 * 16:9 is today's existing widescreen baseline (EB_DEFAULT_WIDTH, unaffected
 * by this setting historically); 4:3 crops to the true original SNES
 * resolution (SNES_WIDTH x SNES_HEIGHT, sdl2_video.c). Only affects the
 * EB_ZOOM_OFF baseline -- Zoom In/Wide FOV still layer their own crop on
 * top exactly as before, whichever aspect is selected. */
typedef enum {
    ASPECT_RATIO_16_9 = 0,
    ASPECT_RATIO_4_3  = 1,
    ASPECT_RATIO_COUNT,
} AspectRatioSetting;

extern uint8_t engine_aspect_ratio; /* current value, one of AspectRatioSetting */

/* Logging: On redirects stdout/stderr to a log file (see
 * platform_log_set_enabled(), platform.h) so a tester who hits a bug can
 * enable it from the Config menu, reproduce the problem, and share the
 * resulting file, without needing to launch from a terminal or know about
 * the pre-existing "--log-file" command-line flag (port/unix/main.c) at
 * all. Off by default: the redirect has no runtime cost either way, but
 * defaulting it off avoids surprising a player who goes looking for
 * console output during normal play (e.g. via a bundled terminal). Applied
 * both on live toggle (text.c's mode_step_settings_menu()) and at startup
 * in main() if a previous session left it on, see settings_load()'s call
 * site. */
typedef enum {
    LOGGING_OFF = 0,
    LOGGING_ON  = 1,
    LOGGING_COUNT,
} LoggingSetting;

extern uint8_t engine_logging; /* current value, one of LoggingSetting */

/* Call once at startup (after platform_save_init(), before the game loop).
 * Loads settings.dat if present and valid; otherwise leaves/sets defaults. */
void settings_load(void);

/* Call after changing any engine_* setting to persist it immediately --
 * settings are small and changed rarely (a menu confirm press), so there's
 * no batching/dirty-flag machinery, just write-through. */
void settings_save(void);

#endif
