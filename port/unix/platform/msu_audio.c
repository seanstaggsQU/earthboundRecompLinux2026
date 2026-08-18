/* MSU1 PCM track player — see the "MSU1 audio" section of platform.h.
 *
 * File format (the de facto community convention, same one real SNES MSU1
 * romhacks use): "<pack_name>-<track_id>.pcm" per track, each:
 *   offset 0, 4 bytes: magic "MSU1"
 *   offset 4, 4 bytes: loop point, LE u32, in stereo sample FRAMES
 *   offset 8+:         raw interleaved S16LE stereo samples @ 44100 Hz
 *
 * Uses its own mutex, deliberately separate from audio_mutex (game/audio.c):
 * platform_audio_msu_play()/_stop() are called from inside an audio_lock()/
 * audio_unlock() pair in change_music()/stop_music() (see the comment there
 * about write_apu_port1() and non-reentrant locking) — using audio_mutex
 * here too would deadlock.
 *
 * Only compiled when EB_ENABLE_AUDIO is on (matches sdl2_audio.c). Loading
 * a pack is opt-in (platform_audio_msu_load(), wired to --msu-dir/--msu-name
 * in main.c) — with no pack loaded, platform_audio_msu_play() always
 * returns false and platform_audio_msu_mix() is a no-op, so this is
 * entirely inert for players who never point at a pack.
 *
 * platform_audio_msu_autodetect_name() (below) lets main.c skip needing
 * --msu-dir/--msu-name as CLI flags/launch options at all: pointed at a
 * folder containing exactly one pack, it inspects one "<name>-<track>.pcm"
 * filename to recover <name> itself, so dropping a pack into the
 * conventional default location (a "msu" folder next to the game's other
 * per-install files) just works with zero configuration -- important for
 * launchers (e.g. a Steam shortcut) where editing Launch Options is real
 * friction, especially every time the pack changes. */
#include "platform/platform.h"

#ifdef EB_ENABLE_AUDIO

#include <stdio.h>
#include <string.h>
#ifdef _WIN32
/* No mmap on Windows -- load the whole PCM file into a malloc'd buffer
 * instead of mapping it. Tracks are at most a few tens of MB (uncompressed
 * 16-bit stereo @ 44.1kHz), so reading the file up front is negligible.
 * _findfirst/_findnext (io.h) stand in for opendir/readdir for the name
 * autodetection's directory listing. */
#include <stdlib.h>
#include <io.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>
#endif
#include <SDL.h>

#define MSU_SOURCE_RATE 44100.0

static char msu_dir[900];
static char msu_name[128];
static bool msu_pack_loaded = false;

static SDL_mutex *msu_mutex;

/* Currently open/mapped track (NULL when nothing is streaming) */
static void *msu_map;
static size_t msu_map_size;
static const int16_t *msu_pcm;     /* mapped[8:], interleaved stereo S16LE */
static uint32_t msu_total_frames;  /* stereo sample frames in msu_pcm */
static uint32_t msu_loop_frame;    /* loop target, in source frames */
static double msu_read_pos;        /* fractional read cursor, in source frames */
static bool msu_active;

static void msu_close_locked(void) {
    if (msu_map) {
#ifdef _WIN32
        free(msu_map);
#else
        munmap(msu_map, msu_map_size);
#endif
        msu_map = NULL;
    }
    msu_pcm = NULL;
    msu_total_frames = 0;
    msu_loop_frame = 0;
    msu_read_pos = 0.0;
    msu_active = false;
}

void platform_audio_msu_load(const char *dir, const char *pack_name) {
    if (!msu_mutex) msu_mutex = SDL_CreateMutex();
    SDL_LockMutex(msu_mutex);
    snprintf(msu_dir, sizeof(msu_dir), "%s", dir);
    snprintf(msu_name, sizeof(msu_name), "%s", pack_name);
    msu_pack_loaded = true;
    SDL_UnlockMutex(msu_mutex);
}

/* If `filename` matches "<name>-<digits>.pcm", writes <name> (everything
 * before the last '-') into out_name and returns true. Used by
 * platform_audio_msu_autodetect_name() below to recover a pack's name
 * prefix from one of its own track filenames, so it never has to be known
 * ahead of time. */
static bool msu_parse_track_filename(const char *filename, char *out_name, size_t out_size) {
    size_t len = strlen(filename);
    static const char ext[] = ".pcm";
    size_t ext_len = sizeof(ext) - 1;
    if (len <= ext_len || strcmp(filename + len - ext_len, ext) != 0)
        return false;

    size_t base_len = len - ext_len; /* filename length without ".pcm" */
    size_t i = base_len;
    while (i > 0 && filename[i - 1] >= '0' && filename[i - 1] <= '9')
        i--;
    if (i == base_len)         return false; /* no track-number digits before .pcm */
    if (i == 0 || filename[i - 1] != '-') return false; /* digits not preceded by '-' */

    size_t name_len = i - 1; /* exclude the '-' itself */
    if (name_len == 0 || name_len >= out_size) return false;

    memcpy(out_name, filename, name_len);
    out_name[name_len] = '\0';
    return true;
}

/* Scans `dir` for any "<name>-<track>.pcm" file and, on the first match,
 * writes <name> into out_name. Returns false if `dir` doesn't exist or
 * has no matching files (not an error -- just "nothing to autodetect
 * here", exactly like no pack being configured at all). Assumes a single
 * pack per directory, which matches how main.c's default "msu" folder is
 * meant to be used. */
