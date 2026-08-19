/*
 * Engine settings persistence -- this port's own addition, not a ROM port.
 * See settings.h. Storage is a tiny versioned blob via platform_settings_*
 * (platform.h), deliberately separate from the SRAM save-file format
 * (platform_save_*) since these are engine/device preferences, not game
 * save data -- they should survive independent of which save slot is
 * active, and never touch the byte-for-byte SRAM mirror.
 */
#include "game/settings.h"
#include "platform/platform.h"
#include <string.h>

/* 'EBST' -- sentinel so a garbage/foreign settings.dat is rejected instead
 * of silently misinterpreted (same defensive intent as assets.pak's magic,
 * just far simpler since this blob is tiny and local-only). */
#define SETTINGS_MAGIC   0x54534245u
/* Bumped 1 -> 2 when hq_audio took over what used to be a reserved byte
 * (a version-1 file's zeroed reserved byte was indistinguishable from an
 * explicit Off, which broke HQ_AUDIO_ON's intended default for pre-
 * existing files). Bumped 2 -> 3 when Bloom/Depth of Field/Light Shafts/
 * Color Grading -- four separate bytes at this point in the layout --
 * were collapsed into the single experimental_visuals byte below: same
 * byte position, different meaning, which is exactly the "old file reads
 * as something it didn't mean" trap the 1->2 bump above already fixed
 * once. Bumping again makes any file written before this collapse fail
 * the check and fall through to today's defaults, rather than silently
 * reinterpreting a stale "Bloom" byte as "Experimental Visuals". */
#define SETTINGS_VERSION 3

typedef struct {
    uint32_t magic;
    uint8_t  version;
    uint8_t  sprint_speed;
    uint8_t  hq_audio;
    uint8_t  alt_controls;
    uint8_t  experimental_visuals;
    uint8_t  logging;
} EngineSettingsBlob;

uint8_t engine_sprint_speed = SPRINT_SPEED_MEDIUM;             /* default until settings_load() runs */
uint8_t engine_hq_audio = HQ_AUDIO_ON;                         /* default until settings_load() runs */
uint8_t engine_alt_controls = ALT_CONTROLS_OFF;                /* default until settings_load() runs */
uint8_t engine_experimental_visuals = EXPERIMENTAL_VISUALS_OFF; /* default until settings_load() runs */
uint8_t engine_logging = LOGGING_OFF;                          /* default until settings_load() runs */

void settings_load(void) {
    EngineSettingsBlob blob;
    memset(&blob, 0, sizeof(blob));

    /* `logging` was appended after SETTINGS_VERSION 3 shipped -- no version
     * bump needed for it specifically: growing the struct means an old,
     * shorter on-disk blob now reads fewer bytes than sizeof(blob), which
     * already fails the == sizeof(blob) check below and falls through to
     * defaults, the same protection a version bump would buy. (Contrast
     * the 2->3 bump in the comment above SETTINGS_VERSION -- that case
     * reused an *existing* byte's position with a new meaning, which the
     * size check alone can't catch.) */
    if (platform_settings_read(&blob, sizeof(blob)) == sizeof(blob) &&
        blob.magic == SETTINGS_MAGIC && blob.version == SETTINGS_VERSION &&
        blob.sprint_speed < SPRINT_SPEED_COUNT) {
        engine_sprint_speed = blob.sprint_speed;
        engine_hq_audio = (blob.hq_audio < HQ_AUDIO_COUNT) ? blob.hq_audio : HQ_AUDIO_ON;
        engine_alt_controls = (blob.alt_controls < ALT_CONTROLS_COUNT) ? blob.alt_controls : ALT_CONTROLS_OFF;
        engine_experimental_visuals = (blob.experimental_visuals < EXPERIMENTAL_VISUALS_COUNT)
            ? blob.experimental_visuals : EXPERIMENTAL_VISUALS_OFF;
        engine_logging = (blob.logging < LOGGING_COUNT) ? blob.logging : LOGGING_OFF;
    } else {
        /* No settings file, or unreadable/foreign/stale -- fall back to
         * defaults rather than leaving partially-read garbage in place. */
        engine_sprint_speed = SPRINT_SPEED_MEDIUM;
        engine_hq_audio = HQ_AUDIO_ON;
        engine_alt_controls = ALT_CONTROLS_OFF;
        engine_experimental_visuals = EXPERIMENTAL_VISUALS_OFF;
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
    blob.experimental_visuals = engine_experimental_visuals;
    blob.logging = engine_logging;
    platform_settings_write(&blob, sizeof(blob));
}
