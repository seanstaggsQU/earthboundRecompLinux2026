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
 * new meaning/range, same trap as the 2->3 bump). Unlike the earlier
 * bumps, this one keeps a narrow, explicit migration path in
 * settings_load() below (see the `blob.version == 3` branch) instead of
 * just falling back to defaults, since collapsing an existing On/Off
 * preference into "reset to Off" would silently drop something the player
 * had actively chosen; the mapping is simple enough (old On -> Modern) to
 * be worth preserving. Version 4 also added an `auto_save` byte at the
 * position now named `reserved0` below -- the Auto Save feature itself
 * was removed (it could silently save over a manually-curated state, and
 * during the Onett police-gauntlet cutscene specifically, a mid-sequence
 * auto-save could land the player in a state the game had no way to
 * resume). The byte position is kept as reserved rather than removed, so
 * an upgrader's existing version-4 settings.dat (with a real 0/1
 * auto_save value already written into it) still matches this struct's
 * size and version exactly, and every OTHER preference (sprint speed, HQ
 * audio, alt controls, visuals, logging) loads correctly instead of
 * silently resetting to defaults; the leftover byte is simply never
 * read. */
#define SETTINGS_VERSION 4

typedef struct {
    uint32_t magic;
    uint8_t  version;
    uint8_t  sprint_speed;
    uint8_t  hq_audio;
    uint8_t  alt_controls;
    uint8_t  alternative_visuals; /* was `experimental_visuals` (Off/On) through version 3 */
    uint8_t  logging;
    uint8_t  reserved0;           /* was `auto_save` (version 4); feature removed, see above */
} EngineSettingsBlob;

uint8_t engine_sprint_speed = SPRINT_SPEED_MEDIUM;             /* default until settings_load() runs */
uint8_t engine_hq_audio = HQ_AUDIO_ON;                         /* default until settings_load() runs */
uint8_t engine_alt_controls = ALT_CONTROLS_OFF;                /* default until settings_load() runs */
uint8_t engine_alternative_visuals = ALT_VISUALS_OFF;          /* default until settings_load() runs */
uint8_t engine_logging = LOGGING_OFF;                          /* default until settings_load() runs */

void settings_load(void) {
    EngineSettingsBlob blob;
    memset(&blob, 0, sizeof(blob));

    /* Accept either the current version or exactly the prior version (3),
     * so a pre-existing file's experimental_visuals On/Off preference can
     * be migrated below rather than discarded, see the SETTINGS_VERSION
     * comment above. Both versions currently happen to produce the same
     * sizeof(EngineSettingsBlob) (the reserved0 byte fills what was
     * trailing struct padding in the old layout), so the size check alone
     * can't distinguish them; blob.version does that instead. Any other
     * version (older still, or a future one this build doesn't know
     * about) falls through to defaults. */
    if (platform_settings_read(&blob, sizeof(blob)) == sizeof(blob) &&
        blob.magic == SETTINGS_MAGIC &&
        (blob.version == SETTINGS_VERSION || blob.version == 3) &&
        blob.sprint_speed < SPRINT_SPEED_COUNT) {
        engine_sprint_speed = blob.sprint_speed;
        engine_hq_audio = (blob.hq_audio < HQ_AUDIO_COUNT) ? blob.hq_audio : HQ_AUDIO_ON;
        engine_alt_controls = (blob.alt_controls < ALT_CONTROLS_COUNT) ? blob.alt_controls : ALT_CONTROLS_OFF;
        engine_logging = (blob.logging < LOGGING_COUNT) ? blob.logging : LOGGING_OFF;
        if (blob.version == SETTINGS_VERSION) {
            engine_alternative_visuals = (blob.alternative_visuals < ALT_VISUALS_COUNT)
                ? blob.alternative_visuals : ALT_VISUALS_OFF;
        } else {
            /* version == 3: blob.alternative_visuals actually holds the
             * old experimental_visuals byte at this same offset (0=Off,
             * 1=On), map On to the closest new equivalent, Modern. */
            engine_alternative_visuals = (blob.alternative_visuals == 1) ? ALT_VISUALS_MODERN : ALT_VISUALS_OFF;
        }
    } else {
        /* No settings file, or unreadable/foreign/stale, fall back to
         * defaults rather than leaving partially-read garbage in place. */
        engine_sprint_speed = SPRINT_SPEED_MEDIUM;
        engine_hq_audio = HQ_AUDIO_ON;
        engine_alt_controls = ALT_CONTROLS_OFF;
        engine_alternative_visuals = ALT_VISUALS_OFF;
        engine_logging = LOGGING_OFF;
    }
}

void settings_save(void) {
    EngineSettingsBlob blob;
    memset(&blob, 0, sizeof(blob));
    blob.magic = SETTINGS_MAGIC;
    blob.version = SETTINGS_VERSION;
    blob.sprint_speed = engine_sprint_speed;
    blob.hq_audio = engine_hq_audio;
    blob.alt_controls = engine_alt_controls;
    blob.alternative_visuals = engine_alternative_visuals;
    blob.logging = engine_logging;
    platform_settings_write(&blob, sizeof(blob));
}
