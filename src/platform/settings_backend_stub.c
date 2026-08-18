/*
 * Default no-op engine-settings storage backend.
 *
 * Mirrors src/platform/savestate_backend_stub.c's convention exactly, for
 * the same reason: platform_settings_* (platform.h) is target-specific --
 * desktop writes a small file (port/unix/platform/sdl2_settings.c); an
 * embedded target might use flash, or nothing yet.
 *
 * Until an embedded target provides a real backend, these stubs satisfy the
 * link and fail safe: _read always reports "no settings saved" (0), so
 * game/settings.c falls back to defaults, and _write silently no-ops (the
 * preference just won't persist across runs on that target yet).
 *
 * A target that supplies a real backend defines EB_HAVE_SETTINGS_BACKEND
 * (build flag) to drop these stubs. Desktop is excluded by the EB_EMBEDDED
 * guard (it links the real SDL backend), so this TU is empty on desktop.
 */
#include "platform/platform.h"

#if defined(EB_EMBEDDED) && !defined(EB_HAVE_SETTINGS_BACKEND)

size_t platform_settings_read(void *dst, size_t size) {
    (void)dst;
    (void)size;
    return 0;
}

bool platform_settings_write(const void *src, size_t size) {
    (void)src;
    (void)size;
    return false;
}

#endif /* EB_EMBEDDED && !EB_HAVE_SETTINGS_BACKEND */
