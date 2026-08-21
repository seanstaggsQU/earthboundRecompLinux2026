/* Builds assets.pak from a raw ROM file, see rom_extract.h.
 *
 * Only compiled when EB_RUNTIME_ASSETS is defined. Uses POSIX file I/O,
 * matching runtime_assets.c's own "port/unix only for now" convention. */
#include "rom_extract.h"

#ifdef EB_RUNTIME_ASSETS

#include "asset_ids.h"
#include "asset_pack_layout.h"
#include "rom_extract_table.h"

#include <dirent.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
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

/* Port of ebtools/rom.py's detect(): checks the SNES header at both
 * possible offsets (headerless and headered), validates the built-in
 * checksum/complement pair, then matches the cart title against
 * ROM_IDENTIFIER (see rom_extract_table.h). */
static bool detect_rom(const unsigned char *data, size_t len, bool *out_header) {
    static const size_t bases[2] = { 0xFFB0, 0x101B0 };
    static const bool headered[2] = { false, true };
    size_t id_len = sizeof(ROM_IDENTIFIER) - 1;

    for (int i = 0; i < 2; i++) {
        size_t base = bases[i];
        if (base + 48 > len) {
            continue;
        }
        uint16_t checksum_complement = (uint16_t)data[base + 44] | ((uint16_t)data[base + 45] << 8);
        uint16_t checksum = (uint16_t)data[base + 46] | ((uint16_t)data[base + 47] << 8);
        if ((uint16_t)(checksum ^ checksum_complement) != 0xFFFF) {
            continue;
        }
        if (base + 16 + id_len > len) {
            continue;
        }
        if (memcmp(data + base + 16, ROM_IDENTIFIER, id_len) == 0) {
            *out_header = headered[i];
            return true;
        }
    }
    return false;
}

/* mkdir -p, one path component at a time. POSIX only (see file header). */
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

