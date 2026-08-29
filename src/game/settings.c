/*
 * Engine settings persistence, this port's own addition, not a ROM port.
 * See settings.h. Storage is a tiny versioned blob via platform_settings_*
 * (platform.h), deliberately separate from the SRAM save-file format
 * (platform_save_*) since these are engine/device preferences, not game
 * save data, they should survive independent of which save slot is
 * active, and never touch the byte-for-byte SRAM mirror.
 */
#include "game/settings.h"
#include "platform/platform.h"
#include <string.h>

/* 'EBST', sentinel so a garbage/foreign settings.dat is rejected instead
 * of silently misinterpreted (same defensive intent as assets.pak's magic,
 * just far simpler since this blob is tiny and local-only). */
#define SETTINGS_MAGIC   0x54534245u

/* Bumped 1 -> 2 when hq_audio took over what used to be a reserved byte
 * (a version-1 file's zeroed reserved byte was indistinguishable from an
 * explicit Off, which broke HQ_AUDIO_ON's intended default for pre-
 * existing files). Bumped 2 -> 3 when Bloom/Depth of Field/Light Shafts/
 * Color Grading, four separate bytes at this point in the layout --
 * were collapsed into the single experimental_visuals byte below: same
 * byte position, different meaning, which is exactly the "old file reads
 * as something it didn't mean" trap the 1->2 bump above already fixed
 * once. Bumped 3 -> 4 when experimental_visuals's Off/On toggle became the
 * 3-way alternative_visuals Off/Classic/Modern mode (same byte position,
 * new meaning/range, same trap as the 2->3 bump). Version 4 also added an
 * `auto_save` byte at the position now named `reserved0` below -- the Auto
 * Save feature itself was removed (it could silently save over a manually-
 * curated state, and during the Onett police-gauntlet cutscene
 * specifically, a mid-sequence auto-save could land the player in a state
 * the game had no way to resume).
 *
 * Bumped 4 -> 5 when the 3-way alternative_visuals mode was split into
 * five independent FxToggleSetting bytes plus a separate aspect_ratio byte
 * (settings.h's FxToggleSetting/AspectRatioSetting comment) -- unlike the
 * earlier bumps, THIS one actually changes sizeof(EngineSettingsBlob), so
 * a version-4 (or 3) file can't just be read into the current struct and
 * reinterpreted at the same offsets; see EngineSettingsBlobLegacy below
 * and the read logic in settings_load() that branches on how many bytes
 * actually came back. */
#define SETTINGS_VERSION 5

typedef struct {
    uint32_t magic;
    uint8_t  version;
    uint8_t  sprint_speed;
    uint8_t  hq_audio;
    uint8_t  alt_controls;
    uint8_t  logging;
    uint8_t  fx_scanlines;
    uint8_t  fx_antialiasing;
    uint8_t  fx_tiltshift;
    uint8_t  fx_wide_fov;
    uint8_t  fx_color_grading;
    uint8_t  aspect_ratio;
} EngineSettingsBlob;

/* Byte-identical to the version 3/4 on-disk layout (both versions happened
 * to share this exact layout -- version 3's `alternative_visuals` byte
 * held the old Off/On experimental_visuals toggle instead of the 3-way
 * mode, everything else the same, see blob.version's own disambiguation
 * below). Kept as a separate struct, rather than reusing fields inside
 * EngineSettingsBlob at different offsets, so a version-4 file's bytes
 * land in fields named for what they actually meant on disk. */
typedef struct {
    uint32_t magic;
    uint8_t  version;
    uint8_t  sprint_speed;
    uint8_t  hq_audio;
    uint8_t  alt_controls;
    uint8_t  alternative_visuals; /* v3: old experimental_visuals (0/1); v4: 3-way (0/1/2) */
    uint8_t  logging;
    uint8_t  reserved0;           /* was `auto_save` (version 4); feature removed */
} EngineSettingsBlobLegacy;

