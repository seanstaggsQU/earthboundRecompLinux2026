/* Exports this binary's own compiled-in assets to an assets.pak, see
 * pak_export.h. Only compiled when EB_RUNTIME_ASSETS is NOT defined --
 * that's the only build with real data in embedded_assets[] before any
 * pak is loaded. POSIX file I/O, matching runtime_assets.c/rom_extract.c's
 * own "port/unix only for now" convention. */
#include "pak_export.h"

#ifndef EB_RUNTIME_ASSETS

#include "asset_ids.h"
#include "asset_pack_hash.h"
#include "embedded_assets.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static bool write_u32le(FILE *f, uint32_t v) {
    unsigned char b[4] = { (unsigned char)v, (unsigned char)(v >> 8), (unsigned char)(v >> 16), (unsigned char)(v >> 24) };
    return fwrite(b, 1, 4, f) == 4;
}

static void hex_to_bytes(const char *hex, unsigned char *out, size_t out_len) {
    for (size_t i = 0; i < out_len; i++) {
        unsigned hi = 0, lo = 0;
        sscanf(hex + i * 2, "%1x", &hi);
        sscanf(hex + i * 2 + 1, "%1x", &lo);
        out[i] = (unsigned char)((hi << 4) | lo);
    }
}

/* mkdir -p, one path component at a time. */
static void mkdir_parents(const char *path) {
    char buf[4096];
    size_t len = strlen(path);
    if (len == 0 || len >= sizeof(buf)) {
        return;
    }
    memcpy(buf, path, len + 1);
    for (size_t i = 1; i < len; i++) {
        if (buf[i] == '/') {
            buf[i] = '\0';
            mkdir(buf, 0755);
            buf[i] = '/';
        }
    }
}

bool pak_export_write(const char *out_path) {
    /* Blob offsets, same packed-back-to-back scheme rom_extract.c uses. */
    uint32_t *blob_offsets = malloc(sizeof(uint32_t) * ASSET_COUNT);
    if (!blob_offsets) {
        return false;
    }
    uint32_t running = 0;
    for (int i = 0; i < ASSET_COUNT; i++) {
        unsigned int sz = embedded_assets[i].data ? *embedded_assets[i].size_ptr : 0;
        blob_offsets[i] = sz ? running : 0;
        running += sz;
    }

    char tmp_path[4096];
    int n = snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", out_path);
    if (n <= 0 || (size_t)n >= sizeof(tmp_path)) {
        free(blob_offsets);
        return false;
    }

    mkdir_parents(out_path);

    FILE *of = fopen(tmp_path, "wb");
    if (!of) {
        free(blob_offsets);
        return false;
    }

    bool ok = true;
    ok = ok && fwrite("EBPK", 1, 4, of) == 4;
    ok = ok && write_u32le(of, 1u); /* pak format version */
    ok = ok && write_u32le(of, (uint32_t)ASSET_COUNT);

    unsigned char hash_bytes[32];
    hex_to_bytes(ASSET_PACK_LAYOUT_HASH, hash_bytes, sizeof(hash_bytes));
    ok = ok && fwrite(hash_bytes, 1, sizeof(hash_bytes), of) == sizeof(hash_bytes);

    for (int i = 0; i < ASSET_COUNT && ok; i++) {
        unsigned int sz = embedded_assets[i].data ? *embedded_assets[i].size_ptr : 0;
        ok = write_u32le(of, blob_offsets[i]) && write_u32le(of, sz);
    }

    for (int i = 0; i < ASSET_COUNT && ok; i++) {
        if (!embedded_assets[i].data) {
            continue;
        }
        unsigned int sz = *embedded_assets[i].size_ptr;
        if (sz == 0) {
            continue;
        }
        ok = fwrite(embedded_assets[i].data, 1, sz, of) == sz;
    }

    fclose(of);
    free(blob_offsets);

    if (!ok) {
        remove(tmp_path);
        return false;
    }
    if (rename(tmp_path, out_path) != 0) {
        remove(tmp_path);
        return false;
    }
    return true;
}

#endif /* !EB_RUNTIME_ASSETS */