EbRomExtractResult rom_extract_build_pak(const char *rom_path, const char *out_path) {
    FILE *rf = fopen(rom_path, "rb");
    if (!rf) {
        return EB_ROM_EXTRACT_IO_ERROR;
    }
    if (fseek(rf, 0, SEEK_END) != 0) {
        fclose(rf);
        return EB_ROM_EXTRACT_IO_ERROR;
    }
    long rom_len = ftell(rf);
    if (rom_len < 0 || fseek(rf, 0, SEEK_SET) != 0) {
        fclose(rf);
        return EB_ROM_EXTRACT_IO_ERROR;
    }

    unsigned char *rom_data = malloc((size_t)rom_len);
    if (!rom_data) {
        fclose(rf);
        return EB_ROM_EXTRACT_IO_ERROR;
    }
    size_t nread = fread(rom_data, 1, (size_t)rom_len, rf);
    fclose(rf);
    if (nread != (size_t)rom_len) {
        free(rom_data);
        return EB_ROM_EXTRACT_IO_ERROR;
    }

    if ((size_t)rom_len < ROM_SIZE) {
        free(rom_data);
        return EB_ROM_EXTRACT_TOO_SMALL;
    }

    bool header = false;
    if (!detect_rom(rom_data, (size_t)rom_len, &header)) {
        free(rom_data);
        return EB_ROM_EXTRACT_NOT_MATCHED;
    }

    const unsigned char *rom = rom_data + (header ? 0x200 : 0);
    size_t rom_avail = (size_t)rom_len - (header ? 0x200 : 0);
    if (rom_avail < ROM_SIZE) {
        free(rom_data);
        return EB_ROM_EXTRACT_TOO_SMALL;
    }

    /* Blob offsets: each asset's slot in the pak's blob region, in AssetId
     * order, packed back to back (gaps take no space). */
    uint32_t *blob_offsets = malloc(sizeof(uint32_t) * ASSET_COUNT);
    if (!blob_offsets) {
        free(rom_data);
        return EB_ROM_EXTRACT_IO_ERROR;
    }
    uint32_t running = 0;
    for (int i = 0; i < ASSET_COUNT; i++) {
        uint32_t sz = rom_extract_table[i].rom_size;
        blob_offsets[i] = sz ? running : 0;
        running += sz;
    }

    char tmp_path[4096];
    int n = snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", out_path);
    if (n <= 0 || (size_t)n >= sizeof(tmp_path)) {
        free(rom_data);
        free(blob_offsets);
        return EB_ROM_EXTRACT_WRITE_FAILED;
    }

    mkdir_parents(out_path);

    FILE *of = fopen(tmp_path, "wb");
    if (!of) {
        free(rom_data);
        free(blob_offsets);
        return EB_ROM_EXTRACT_WRITE_FAILED;
    }

    bool ok = true;
    ok = ok && fwrite("EBPK", 1, 4, of) == 4;
    ok = ok && write_u32le(of, 1u); /* pak format version */
    ok = ok && write_u32le(of, (uint32_t)ASSET_COUNT);

    unsigned char hash_bytes[32];
    hex_to_bytes(ASSET_PACK_LAYOUT_HASH, hash_bytes, sizeof(hash_bytes));
    ok = ok && fwrite(hash_bytes, 1, sizeof(hash_bytes), of) == sizeof(hash_bytes);

    for (int i = 0; i < ASSET_COUNT && ok; i++) {
        ok = write_u32le(of, blob_offsets[i]) && write_u32le(of, rom_extract_table[i].rom_size);
    }

    for (int i = 0; i < ASSET_COUNT && ok; i++) {
        uint32_t off = rom_extract_table[i].rom_offset;
        uint32_t sz = rom_extract_table[i].rom_size;
        if (sz == 0) {
            continue;
        }
        if ((uint64_t)off + (uint64_t)sz > (uint64_t)rom_avail) {
            ok = false;
            break;
        }
        ok = fwrite(rom + off, 1, sz, of) == sz;
    }

    fclose(of);
    free(rom_data);
    free(blob_offsets);

    if (!ok) {
        remove(tmp_path);
        return EB_ROM_EXTRACT_WRITE_FAILED;
    }
    if (rename(tmp_path, out_path) != 0) {
        remove(tmp_path);
        return EB_ROM_EXTRACT_WRITE_FAILED;
    }

    return EB_ROM_EXTRACT_OK;
}

static bool has_rom_extension(const char *name) {
    size_t len = strlen(name);
    if (len < 4) {
        return false;
    }
    const char *ext = name + len - 4;
    return strcasecmp(ext, ".sfc") == 0 || strcasecmp(ext, ".smc") == 0;
}

EbRomExtractResult rom_extract_scan_and_build_pak(const char *dir, const char *out_path) {
    DIR *d = opendir(dir);
    if (!d) {
        return EB_ROM_EXTRACT_IO_ERROR;
    }

    bool tried_any = false;
    EbRomExtractResult best = EB_ROM_EXTRACT_IO_ERROR;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (!has_rom_extension(ent->d_name)) {
            continue;
        }

        char full_path[4096];
        int n = snprintf(full_path, sizeof(full_path), "%s/%s", dir, ent->d_name);
        if (n <= 0 || (size_t)n >= sizeof(full_path)) {
            continue;
        }

        struct stat st;
        if (stat(full_path, &st) != 0 || !S_ISREG(st.st_mode)) {
            continue; /* skip directories/symlinks-to-directories/etc named *.sfc */
        }

        tried_any = true;
        EbRomExtractResult r = rom_extract_build_pak(full_path, out_path);
        if (r == EB_ROM_EXTRACT_OK) {
            closedir(d);
            return EB_ROM_EXTRACT_OK;
        }
        /* Keep scanning -- a wrong/corrupt file with a .sfc name shouldn't
         * stop us from finding the real ROM sitting right next to it. */
        best = r;
    }
    closedir(d);

    return tried_any ? best : EB_ROM_EXTRACT_IO_ERROR;
}

#endif /* EB_RUNTIME_ASSETS */
