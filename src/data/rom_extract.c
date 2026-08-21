/* Builds assets.pak from a raw ROM file, see rom_extract.h.
 *
 * Only compiled when EB_RUNTIME_ASSETS is defined. Uses POSIX file I/O,
 * matching runtime_assets.c's own "port/unix only for now" convention. */
#include "rom_extract.h"

#ifdef EB_RUNTIME_ASSETS

#include "asset_ids.h"
#include "asset_pack_layout.h"
#include "core/decomp.h"
#include "rom_extract_table.h"

#include <dirent.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>
#define MKDIR_ONE(path) _mkdir(path)
#define strcasecmp _stricmp
#else
#include <strings.h>
#define MKDIR_ONE(path) mkdir(path, 0755)
#endif

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

/* ---- HALLZ2 encoder (byte-fill / word-fill / incrementing-fill / forward copy) ----
 *
 * Simplified port of ebtools/hallz/compress.py: same header encoding and
 * command framing, but only 4 of the 6 command types (skips bit-reversed
 * and backward copy -- PSI arrangement data compresses very well with just
 * these four; a real ROM arrangement's ~65KB bundle set shrinks to a few
 * KB with them). Doesn't need to match Python's output byte-for-byte, only
 * needs to decompress correctly via decomp() (core/decomp.c) -- and does,
 * self-checked in build_psi_bundle() below before it's ever written out. */

static void hallz_write_header(uint8_t **out, uint8_t cmd_id, size_t length) {
    if (length <= 32) {
        *(*out)++ = (uint8_t)((cmd_id << 5) | (length - 1));
    } else {
        uint32_t encoded = (uint32_t)(length - 1);
        *(*out)++ = (uint8_t)(0xE0 | (cmd_id << 2) | (encoded >> 8));
        *(*out)++ = (uint8_t)(encoded & 0xFF);
    }
}

static size_t hallz_header_cost(size_t length) {
    return length <= 32 ? 1 : 2;
}

/* Forward-match hash chain, sized for one bundle (max 8192 bytes -- see
 * PSI_FRAME_SIZE * PSI_FRAMES_PER_BUNDLE). Reset per hallz_encode() call. */
#define HALLZ_HASH_SIZE 65536u
#define HALLZ_MAX_CHAIN 256

static size_t count_byte_fill(const uint8_t *d, size_t n, size_t p) {
    uint8_t val = d[p];
    size_t limit = n < p + 1024 ? n : p + 1024;
    size_t i = p + 1;
    while (i < limit && d[i] == val) i++;
    return i - p;
}

static size_t count_word_fill(const uint8_t *d, size_t n, size_t p) {
    if (p + 3 >= n) return 0;
    uint8_t lo = d[p], hi = d[p + 1];
    size_t limit = n - 1 < p + 2048 ? n - 1 : p + 2048;
    size_t count = 1, i = p + 2;
    while (i < limit && d[i] == lo && d[i + 1] == hi) { count++; i += 2; }
    return count;
}

static size_t count_inc_fill(const uint8_t *d, size_t n, size_t p) {
    uint8_t val = d[p];
    size_t limit = n < p + 1024 ? n : p + 1024;
    size_t i = p + 1;
    while (i < limit && d[i] == (uint8_t)(val + (i - p))) i++;
    size_t run = i - p;
    return run > 1 ? run : 0;
}

/* Encodes `len` bytes of `data` (a single, self-contained buffer -- no
 * cross-call back-references, matching ebtools compress()'s per-bundle
 * call convention). Returns bytes written to out, or 0 on failure
 * (out_cap too small). */
