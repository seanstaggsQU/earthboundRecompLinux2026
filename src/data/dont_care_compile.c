/*
 * ROM-only build of the "Don't Care" naming-screen name pool. Unlike
 * every other simple binary asset rom_extract.c hands out, this one
 * isn't a raw ROM byte range at all -- see rom_extract_table.c's
 * ASSET_US_DATA_DONT_CARE_NAMES_BIN = {0, 0} gap. It's this port's own
 * custom overlay (src/custom_assets/dont_care_names.json) on the vanilla
 * name pool, applied by the embedded-assets build's offline Python
 * pipeline (ebtools/parsers/simple_tables.py's pack_dont_care_names())
 * but, until now, never reproduced by the ROM-only path at all -- a
 * brand-new player supplying only a ROM would see the untouched vanilla
 * pool instead (reported live: an already-fixed custom name still showed
 * up in-game because the actual running build was the ROM-only variant,
 * which had no code path to apply the override at all, not a stale-cache
 * issue). Small and simple enough (294 bytes, no bytecode/relocation,
 * unlike dialogue.bin/text_compile.c) to just build directly here.
 *
 * dont_care_names_source[][] (runtime_generated/dont_care_names_source.c,
 * `ebtools generate dont-care-names-source`) carries the JSON's strings
 * through as plain ASCII C literals; ascii_to_eb_char() (text.c, the same
 * encoder the naming screen's own typed-name input uses) does the actual
 * EB-text encoding here, so this file needs no text_table of its own.
 */
#include "data/dont_care_compile.h"

/* Only compiled when EB_RUNTIME_ASSETS is defined -- see text_compile.c's
 * matching guard for why (this file includes a runtime_generated header,
 * only on the include path in that build mode). */
#ifdef EB_RUNTIME_ASSETS

#include "dont_care_names_source.h"
#include "game/text.h"
#include <stdlib.h>
#include <string.h>

#define DONT_CARE_ENTRY_SIZE 6

/* Mirrors simple_tables.py's DONT_CARE_CATEGORIES max_usable_length list
 * (ness/paula/jeff/poo = 5, pet/food/thing = 6) purely for a debug-build
 * sanity bound -- the JSON itself was already validated at generation
 * time (generate_dont_care_names_source() raises on anything too long),
 * so this never actually fires for a checked-in dont_care_names.json,
 * it's just cheap insurance against a hand-edited generated file. */
static const uint8_t dont_care_max_len[DONT_CARE_CATEGORY_COUNT] = { 5, 5, 5, 5, 6, 6, 6 };

bool dont_care_names_build_blob(uint8_t **out_buf, size_t *out_size) {
    size_t total = (size_t)DONT_CARE_CATEGORY_COUNT * DONT_CARE_PER_CATEGORY * DONT_CARE_ENTRY_SIZE;
    uint8_t *buf = malloc(total);
    if (!buf) return false;

    size_t pos = 0;
    for (int cat = 0; cat < DONT_CARE_CATEGORY_COUNT; cat++) {
        for (int entry = 0; entry < DONT_CARE_PER_CATEGORY; entry++) {
            const char *name = dont_care_names_source[cat][entry];
            size_t len = strlen(name);
            if (len > dont_care_max_len[cat]) len = dont_care_max_len[cat];

            size_t i;
            for (i = 0; i < len; i++)
                buf[pos + i] = ascii_to_eb_char(name[i]);
            for (; i < DONT_CARE_ENTRY_SIZE; i++)
                buf[pos + i] = 0x00;
            pos += DONT_CARE_ENTRY_SIZE;
        }
    }

    *out_buf = buf;
    *out_size = total;
    return true;
}

#endif /* EB_RUNTIME_ASSETS */
