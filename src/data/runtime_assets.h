#ifndef DATA_RUNTIME_ASSETS_H
#define DATA_RUNTIME_ASSETS_H

/* Runtime asset loader: mmaps an assets.pak file (see docs/assets.md) and
 * populates embedded_assets[] / asset_family_*[] from it, so every existing
 * ASSET_DATA(id) / ASSET_SIZE(id) call site works unmodified.
 *
 * Only compiled in when the build defines EB_RUNTIME_ASSETS (see the
 * EB_RUNTIME_ASSETS CMake option in src/CMakeLists.txt). The default build
 * keeps embedding assets at compile time via INCBIN, exactly as before. */

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    EB_ASSETS_OK = 0,
    EB_ASSETS_MISSING,          /* file not found / couldn't be opened */
    EB_ASSETS_BAD_MAGIC,        /* not an assets.pak (or truncated header) */
    EB_ASSETS_VERSION_MISMATCH, /* pak format version doesn't match this build */
    EB_ASSETS_LAYOUT_MISMATCH,  /* pak was built from a different asset layout */
    EB_ASSETS_COUNT_MISMATCH,   /* asset_count doesn't match ASSET_COUNT */
    EB_ASSETS_IO_ERROR,         /* stat/mmap failed, or file too small for its own index table */
} EbAssetLoadResult;

/* Loads (mmaps) assets.pak at `path` and populates embedded_assets[] and
 * every asset_family_*[] array. Must be called once at startup, before any
 * ASSET_DATA/ASSET_SIZE use. On any failure, no globals are modified and
 * eb_runtime_assets_ready() remains false. Calling this while a pack is
 * already loaded first unloads the previous one. */
EbAssetLoadResult eb_runtime_assets_load(const char *path);

/* True if a valid pack is currently loaded. */
bool eb_runtime_assets_ready(void);

/* Unmaps the current pack and clears embedded_assets[]/family arrays back
 * to all-NULL entries. Only needed for clean shutdown / tests, the OS
 * reclaims the mapping on process exit regardless. */
void eb_runtime_assets_unload(void);

/* Writes the platform-conventional assets.pak path into buf (XDG data dir
 * on Linux/macOS, %APPDATA% on Windows, both under "EarthBoundRecomp/").
 * Shared by the CLI --assets default here and by a future launcher, so
 * "where does the pak live" is defined exactly once. Returns false (buf
 * untouched) if the platform directory can't be resolved, e.g. HOME/
 * APPDATA unset, or buf_size is too small. */
bool eb_runtime_assets_default_path(char *buf, size_t buf_size);

#endif /* DATA_RUNTIME_ASSETS_H */