static size_t hallz_encode(const uint8_t *data, size_t len, uint8_t *out, size_t out_cap) {
    int32_t *head = malloc(HALLZ_HASH_SIZE * sizeof(int32_t));
    int32_t *chain = malloc(len ? len * sizeof(int32_t) : sizeof(int32_t));
    if (!head || !chain) {
        free(head);
        free(chain);
        return 0;
    }
    for (uint32_t i = 0; i < HALLZ_HASH_SIZE; i++) head[i] = -1;

    uint8_t *out_start = out;
    uint8_t *out_end = out + out_cap;
    size_t pos = 0, literal_start = 0;
    bool ok = true;

#define HALLZ_HASH2(p) ((uint16_t)(((uint16_t)data[p] << 8) | data[(p) + 1]))
#define HALLZ_ADVANCE(p) \
    do { if ((p) + 1 < len) { uint16_t h = HALLZ_HASH2(p); chain[p] = head[h]; head[h] = (int32_t)(p); } } while (0)

    while (pos < len && ok) {
        size_t best_savings = 0;
        int best_cmd = -1; /* 1=byte-fill 2=word-fill 3=inc-fill 4=copy */
        size_t best_len = 0, best_off = 0;

        size_t run = count_byte_fill(data, len, pos);
        if (run >= 3) {
            size_t cost = hallz_header_cost(run) + 1;
            if (run > cost && run - cost > best_savings) { best_savings = run - cost; best_cmd = 1; best_len = run; }
        }
        run = count_inc_fill(data, len, pos);
        if (run >= 3) {
            size_t cost = hallz_header_cost(run) + 1;
            if (run > cost && run - cost > best_savings) { best_savings = run - cost; best_cmd = 3; best_len = run; }
        }
        size_t wrun = count_word_fill(data, len, pos);
        if (wrun >= 2) {
            size_t consumed = wrun * 2;
            size_t cost = hallz_header_cost(wrun) + 2;
            if (consumed > cost && consumed - cost > best_savings) { best_savings = consumed - cost; best_cmd = 2; best_len = wrun; }
        }
        if (pos > 0 && pos + 1 < len) {
            uint16_t h = HALLZ_HASH2(pos);
            int32_t off = head[h];
            int chain_left = HALLZ_MAX_CHAIN;
            size_t max_len = len - pos < 1024 ? len - pos : 1024;
            while (off >= 0 && chain_left-- > 0) {
                if (data[off] == data[pos]) {
                    size_t l = 0;
                    while (l < max_len && data[(size_t)off + l] == data[pos + l]) l++;
                    if (l >= 4) {
                        size_t cost = hallz_header_cost(l) + 2;
                        if (l > cost && l - cost > best_savings) { best_savings = l - cost; best_cmd = 4; best_len = l; best_off = (size_t)off; }
                    }
                }
                off = chain[off];
            }
        }

        if (best_cmd >= 0 && best_savings > 0) {
            if (pos > literal_start) {
                size_t lit_len = pos - literal_start;
                size_t lp = literal_start;
                while (lp < pos) {
                    size_t chunk = pos - lp < 1024 ? pos - lp : 1024;
                    if (out + hallz_header_cost(chunk) + chunk > out_end) { ok = false; break; }
                    hallz_write_header(&out, 0, chunk);
                    memcpy(out, data + lp, chunk);
                    out += chunk;
                    lp += chunk;
                }
                (void)lit_len;
                if (!ok) break;
            }
            size_t consumed;
            if (best_cmd == 1 || best_cmd == 3) {
                if (out + hallz_header_cost(best_len) + 1 > out_end) { ok = false; break; }
                hallz_write_header(&out, (uint8_t)best_cmd, best_len);
                *out++ = data[pos];
                consumed = best_len;
            } else if (best_cmd == 2) {
                if (out + hallz_header_cost(best_len) + 2 > out_end) { ok = false; break; }
                hallz_write_header(&out, 2, best_len);
                *out++ = data[pos];
                *out++ = data[pos + 1];
                consumed = best_len * 2;
            } else {
                if (out + hallz_header_cost(best_len) + 2 > out_end) { ok = false; break; }
                hallz_write_header(&out, 4, best_len);
                *out++ = (uint8_t)(best_off >> 8);
                *out++ = (uint8_t)(best_off & 0xFF);
                consumed = best_len;
            }
            for (size_t i = pos; i < pos + consumed; i++) HALLZ_ADVANCE(i);
            pos += consumed;
            literal_start = pos;
        } else {
            HALLZ_ADVANCE(pos);
            pos++;
            if (pos - literal_start >= 1024) {
                if (out + hallz_header_cost(1024) + 1024 > out_end) { ok = false; break; }
                hallz_write_header(&out, 0, 1024);
                memcpy(out, data + literal_start, 1024);
                out += 1024;
                literal_start = pos;
            }
        }
    }
    if (ok && pos > literal_start) {
        size_t lp = literal_start;
        while (lp < pos) {
            size_t chunk = pos - lp < 1024 ? pos - lp : 1024;
            if (out + hallz_header_cost(chunk) + chunk > out_end) { ok = false; break; }
            hallz_write_header(&out, 0, chunk);
            memcpy(out, data + lp, chunk);
            out += chunk;
            lp += chunk;
        }
    }
    if (ok) {
        if (out + 1 > out_end) ok = false;
        else *out++ = 0xFF;
    }

#undef HALLZ_HASH2
#undef HALLZ_ADVANCE
    free(head);
    free(chain);
    return ok ? (size_t)(out - out_start) : 0;
}

