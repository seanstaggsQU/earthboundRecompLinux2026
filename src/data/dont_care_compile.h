/*
 * ROM-only build of the "Don't Care" naming-screen name pool
 * (US/data/dont_care_names.bin, ASSET_US_DATA_DONT_CARE_NAMES_BIN) --
 * see src/data/dont_care_compile.c for why this isn't a raw ROM byte
 * range and text_compile.h for the sibling dialogue.bin case this
 * mirrors.
 */
#ifndef DONT_CARE_COMPILE_H
#define DONT_CARE_COMPILE_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Builds the full 294-byte (7 categories x 7 entries x 6 bytes) blob into
 * a freshly malloc'd buffer (*out_buf, caller frees), matching
 * ebtools/parsers/simple_tables.py's pack_dont_care_names() byte-for-byte.
 * Never actually fails (no ROM data or relocation involved, unlike the
 * dialogue.bin case) but returns bool/out_size for a consistent calling
 * convention with that sibling. */
bool dont_care_names_build_blob(uint8_t **out_buf, size_t *out_size);

#endif /* DONT_CARE_COMPILE_H */
