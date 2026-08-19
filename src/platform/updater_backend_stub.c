/*
 * Default no-op self-update backend.
 *
 * Mirrors src/platform/settings_backend_stub.c's convention exactly, for
 * the same reason: platform_update_* (platform.h) is target-specific and
 * not universally available -- desktop (port/unix) has a real
 * implementation (platform/sdl2_updater.c) when built with a private
 * release feed configured; an embedded target has no self-update story at
 * all yet.
 *
 * platform_update_supported() reports false and every other call is a
 * no-op, so callers (src/intro/update_screen.c, and the "Check for
 * Updates" row's platform_update_supported() gate in file_select.c) never
 * need '#ifdef EB_EMBEDDED' at the call site.
 *
 * Desktop is excluded by the EB_EMBEDDED guard -- sdl2_updater.c provides
 * platform_update_* there instead (whether or not a release feed is
 * actually configured; see that file's own EB_UPDATER_ENABLED branch for
 * the desktop-but-unconfigured case), so this TU is empty on desktop.
 */
#include "platform/platform.h"

#if defined(EB_EMBEDDED)

bool platform_update_supported(void) {
    return false;
}

void platform_update_check_start(void) {
}

void platform_update_download_start(void) {
}

void platform_update_poll(EbUpdateProgress *out) {
    *out = (EbUpdateProgress){0};
    out->status = EB_UPDATE_UNSUPPORTED;
}

/* platform_get_version_string() lives here too, despite the "updater"
 * filename -- same reasoning as everything else in this file: an
 * embedded target has no build-time git-describe step (or self-update
 * story) at all yet. Desktop's real implementation is in port/unix/
 * main.c, unconditionally (not gated behind EB_UPDATER_ENABLED). */
const char *platform_get_version_string(void) {
    return "";
}

#endif /* EB_EMBEDDED */