/* Max decompressed size of any single PSI arrangement, generously rounded
 * up from the current largest (65536 bytes) -- see docs/get-your-own-assets.md
 * note in rom_extract.h if this ever needs bumping. */
#define PSI_ARRANGEMENT_MAX_RAW (128u * 1024u)
#define PSI_FRAME_SIZE 1024u
#define PSI_FRAMES_PER_BUNDLE 8u

/* Builds one .arr.bundled blob (see ebtools/parsers/psi_arrangements.py for
 * the format) from a single PSI arrangement's raw (still-compressed) ROM
 * bytes. Decompresses once, splits into 8-frame chunks, literal-encodes
 * each chunk independently (see hallz_encode_literal() -- no real
 * compression needed, decomp() only cares that the stream is valid).
 * Returns the blob size written to out, or 0 on failure. */
static size_t build_psi_bundle(const uint8_t *lzhal_data, uint32_t lzhal_size, uint8_t *out, size_t out_cap) {
    uint8_t *raw = malloc(PSI_ARRANGEMENT_MAX_RAW);
    if (!raw) return 0;
    size_t raw_size = decomp(lzhal_data, lzhal_size, raw, PSI_ARRANGEMENT_MAX_RAW);
    if (raw_size == 0 || raw_size % PSI_FRAME_SIZE != 0) {
        free(raw);
        return 0;
    }

    uint32_t total_frames = (uint32_t)(raw_size / PSI_FRAME_SIZE);
    uint32_t bundle_count = (total_frames + PSI_FRAMES_PER_BUNDLE - 1) / PSI_FRAMES_PER_BUNDLE;

    /* Header: u8 frames_per_bundle, u8 total_frames, u16 bundle_count,
     * then (bundle_count+1) u16 offsets (from start of data section),
     * then the concatenated per-bundle literal-encoded streams. Matches
     * pack_bundled_arrangements()'s struct.pack("<BBH", ...) + "<NH". */
    if (total_frames > 255 || bundle_count > 0xFFFF) {
        free(raw);
        return 0; /* would overflow the u8/u16 header fields -- not expected in practice */
    }

    size_t header_size = 4 + (size_t)(bundle_count + 1) * 2;
    if (header_size > out_cap) {
        free(raw);
        return 0;
    }
    out[0] = (uint8_t)PSI_FRAMES_PER_BUNDLE;
    out[1] = (uint8_t)total_frames;
    out[2] = (uint8_t)(bundle_count & 0xFF);
    out[3] = (uint8_t)(bundle_count >> 8);

    size_t data_pos = 0;
    bool ok = true;
    for (uint32_t b = 0; b < bundle_count && ok; b++) {
        /* offsets[b] written once data_pos for this bundle is known */
        uint32_t start_frame = b * PSI_FRAMES_PER_BUNDLE;
        uint32_t end_frame = start_frame + PSI_FRAMES_PER_BUNDLE;
        if (end_frame > total_frames) end_frame = total_frames;
        size_t bundle_raw_size = (size_t)(end_frame - start_frame) * PSI_FRAME_SIZE;

        size_t off_field = 4 + (size_t)b * 2;
        out[off_field] = (uint8_t)(data_pos & 0xFF);
        out[off_field + 1] = (uint8_t)(data_pos >> 8);

        uint8_t *bundle_out = out + header_size + data_pos;
        size_t bundle_out_cap = out_cap > header_size + data_pos ? out_cap - header_size - data_pos : 0;
        size_t written = hallz_encode(raw + (size_t)start_frame * PSI_FRAME_SIZE, bundle_raw_size,
                                       bundle_out, bundle_out_cap);
        if (written == 0) {
            ok = false;
            break;
        }
        /* Round-trip self-check: decompress what we just wrote and confirm
         * it reproduces the exact input bytes before trusting it. Cheap
         * (this whole function runs once per arrangement, at most 34 times,
         * only on first launch) and turns any encoder bug into a clean
         * failure (falls through to the "couldn't build a pak" path)
         * instead of silently shipping corrupt PSI animation data. */
        {
            uint8_t *verify = malloc(bundle_raw_size ? bundle_raw_size : 1);
            size_t got = verify ? decomp(bundle_out, written, verify, bundle_raw_size) : 0;
            bool matches = verify && got == bundle_raw_size &&
                            memcmp(verify, raw + (size_t)start_frame * PSI_FRAME_SIZE, bundle_raw_size) == 0;
            free(verify);
            if (!matches) {
                ok = false;
                break;
            }
        }
        /* Format constraint: offsets are u16 (see the struct format in
         * ebtools/parsers/psi_arrangements.py) -- refuse rather than
         * silently truncate if we ever blew past that. */
        if (data_pos + written > 0xFFFFu) {
            ok = false;
            break;
        }
        data_pos += written;
    }
    /* Sentinel offset entry: total data section size */
    if (ok) {
        size_t off_field = 4 + (size_t)bundle_count * 2;
        out[off_field] = (uint8_t)(data_pos & 0xFF);
        out[off_field + 1] = (uint8_t)(data_pos >> 8);
    }

    free(raw);
    return ok ? header_size + data_pos : 0;
}