bool platform_audio_msu_autodetect_name(const char *dir, char *out_name, size_t out_size) {
    bool found = false;
#ifdef _WIN32
    char pattern[900];
    snprintf(pattern, sizeof(pattern), "%s\\*.pcm", dir);
    struct _finddata_t fd;
    intptr_t h = _findfirst(pattern, &fd);
    if (h != -1) {
        do {
            if (msu_parse_track_filename(fd.name, out_name, out_size)) {
                found = true;
                break;
            }
        } while (_findnext(h, &fd) == 0);
        _findclose(h);
    }
#else
    DIR *dp = opendir(dir);
    if (dp) {
        struct dirent *entry;
        while ((entry = readdir(dp)) != NULL) {
            if (msu_parse_track_filename(entry->d_name, out_name, out_size)) {
                found = true;
                break;
            }
        }
        closedir(dp);
    }
#endif
    return found;
}

bool platform_audio_msu_play(uint16_t track_id) {
    if (!msu_pack_loaded) return false;

    char path[1024];
    snprintf(path, sizeof(path), "%s/%s-%u.pcm", msu_dir, msu_name, (unsigned)track_id);

#ifdef _WIN32
    FILE *pf = fopen(path, "rb");
    if (!pf) return false; /* not covered by the pack -- normal, not an error */
    if (fseek(pf, 0, SEEK_END) != 0) { fclose(pf); return false; }
    long fsize = ftell(pf);
    if (fsize < 8) { fclose(pf); return false; }
    rewind(pf);
    size_t size = (size_t)fsize;
    void *map = malloc(size);
    if (!map) { fclose(pf); return false; }
    size_t got = fread(map, 1, size, pf);
    fclose(pf);
    if (got != size) { free(map); return false; }
#else
    int fd = open(path, O_RDONLY);
    if (fd < 0) return false; /* not covered by the pack -- normal, not an error */

    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size < 8) {
        close(fd);
        return false;
    }
    size_t size = (size_t)st.st_size;

    void *map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (map == MAP_FAILED) return false;
#endif

    const unsigned char *hdr = (const unsigned char *)map;
    if (memcmp(hdr, "MSU1", 4) != 0) {
#ifdef _WIN32
        free(map);
#else
        munmap(map, size);
#endif
        fprintf(stderr, "msu: %s has no MSU1 magic, skipping\n", path);
        return false;
    }
    uint32_t loop_frame = (uint32_t)hdr[4] | ((uint32_t)hdr[5] << 8) | ((uint32_t)hdr[6] << 16) | ((uint32_t)hdr[7] << 24);
    uint32_t total_frames = (uint32_t)((size - 8) / 4); /* 4 bytes = 1 stereo S16 frame */

    SDL_LockMutex(msu_mutex);
    msu_close_locked();
    msu_map = map;
    msu_map_size = size;
    msu_pcm = (const int16_t *)((const unsigned char *)map + 8);
    msu_total_frames = total_frames;
    msu_loop_frame = (loop_frame < total_frames) ? loop_frame : 0;
    msu_read_pos = 0.0;
    msu_active = (total_frames > 0);
    SDL_UnlockMutex(msu_mutex);
    return true;
}

void platform_audio_msu_stop(void) {
    if (!msu_mutex) return; /* platform_audio_msu_load() never called -- nothing to stop */
    SDL_LockMutex(msu_mutex);
    msu_close_locked();
    SDL_UnlockMutex(msu_mutex);
}

/* Mixes `frames` stereo frames of MSU PCM, resampled from 44100 Hz to
 * out_rate, ADDING (not overwriting -- lets the SPC700/DSP's sound effects
 * keep playing underneath) into `out` with clipping. Linear interpolation
 * between adjacent source frames; simple but adequate for a music bed.
 * No-op if nothing is actively streaming. Called from the audio callback
 * thread (sdl2_audio.c), guarded by msu_mutex, never audio_mutex. */
void platform_audio_msu_mix(int16_t *out, int frames, int out_rate) {
    if (!msu_mutex) return;
    SDL_LockMutex(msu_mutex);
    if (!msu_active || !msu_pcm) {
        SDL_UnlockMutex(msu_mutex);
        return;
    }

    double step = MSU_SOURCE_RATE / (double)out_rate;
    for (int i = 0; i < frames; i++) {
        uint32_t i0 = (uint32_t)msu_read_pos;
        if (i0 >= msu_total_frames) {
            /* Reached the end: loop back (a zero loop point with a very
             * short/empty pre-loop section behaves like a plain repeat). */
            msu_read_pos -= (double)(msu_total_frames - msu_loop_frame);
            i0 = (uint32_t)msu_read_pos;
            if (i0 >= msu_total_frames) { i0 = msu_loop_frame; msu_read_pos = i0; }
        }
        uint32_t i1 = i0 + 1;
        if (i1 >= msu_total_frames) i1 = msu_loop_frame;
        double frac = msu_read_pos - (double)i0;

        for (int ch = 0; ch < 2; ch++) {
            int32_t s0 = msu_pcm[i0 * 2 + ch];
            int32_t s1 = msu_pcm[i1 * 2 + ch];
            int32_t sample = (int32_t)(s0 + (s1 - s0) * frac);
            int32_t mixed = (int32_t)out[i * 2 + ch] + sample;
            if (mixed > 32767) mixed = 32767;
            if (mixed < -32768) mixed = -32768;
            out[i * 2 + ch] = (int16_t)mixed;
        }

        msu_read_pos += step;
    }
    SDL_UnlockMutex(msu_mutex);
}

#endif /* EB_ENABLE_AUDIO */