uint8_t engine_sprint_speed = SPRINT_SPEED_MEDIUM;             /* default until settings_load() runs */
uint8_t engine_hq_audio = HQ_AUDIO_ON;                         /* default until settings_load() runs */
uint8_t engine_alt_controls = ALT_CONTROLS_OFF;                /* default until settings_load() runs */
uint8_t engine_fx_scanlines = FX_TOGGLE_OFF;                   /* default until settings_load() runs */
uint8_t engine_fx_antialiasing = FX_TOGGLE_OFF;                /* default until settings_load() runs */
uint8_t engine_fx_tiltshift = FX_TOGGLE_OFF;                   /* default until settings_load() runs */
uint8_t engine_fx_wide_fov = FX_TOGGLE_OFF;                    /* default until settings_load() runs */
uint8_t engine_fx_color_grading = FX_TOGGLE_OFF;               /* default until settings_load() runs */
uint8_t engine_aspect_ratio = ASPECT_RATIO_16_9;                /* default until settings_load() runs */
uint8_t engine_logging = LOGGING_OFF;                          /* default until settings_load() runs */

/* Maps an old 3-way alternative_visuals value onto the new independent
 * toggles, preserving each old mode's net look as closely as the new
 * model allows -- see settings.h's FxToggleSetting comment for what each
 * toggle does. Old Off -> everything off, 16:9 (today's unaffected
 * baseline). Old Classic -> Scanlines + Color Grading on, 4:3 (its two
 * post-processes plus its forced aspect, now as independent settings
 * instead of a package deal). Old Modern -> Antialiasing + Tilt Shift +
 * Wide FOV + Color Grading on, 16:9. */
static void migrate_alternative_visuals(uint8_t old_value) {
    if (old_value == 1) { /* old ALT_VISUALS_CLASSIC */
        engine_fx_scanlines = FX_TOGGLE_ON;
        engine_fx_antialiasing = FX_TOGGLE_OFF;
        engine_fx_tiltshift = FX_TOGGLE_OFF;
        engine_fx_wide_fov = FX_TOGGLE_OFF;
        engine_fx_color_grading = FX_TOGGLE_ON;
        engine_aspect_ratio = ASPECT_RATIO_4_3;
    } else if (old_value == 2) { /* old ALT_VISUALS_MODERN */
        engine_fx_scanlines = FX_TOGGLE_OFF;
        engine_fx_antialiasing = FX_TOGGLE_ON;
        engine_fx_tiltshift = FX_TOGGLE_ON;
        engine_fx_wide_fov = FX_TOGGLE_ON;
        engine_fx_color_grading = FX_TOGGLE_ON;
        engine_aspect_ratio = ASPECT_RATIO_16_9;
    } else { /* old ALT_VISUALS_OFF (or an unrecognized value) */
        engine_fx_scanlines = FX_TOGGLE_OFF;
        engine_fx_antialiasing = FX_TOGGLE_OFF;
        engine_fx_tiltshift = FX_TOGGLE_OFF;
        engine_fx_wide_fov = FX_TOGGLE_OFF;
        engine_fx_color_grading = FX_TOGGLE_OFF;
        engine_aspect_ratio = ASPECT_RATIO_16_9;
    }
}

static void load_defaults(void) {
    engine_sprint_speed = SPRINT_SPEED_MEDIUM;
    engine_hq_audio = HQ_AUDIO_ON;
    engine_alt_controls = ALT_CONTROLS_OFF;
    engine_fx_scanlines = FX_TOGGLE_OFF;
    engine_fx_antialiasing = FX_TOGGLE_OFF;
    engine_fx_tiltshift = FX_TOGGLE_OFF;
    engine_fx_wide_fov = FX_TOGGLE_OFF;
    engine_fx_color_grading = FX_TOGGLE_OFF;
    engine_aspect_ratio = ASPECT_RATIO_16_9;
    engine_logging = LOGGING_OFF;
}