/* mkdir -p, one path component at a time. Splits on both '/' and '\\' --
 * eb_runtime_assets_default_path() (see runtime_assets.h) builds a
 * backslash-separated path on Windows. */
static void mkdir_parents(const char *path) {
    char buf[4096];
    size_t len = strlen(path);
    if (len == 0 || len >= sizeof(buf)) {
        return;
    }
    memcpy(buf, path, len + 1);
    for (size_t i = 1; i < len; i++) {
        char c = buf[i];
        if (c == '/' || c == '\\') {
            buf[i] = '\0';
            MKDIR_ONE(buf);
            buf[i] = c;
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

    /* Build the 34 PSI arrangement bundles up front (see build_psi_bundle):
     * these AssetIds don't get a plain ROM byte-slice like everything
     * else, since the final asset is a derived, re-chunked/recompressed
     * form of the raw ROM data (see rom_extract_table.h's comment on
     * psi_arrangement_source[]). Output cap generously sized -- real
     * compression on this data runs 90%+, comfortably under the format's
     * own 65535-byte-per-arrangement ceiling (checked inside the builder
     * too, so a failure here is a clean bail-out, not silent corruption). */
    uint8_t *psi_bundles[PSI_ARRANGEMENT_COUNT] = { 0 };
    size_t psi_bundle_sizes[PSI_ARRANGEMENT_COUNT] = { 0 };
    bool psi_ok = true;
    for (int i = 0; i < PSI_ARRANGEMENT_COUNT && psi_ok; i++) {
        RomExtractEntry src = psi_arrangement_source[i];
        if (src.rom_size == 0) {
            continue; /* no source in this build's earthbound.yml -- leave empty */
        }
        if ((uint64_t)src.rom_offset + (uint64_t)src.rom_size > (uint64_t)rom_avail) {
            psi_ok = false;
            break;
        }
        uint8_t *buf = malloc(70000); /* generous; well above any observed compressed size */
        if (!buf) {
            psi_ok = false;
            break;
        }
        size_t sz = build_psi_bundle(rom + src.rom_offset, src.rom_size, buf, 70000);
        if (sz == 0) {
            free(buf);
            psi_ok = false;
            break;
        }
        psi_bundles[i] = buf;
        psi_bundle_sizes[i] = sz;
    }
    if (!psi_ok) {
        for (int i = 0; i < PSI_ARRANGEMENT_COUNT; i++) free(psi_bundles[i]);
        free(rom_data);
        return EB_ROM_EXTRACT_WRITE_FAILED;
    }

    int psi_base = (int)ASSET_PSIANIMS_ARRANGEMENTS(0);

    /* Effective per-asset size: the plain ROM byte range, except for the
     * PSI arrangement family, which uses its derived bundle size instead. */
    uint32_t *eff_size = malloc(sizeof(uint32_t) * ASSET_COUNT);
    if (!eff_size) {
        for (int i = 0; i < PSI_ARRANGEMENT_COUNT; i++) free(psi_bundles[i]);
        free(rom_data);
        return EB_ROM_EXTRACT_IO_ERROR;
    }
    for (int i = 0; i < ASSET_COUNT; i++) {
        int psi_idx = i - psi_base;
        eff_size[i] = (psi_idx >= 0 && psi_idx < PSI_ARRANGEMENT_COUNT)
                          ? (uint32_t)psi_bundle_sizes[psi_idx]
                          : rom_extract_table[i].rom_size;
    }

    /* Blob offsets: each asset's slot in the pak's blob region, in AssetId
     * order, packed back to back (gaps take no space). */
    uint32_t *blob_offsets = malloc(sizeof(uint32_t) * ASSET_COUNT);
    if (!blob_offsets) {
        for (int i = 0; i < PSI_ARRANGEMENT_COUNT; i++) free(psi_bundles[i]);
        free(eff_size);
        free(rom_data);
        return EB_ROM_EXTRACT_IO_ERROR;
    }
    uint32_t running = 0;
    for (int i = 0; i < ASSET_COUNT; i++) {
        uint32_t sz = eff_size[i];
        blob_offsets[i] = sz ? running : 0;
        running += sz;
    }

    char tmp_path[4096];
    int n = snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", out_path);
    if (n <= 0 || (size_t)n >= sizeof(tmp_path)) {
        for (int i = 0; i < PSI_ARRANGEMENT_COUNT; i++) free(psi_bundles[i]);
        free(eff_size);
        free(rom_data);
        free(blob_offsets);
        return EB_ROM_EXTRACT_WRITE_FAILED;
    }

    mkdir_parents(out_path);

    FILE *of = fopen(tmp_path, "wb");
    if (!of) {
        for (int i = 0; i < PSI_ARRANGEMENT_COUNT; i++) free(psi_bundles[i]);
        free(eff_size);
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
        ok = write_u32le(of, blob_offsets[i]) && write_u32le(of, eff_size[i]);
    }

    for (int i = 0; i < ASSET_COUNT && ok; i++) {
        uint32_t sz = eff_size[i];
        if (sz == 0) {
            continue;
        }
        int psi_idx = i - psi_base;
        if (psi_idx >= 0 && psi_idx < PSI_ARRANGEMENT_COUNT) {
            ok = fwrite(psi_bundles[psi_idx], 1, sz, of) == sz;
            continue;
        }
        uint32_t off = rom_extract_table[i].rom_offset;
        if ((uint64_t)off + (uint64_t)sz > (uint64_t)rom_avail) {
            ok = false;
            break;
        }
        ok = fwrite(rom + off, 1, sz, of) == sz;
    }

    fclose(of);
    for (int i = 0; i < PSI_ARRANGEMENT_COUNT; i++) free(psi_bundles[i]);
    free(eff_size);
    free(rom_data);
    free(blob_offsets);

    if (!ok) {
        remove(tmp_path);
        return EB_ROM_EXTRACT_WRITE_FAILED;
    }
#ifdef _WIN32
    /* Unlike POSIX, Windows' rename() fails if out_path already exists. */
    remove(out_path);
#endif
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

bool rom_extract_find_rom(const char *dir, char *out_path, size_t out_cap) {
    DIR *d = opendir(dir);
    if (!d) {
        return false;
    }

    /* Only need enough of the file to run detect_rom()'s header check
     * (bases up to 0x101B0 + 48 bytes) -- no need to read the whole ROM
     * just to locate it. */
    unsigned char header_buf[0x20000];

    struct dirent *ent;
    bool found = false;
    while (!found && (ent = readdir(d)) != NULL) {
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
            continue;
        }

        FILE *f = fopen(full_path, "rb");
        if (!f) {
            continue;
        }
        size_t nread = fread(header_buf, 1, sizeof(header_buf), f);
        fclose(f);

        bool header = false;
        if (detect_rom(header_buf, nread, &header)) {
            if (snprintf(out_path, out_cap, "%s", full_path) < (int)out_cap) {
                found = true;
            }
        }
    }
    closedir(d);
    return found;
}

#endif /* EB_RUNTIME_ASSETS */
