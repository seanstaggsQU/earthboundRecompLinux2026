/*
 * Game & Watch debug platform stub.
 *
 * Real implementation lives in the firmware repo under
 * Core/Src/porting/earthbound/ — but on this target the debug dumps
 * (PPU snapshots, VRAM-as-BMP) are no-ops. There's no filesystem path
 * on the device to write to, and the SD card driver is busy serving
 * asset loads.
 *
 * If a developer ever wants on-device dumps, the launcher's
 * make_dump_screenshot target (Makefile.common) and the SD card's
 * /debug/ directory are the natural integration points.
 */

#include "platform/platform.h"

void platform_debug_dump_ppu(const pixel_t *framebuffer) {
    (void)framebuffer;
}

void platform_debug_dump_vram_image(void) {
}

void platform_debug_mark_screenshot(const pixel_t *framebuffer) {
    (void)framebuffer;
}

/* No filesystem path meant for player-visible text output on this target
 * (the SD card driver is busy serving asset loads, same reasoning as the
 * dumps above) -- LOG_WARN/LOG_TRACE are already no-ops under EB_EMBEDDED
 * (core/log.h) regardless, so there is nothing for this to redirect. */
void platform_log_set_enabled(bool enabled) {
    (void)enabled;
}
