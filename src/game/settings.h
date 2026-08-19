#ifndef GAME_SETTINGS_H
#define GAME_SETTINGS_H

#include "core/types.h"

/*
 * Engine settings — this port's own preferences, not part of the original
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
 * independent of whether a pack is actually present -- harmless either way
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
 * regardless of what's physically printed on the button -- see
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
 * function a Nintendo player expects. Controller only -- keyboard bindings
 * are arbitrary key choices with no "which convention" question to
 * resolve. */
typedef enum {
    ALT_CONTROLS_OFF = 0,
    ALT_CONTROLS_ON  = 1,
    ALT_CONTROLS_COUNT,
} AltControlsSetting;

extern uint8_t engine_alt_controls; /* current value, one of AltControlsSetting */

/* Experimental Visuals: a single toggle covering three subtle screen-space
 * post-processes, all applied in platform_video_end_frame() (sdl2_video.c)
 * after the PPU finishes rendering, entirely on the SDL2/desktop side --
 * desktop-only, there's no equivalent hook on the embedded ports (Pico/
 * Game&Watch), which just never read this:
 *
 *   - Depth of Field / tilt-shift: a very subtle blur that increases toward
 *     the top/bottom screen edges and stays sharp in a central band,
 *     approximating the "miniature diorama" look of HD-2D-style games.
 *     There's no real depth buffer here (the PPU has no concept of scene
 *     depth beyond BG layer priority) -- this is a screen-space fake keyed
 *     purely on row position. Also fully suppressed (regardless of this
 *     setting) during battle, the Town Map, and any time a text/menu
 *     window is open -- see platform_video_set_dof_suppressed()/
 *     host_process_frame().
 *   - Light Shafts: subtle "god ray" streaks radiating from a fixed
 *     screen-space point (not a real scene light -- the overworld has no
 *     single light-source position to derive one from). An atmospheric
 *     flourish, not a lighting simulation.
 *   - Color Grading: a mild global contrast/saturation/warmth adjustment
 *     applied to the whole frame as the last post-process step, after
 *     Light Shafts is already blended in.
 *
 * Originally three separate settings (plus a fourth, Bloom, since
 * removed): combined into one after feedback that a 7-row Config screen
 * was more granularity than useful, and simply labeled "Experimental
 * Visuals" since all three are still being tuned. Off by default -- purely
 * a visual preference, shouldn't change any existing screenshot/
 * recording's look without the player opting in. */
typedef enum {
    EXPERIMENTAL_VISUALS_OFF = 0,
    EXPERIMENTAL_VISUALS_ON  = 1,
    EXPERIMENTAL_VISUALS_COUNT,
} ExperimentalVisualsSetting;

extern uint8_t engine_experimental_visuals; /* current value, one of ExperimentalVisualsSetting */

/* Logging: On redirects stdout/stderr to a log file (see
 * platform_log_set_enabled(), platform.h) so a tester who hits a bug can
 * enable it from the Config menu, reproduce the problem, and share the
 * resulting file -- without needing to launch from a terminal or know about
 * the pre-existing "--log-file" command-line flag (port/unix/main.c) at
 * all. Off by default: the redirect has no runtime cost either way, but
 * defaulting it off avoids surprising a player who goes looking for
 * console output during normal play (e.g. via a bundled terminal). Applied
 * both on live toggle (text.c's mode_step_settings_menu()) and at startup
 * in main() if a previous session left it on -- see settings_load()'s call
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