void settings_load(void) {
    /* Read into a buffer sized for the CURRENT (largest known) layout --
     * fread() (platform_settings_read()'s backing implementation) stops at
     * whatever's actually on disk, so an old, smaller version-3/4 file
     * comes back short rather than padded/garbage-filled; the returned
     * byte count is what tells the two layouts apart below, since
     * versions 3/4/5 don't all share one struct size the way 3 and 4 used
     * to (see the SETTINGS_VERSION comment above). */
    uint8_t raw[sizeof(EngineSettingsBlob)];
    memset(raw, 0, sizeof(raw));
    size_t n = platform_settings_read(raw, sizeof(raw));

    if (n == sizeof(EngineSettingsBlob)) {
        EngineSettingsBlob blob;
        memcpy(&blob, raw, sizeof(blob));
        if (blob.magic == SETTINGS_MAGIC && blob.version == SETTINGS_VERSION &&
            blob.sprint_speed < SPRINT_SPEED_COUNT) {
            engine_sprint_speed = blob.sprint_speed;
            engine_hq_audio = (blob.hq_audio < HQ_AUDIO_COUNT) ? blob.hq_audio : HQ_AUDIO_ON;
            engine_alt_controls = (blob.alt_controls < ALT_CONTROLS_COUNT) ? blob.alt_controls : ALT_CONTROLS_OFF;
            engine_logging = (blob.logging < LOGGING_COUNT) ? blob.logging : LOGGING_OFF;
            engine_fx_scanlines = (blob.fx_scanlines < FX_TOGGLE_COUNT) ? blob.fx_scanlines : FX_TOGGLE_OFF;
            engine_fx_antialiasing = (blob.fx_antialiasing < FX_TOGGLE_COUNT) ? blob.fx_antialiasing : FX_TOGGLE_OFF;
            engine_fx_tiltshift = (blob.fx_tiltshift < FX_TOGGLE_COUNT) ? blob.fx_tiltshift : FX_TOGGLE_OFF;
            engine_fx_wide_fov = (blob.fx_wide_fov < FX_TOGGLE_COUNT) ? blob.fx_wide_fov : FX_TOGGLE_OFF;
            engine_fx_color_grading = (blob.fx_color_grading < FX_TOGGLE_COUNT) ? blob.fx_color_grading : FX_TOGGLE_OFF;
            engine_aspect_ratio = (blob.aspect_ratio < ASPECT_RATIO_COUNT) ? blob.aspect_ratio : ASPECT_RATIO_16_9;
            return;
        }
        load_defaults();
        return;
    }

    if (n == sizeof(EngineSettingsBlobLegacy)) {
        EngineSettingsBlobLegacy blob;
        memcpy(&blob, raw, sizeof(blob));
        if (blob.magic == SETTINGS_MAGIC && (blob.version == 3 || blob.version == 4) &&
            blob.sprint_speed < SPRINT_SPEED_COUNT) {
            engine_sprint_speed = blob.sprint_speed;
            engine_hq_audio = (blob.hq_audio < HQ_AUDIO_COUNT) ? blob.hq_audio : HQ_AUDIO_ON;
            engine_alt_controls = (blob.alt_controls < ALT_CONTROLS_COUNT) ? blob.alt_controls : ALT_CONTROLS_OFF;
            engine_logging = (blob.logging < LOGGING_COUNT) ? blob.logging : LOGGING_OFF;
            if (blob.version == 4) {
                migrate_alternative_visuals(blob.alternative_visuals < 3 ? blob.alternative_visuals : 0);
            } else {
                /* version == 3: blob.alternative_visuals actually holds the
                 * old experimental_visuals byte at this same offset (0=Off,
                 * 1=On), map On to the closest new equivalent, old Modern. */
                migrate_alternative_visuals(blob.alternative_visuals == 1 ? 2 : 0);
            }
            return;
        }
        load_defaults();
        return;
    }

    /* No settings file, or unreadable/foreign/stale/some other size
     * entirely, fall back to defaults rather than leaving partially-read
     * garbage in place. */
    load_defaults();
}

void settings_save(void) {
    EngineSettingsBlob blob;
    memset(&blob, 0, sizeof(blob));
    blob.magic = SETTINGS_MAGIC;
    blob.version = SETTINGS_VERSION;
    blob.sprint_speed = engine_sprint_speed;
    blob.hq_audio = engine_hq_audio;
    blob.alt_controls = engine_alt_controls;
    blob.logging = engine_logging;
    blob.fx_scanlines = engine_fx_scanlines;
    blob.fx_antialiasing = engine_fx_antialiasing;
    blob.fx_tiltshift = engine_fx_tiltshift;
    blob.fx_wide_fov = engine_fx_wide_fov;
    blob.fx_color_grading = engine_fx_color_grading;
    blob.aspect_ratio = engine_aspect_ratio;
    platform_settings_write(&blob, sizeof(blob));
}
