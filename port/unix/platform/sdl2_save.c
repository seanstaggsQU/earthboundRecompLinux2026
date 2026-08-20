/*
 * Unix/SDL2 save data implementation — file-backed persistent storage.
 *
 * Save data is stored in a single file, default "saves/earthbound.srm" --
 * a dedicated subfolder next to the executable (not a flat file directly
 * beside it) so it doesn't collide with, or get mistaken for, some other
 * incidental file living in the same directory as the binary. main.c's
 * migrate_legacy_saves_to_saves_folder() copies an older flat-file
 * earthbound.srm into this location the first time a build with this
 * change runs, leaving the original in place untouched.
 * The file path can be overridden via --save on the command line.
 */
#include "platform/platform.h"
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>    /* _mkdir */
#define MKDIR(path) _mkdir(path)
#else
#include <sys/stat.h>  /* mkdir */
#define MKDIR(path) mkdir(path, 0755)
#endif

static const char *save_file_path = "saves/earthbound.srm";
static bool save_file_path_is_default = true; /* becomes false on platform_save_set_path() (--save) */

void platform_save_set_path(const char *path) {
    save_file_path = path;
    save_file_path_is_default = false; /* explicit override -- don't create saves/ for it */
}

bool platform_save_init(void) {
    /* Nothing to initialize for file-backed saves */
    return true;
}

size_t platform_save_read(void *dst, size_t offset, size_t size) {
    FILE *f = fopen(save_file_path, "rb");
    if (!f) return 0;

    if (fseek(f, (long)offset, SEEK_SET) != 0) {
        fclose(f);
        return 0;
    }

    size_t read = fread(dst, 1, size, f);
    fclose(f);
    return read;
}

bool platform_save_write(const void *src, size_t offset, size_t size) {
    /* Ensure the saves/ folder exists before the very first write to the
     * default path -- harmless no-op once it already exists (matching the
     * MKDIR-and-ignore-failure pattern already used for the debug/ folder
     * in sdl2_debug.c). Skipped for an explicit --save override, which may
     * not even want a "saves" subfolder relative to it. */
    if (save_file_path_is_default)
        MKDIR("saves");

    /* Open existing or create new */
    FILE *f = fopen(save_file_path, "r+b");
    if (!f) {
        f = fopen(save_file_path, "w+b");
        if (!f) return false;
    }

    if (fseek(f, (long)offset, SEEK_SET) != 0) {
        fclose(f);
        return false;
    }

    size_t written = fwrite(src, 1, size, f);
    fclose(f);
    return written == size;
}
