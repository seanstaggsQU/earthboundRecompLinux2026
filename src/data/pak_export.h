#ifndef DATA_PAK_EXPORT_H
#define DATA_PAK_EXPORT_H

#include <stdbool.h>

/* Writes an EBPK-format assets.pak from THIS binary's own compiled-in
 * asset data, so a compile-time-embed build (EB_RUNTIME_ASSETS off) can
 * hand a runtime-mode build (EB_RUNTIME_ASSETS on) everything it needs,
 * with no ROM and no ebtools/Python involved -- the bytes are already
 * sitting right here.
 *
 * Only compiled into a build that does NOT define EB_RUNTIME_ASSETS (an
 * EB_RUNTIME_ASSETS build has nothing of its own to export -- its
 * embedded_assets[] is only populated after loading a pak, not before).
 * Used two ways:
 *   - the self-updater calls this automatically right before it swaps an
 *     old embedded-assets binary out for a new runtime-assets one, so an
 *     existing tester's update is fully silent, no ROM needed
 *   - `--export-pak PATH` on the command line, for anyone updating by
 *     hand (a fresh download instead of in-app "Check for Updates") */
bool pak_export_write(const char *out_path);

#endif /* DATA_PAK_EXPORT_H */
