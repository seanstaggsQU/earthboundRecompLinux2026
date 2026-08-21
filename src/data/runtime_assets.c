/* Runtime asset loader, see runtime_assets.h.
 *
 * Only compiled when EB_RUNTIME_ASSETS is defined (see the CMake option of
 * the same name in src/CMakeLists.txt). Uses POSIX mmap/open, matching the
 * only port that currently opts into this (port/unix); a future embedded
 * or Windows port adding EB_RUNTIME_ASSETS support will need its own
 * file-mapping backend here.
 *
 * Exception: eb_runtime_assets_default_path() below is always compiled,
 * regardless of EB_RUNTIME_ASSETS -- it's pure env-var string building, no
 * dependency on anything runtime-assets-specific, and the self-updater
 * needs it from an EB_RUNTIME_ASSETS=OFF (compile-time-embedded) build too,
 * to know where to export a pak for the EB_RUNTIME_ASSETS=ON build it's
 * about to swap itself out for (see pak_export.c and sdl2_updater.c). */
#include "runtime_assets.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool eb_runtime_assets_default_path(char *buf, size_t buf_size) {
#if defined(_WIN32)
    const char *appdata = getenv("APPDATA");
    if (appdata == NULL) {
        return false;
    }
    int n = snprintf(buf, buf_size, "%s\\EarthBoundRecomp\\assets.pak", appdata);
    return n > 0 && (size_t)n < buf_size;
#else
    char fallback[PATH_MAX];
    const char *data_home = getenv("XDG_DATA_HOME");
    if (data_home == NULL || data_home[0] == '\0') {
        const char *home = getenv("HOME");
        if (home == NULL) {
            return false;
        }
        int n = snprintf(fallback, sizeof(fallback), "%s/.local/share", home);
        if (n <= 0 || (size_t)n >= sizeof(fallback)) {
            return false;
        }
        data_home = fallback;
    }
    int n = snprintf(buf, buf_size, "%s/EarthBoundRecomp/assets.pak", data_home);
    return n > 0 && (size_t)n < buf_size;
#endif
}

#ifdef EB_RUNTIME_ASSETS

#include "asset_ids.h"
#include "asset_pack_layout.h"
#include "embedded_assets.h"

#include <fcntl.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

/* .pak binary format (see docs/assets.md). All multi-byte fields are
 * little-endian, decoded by hand below rather than memcpy'd as structs so
 * the layout doesn't depend on host struct packing/endianness. */
#define EB_PAK_MAGIC "EBPK"
#define EB_PAK_VERSION 1u
#define EB_PAK_HASH_SIZE 32u
#define EB_PAK_HEADER_SIZE (4u + 4u + 4u + EB_PAK_HASH_SIZE) /* 44 */
#define EB_PAK_INDEX_ENTRY_SIZE 8u                            /* offset:u32 + length:u32 */

static void *s_mapping = NULL;
static size_t s_mapping_size = 0;
static bool s_ready = false;
static unsigned int s_asset_lengths[ASSET_COUNT];

static uint32_t read_u32le(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void hash_to_hex(const unsigned char *hash, char out[EB_PAK_HASH_SIZE * 2 + 1]) {
    static const char digits[] = "0123456789abcdef";
    for (unsigned i = 0; i < EB_PAK_HASH_SIZE; i++) {
        out[i * 2] = digits[hash[i] >> 4];
        out[i * 2 + 1] = digits[hash[i] & 0xF];
    }
    out[EB_PAK_HASH_SIZE * 2] = '\0';
}

/* Resets every AssetEntry to "no data, zero size" and re-anchors size_ptr
 * at our own length table. size_ptr always points somewhere valid (even
 * pre-load / post-unload) so ASSET_SIZE() never dereferences NULL, only
 * ASSET_DATA() returning NULL signals "not loaded", same as a compile-time
 * gap entry. */
static void clear_globals(void) {
    for (int i = 0; i < ASSET_COUNT; i++) {
        embedded_assets[i].data = NULL;
        embedded_assets[i].size_ptr = &s_asset_lengths[i];
        s_asset_lengths[i] = 0;
    }
}

void eb_runtime_assets_unload(void) {
    if (s_mapping != NULL) {
        munmap(s_mapping, s_mapping_size);
        s_mapping = NULL;
        s_mapping_size = 0;
    }
    s_ready = false;
    clear_globals();
}

bool eb_runtime_assets_ready(void) {
    return s_ready;
}

EbAssetLoadResult eb_runtime_assets_load(const char *path) {
    if (s_ready) {
        eb_runtime_assets_unload();
    } else {
        clear_globals();
    }

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return EB_ASSETS_MISSING;
    }

    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size < (off_t)EB_PAK_HEADER_SIZE) {
        close(fd);
        return EB_ASSETS_IO_ERROR;
    }
    size_t file_size = (size_t)st.st_size;

    void *mapping = mmap(NULL, file_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd); /* the mapping holds its own reference; fd isn't needed after this */
    if (mapping == MAP_FAILED) {
        return EB_ASSETS_IO_ERROR;
    }

    const unsigned char *bytes = (const unsigned char *)mapping;

    if (memcmp(bytes, EB_PAK_MAGIC, 4) != 0) {
        munmap(mapping, file_size);
        return EB_ASSETS_BAD_MAGIC;
    }

    uint32_t version = read_u32le(bytes + 4);
    if (version != EB_PAK_VERSION) {
        munmap(mapping, file_size);
        return EB_ASSETS_VERSION_MISMATCH;
    }

    uint32_t asset_count = read_u32le(bytes + 8);

    char pak_hash_hex[EB_PAK_HASH_SIZE * 2 + 1];
    hash_to_hex(bytes + 12, pak_hash_hex);
    if (strcmp(pak_hash_hex, ASSET_PACK_LAYOUT_HASH) != 0) {
        munmap(mapping, file_size);
        return EB_ASSETS_LAYOUT_MISMATCH;
    }

    if (asset_count != (uint32_t)ASSET_COUNT) {
        munmap(mapping, file_size);
        return EB_ASSETS_COUNT_MISMATCH;
    }

    size_t index_table_size = (size_t)asset_count * EB_PAK_INDEX_ENTRY_SIZE;
    size_t blob_start = EB_PAK_HEADER_SIZE + index_table_size;
    if (file_size < blob_start) {
        munmap(mapping, file_size);
        return EB_ASSETS_IO_ERROR;
    }
    size_t blob_size = file_size - blob_start;

    const unsigned char *index = bytes + EB_PAK_HEADER_SIZE;
    for (uint32_t i = 0; i < asset_count; i++) {
        uint32_t offset = read_u32le(index + (size_t)i * EB_PAK_INDEX_ENTRY_SIZE);
        uint32_t length = read_u32le(index + (size_t)i * EB_PAK_INDEX_ENTRY_SIZE + 4);

        if (length == 0) {
            continue; /* gap entry: leave data=NULL, size=0 from clear_globals() */
        }
        if ((uint64_t)offset + (uint64_t)length > (uint64_t)blob_size) {
            munmap(mapping, file_size);
            clear_globals();
            return EB_ASSETS_IO_ERROR; /* truncated/corrupt pak */
        }
        embedded_assets[i].data = bytes + blob_start + offset;
        s_asset_lengths[i] = length;
    }

    s_mapping = mapping;
    s_mapping_size = file_size;
    s_ready = true;

    eb_runtime_assets_populate_families();

    return EB_ASSETS_OK;
}

#endif /* EB_RUNTIME_ASSETS */
