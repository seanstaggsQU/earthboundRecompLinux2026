/* Access to an optionally-embedded copy of the ebtools-setup binary -- see
 * generated/embedded_helper.c.in and CMakeLists.txt's
 * EB_EMBED_SETUP_HELPER_PATH option for how (and when) it actually gets
 * baked in. A plain dev build has nothing embedded (PyInstaller can't run
 * as part of the normal CMake build, so this stays opt-in, wired up only
 * by the release packaging process); this header/its implementation are
 * always compiled either way, so callers don't need their own #ifdef. */
#ifndef EMBEDDED_HELPER_H
#define EMBEDDED_HELPER_H

#include <stddef.h>

/* Returns a pointer to the embedded ebtools-setup binary's bytes and sets
 * *out_size to its length, or returns NULL (with *out_size set to 0) if
 * this build has nothing embedded. */
const unsigned char *eb_embedded_setup_helper_data(size_t *out_size);

#endif /* EMBEDDED_HELPER_H */
