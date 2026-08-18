/*
 * Unix/SDL2 engine-settings storage — a single small file, next to the
 * savestate/save files (CWD). See src/platform/platform.h's "Engine
 * settings" section and src/game/settings.h for the blob this stores.
 */
#include "platform/platform.h"
#include <stdio.h>

static const char *settings_file_path = "settings.dat";

size_t platform_settings_read(void *dst, size_t size) {
    FILE *f = fopen(settings_file_path, "rb");
    if (!f) return 0; /* no settings file yet -- first run, not an error */
    size_t read = fread(dst, 1, size, f);
    fclose(f);
    return read;
}

bool platform_settings_write(const void *src, size_t size) {
    FILE *f = fopen(settings_file_path, "wb");
    if (!f) return false;
    size_t written = fwrite(src, 1, size, f);
    fclose(f);
    return written == size;
}
