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
/* Bumped 1 -> 2 when hq_audio took over what used to be a reserved byte.
 * Reusing a reserved byte for a real field seemed backward-compatible at
 * first (same sizeof(blob), old files still pass the size check), but it
 * isn't: a version-1 file's reserved byte was always zeroed on write, so
 * there's no way to tell "this file predates HQ Audio" apart from "a user
 * explicitly chose Off" -- they're bit-for-bit identical, which meant a
 * pre-existing file could never default to HQ_AUDIO_ON's intended default,
 * only ever load as Off. Bumping the version makes any file written before
 * this fix fail the check below and fall through to today's real defaults
 * (once); every file written by this version on forward round-trips an
 * explicit Off correctly, because it's no longer ambiguous with "unset". */
#define SETTINGS_VERSION 2

typedef struct {
    uint32_t magic;
    uint8_t  version;
    uint8_t  sprint_speed;
    uint8_t  hq_audio;
    /* alt_controls took over what used to be the last reserved byte. Safe
     * to do without another version bump (unlike hq_audio's -- see the
     * SETTINGS_VERSION comment above): its correct default (OFF, i.e.
     * standard/Xbox-style mapping) is the same value an old file's zeroed
     * reserved byte already has, so there's no "predates this setting" vs.
     * "explicitly chose Off" ambiguity to resolve -- both read as OFF,
     * which is right either way. */
    uint8_t  alt_controls;
} EngineSettingsBlob;

uint8_t engine_sprint_speed = SPRINT_SPEED_MEDIUM; /* default until settings_load() runs */
uint8_t engine_hq_audio = HQ_AUDIO_ON;             /* default until settings_load() runs */
uint8_t engine_alt_controls = ALT_CONTROLS_OFF;    /* default until settings_load() runs */

void settings_load(void) {
    EngineSettingsBlob blob;
    memset(&blob, 0, sizeof(blob));

    if (platform_settings_read(&blob, sizeof(blob)) == sizeof(blob) &&
        blob.magic == SETTINGS_MAGIC && blob.version == SETTINGS_VERSION &&
        blob.sprint_speed < SPRINT_SPEED_COUNT) {
        engine_sprint_speed = blob.sprint_speed;
        engine_hq_audio = (blob.hq_audio < HQ_AUDIO_COUNT) ? blob.hq_audio : HQ_AUDIO_ON;
        engine_alt_controls = (blob.alt_controls < ALT_CONTROLS_COUNT) ? blob.alt_controls : ALT_CONTROLS_OFF;
    } else {
        /* No settings file, or unreadable/foreign/stale -- fall back to
         * defaults rather than leaving partially-read garbage in place. */
        engine_sprint_speed = SPRINT_SPEED_MEDIUM;
        engine_hq_audio = HQ_AUDIO_ON;
        engine_alt_controls = ALT_CONTROLS_OFF;
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
    platform_settings_write(&blob, sizeof(blob));
}
