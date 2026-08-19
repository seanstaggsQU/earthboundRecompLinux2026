#ifndef PLATFORM_H
#define PLATFORM_H

#include "core/types.h"

/*
 * Porting Guide: Input Mapping
 *
 * EarthBound uses 8 SNES action buttons, but most have redundant functions.
 * The game checks button *groups* for most actions (see PAD_CONFIRM, PAD_CANCEL,
 * etc. in pad.h). Ports with fewer buttons should prioritize mapping:
 *
 * REQUIRED (minimum playable):
 *   D-pad       — Movement and menu navigation
 *   PAD_A       — Confirm selection, open command window, interact
 *   PAD_B       — Cancel, back, show HP/PP window on overworld
 *
 * RECOMMENDED:
 *   PAD_X       — Toggle town map (unique function, no equivalent)
 *   PAD_START   — Start game from title screen (only use; could be combined)
 *   PAD_L       — "Check/talk to" shortcut on overworld (like A but tries talk first)
 *
 * OPTIONAL (cosmetic or redundant):
 *   PAD_SELECT  — Duplicate of B (for one-handed play)
 *   PAD_R       — Rings bicycle bell (cosmetic sound effect only)
 *   PAD_Y       — No function in normal gameplay
 *
 * For platforms with limited buttons, consider mapping unused physical buttons
 * to PAD_L (for check/talk) rather than PAD_Y or PAD_R.
 */

/* Global mode flags (set before init, read by platform backends) */
extern bool platform_headless;    /* --headless: no window, no frame timing */
extern bool platform_skip_intro;  /* --skip-intro: skip to overworld for debugging */
extern int platform_max_frames;   /* --frames N: quit after N frames (0=unlimited) */
/* --windowed: open a small resizable window instead of auto-fullscreening at
 * the detected display resolution. SDL/unix-only (embedded ports have no
 * concept of a resizable desktop window); other backends never read this. */
extern bool platform_force_windowed;

/*
 * Video — scanline-based rendering
 *
 * The PPU renders one scanline at a time. Each frame follows this sequence:
 *   1. platform_video_begin_frame()       — prepare for a new frame
 *   2. platform_video_send_scanline(y, p) — called for y=0..height-1 with RGBA pixels
 *   3. platform_video_end_frame()         — present the completed frame
 *
 * platform_video_get_framebuffer() returns a pointer to the full frame pixel
 * buffer (for desktop backends that texture-upload), or NULL on embedded
 * targets that stream scanlines directly to the display.
 *
 * platform_render_frame() encapsulates the full begin/render/end sequence.
 * On single-core platforms it calls begin_frame, ppu_render_frame, end_frame.
 * Dual-core platforms override this to distribute scanline rendering across
 * cores (e.g., even/odd scanline interleaving with a ring buffer).
 * The fps_overlay_cb, if non-NULL, is called for each scanline to stamp
 * the FPS overlay text before output.
 */
typedef void (*scanline_stamp_cb_t)(int y, pixel_t *pixels);
bool platform_video_init(void);
void platform_video_shutdown(void);
void platform_video_begin_frame(void);
void platform_video_send_scanline(int y, const pixel_t *pixels);
pixel_t *platform_video_get_framebuffer(void);
void platform_video_end_frame(void);
void platform_video_set_vsync(bool enabled);
/* Overworld FOV/zoom cycle (R3, see game_main.c's host_process_frame()).
 * The desktop build always renders the full EB_VIEWPORT_WIDTH x HEIGHT
 * canvas internally (see port/unix/CMakeLists.txt); this only changes how
 * much of it gets presented -- it's purely a presentation-layer crop, never
 * a change to game/camera/entity logic, which always runs as if the full
 * canvas were showing regardless of mode. EB_ZOOM_OFF (default) shows the
 * same EB_DEFAULT_WIDTH x SNES_HEIGHT footprint every scene already used
 * before this toggle existed; EB_ZOOM_OUT reveals more of the canvas
 * (adapting to the display's aspect ratio -- see sdl2_video.c); EB_ZOOM_IN
 * crops tighter than the default footprint instead, magnifying the view. */
typedef enum {
    EB_ZOOM_OFF = 0,
    EB_ZOOM_OUT = 1,
    EB_ZOOM_IN  = 2,
} EbZoomMode;
void platform_video_set_zoom(EbZoomMode mode);
/* Atmosphere-FX suppression override (Light Shafts, Color Grading -- see
 * game_main.c's host_process_frame(), which scans the mode stack for
 * GAME_MODE_TITLE_SCREEN/GAME_MODE_FILE_MENU the same way it already scans
 * for GAME_MODE_BATTLE/GAME_MODE_TOWN_MAP above). A per-frame *override* of
 * the player's Config setting, not a change to it -- engine_experimental_
 * visuals (settings.h) stays whatever the player chose; this just forces
 * both off on screens they shouldn't touch (the title screen's logo/flash
 * art and the file-select slots were never art-directed with either in
 * mind, and a wrong-looking permanent effect there -- unlike a battle/PSI
 * flash the player leaves in a few seconds -- would be the first and last
 * thing every session shows). Desktop-only, like the setting itself -- see
 * apply_light_shafts()/apply_color_grade() in sdl2_video.c, the only
 * implementation, mirroring platform_video_set_zoom() just above (also
 * unix-only, no embedded stub). */
void platform_video_set_fx_suppressed(bool suppressed);
/* Depth of Field suppression override -- a superset of the above: DoF is
 * ALSO suppressed during battle/Town Map (their own separate full-screen
 * layouts, same reasoning as the zoom-reset case above) and any time a
 * text/menu window is open, since even DoF's small blur radius can soften
 * dialogue text right at the screen edges where windows are usually
 * positioned. Computed independently in host_process_frame() (checked
 * there, not derived from the flag above, since none of DoF's extra
 * conditions apply to Light Shafts/Color Grading). */
void platform_video_set_dof_suppressed(bool suppressed);
void platform_render_frame(scanline_stamp_cb_t fps_overlay_cb);

/* Short build version string ("v1.2.1", "dev", ...) for display on the
 * title screen / file-select (see version_overlay_stamp_scanline(),
 * game_main.c) -- never NULL, but may be "" if none is available.
 * Desktop (port/unix): the same git-describe string the self-updater
 * compares releases against (main.c, generated/version.h) -- available
 * unconditionally, independent of whether the updater itself is
 * configured (EB_UPDATER_ENABLED). Embedded: always "" (no build-time
 * git-describe step exists there yet) -- see updater_backend_stub.c. */
const char *platform_get_version_string(void);

/*
 * Input
 *
 * Pad state is a uint16_t bitmask of SNES joypad buttons (see pad.h).
 * Each bit corresponds to one button — e.g. PAD_A (0x0080), PAD_B (0x8000).
 * Game logic tests buttons via groups like PAD_CONFIRM (A|L) and PAD_CANCEL
 * (B|SELECT), so ports can map physical buttons to whichever SNES buttons
 * make sense for the hardware.
 *
 * Aux state is a uint16_t bitmask of non-game buttons (debug, turbo, etc.).
 * Ports report raw level state (bit set = button currently held). Edge
 * detection and toggle logic are handled centrally in game_main.c, so
 * ports never need to debounce or track previous state for aux buttons.
 *
 * platform_input_poll() is called once per frame before any pad/aux reads.
 * It must sample hardware and update internal state so that get_pad(),
 * get_pad_new(), and get_aux() return consistent values for the frame.
 */

/* Aux button bitmask — non-game actions (debug, turbo, etc.) */
#define AUX_DEBUG_DUMP   (1 << 0)   /* F1: dump PPU state */
#define AUX_VRAM_DUMP    (1 << 1)   /* F2: dump VRAM as image */
#define AUX_FPS_TOGGLE   (1 << 2)   /* F3: toggle FPS overlay */
#define AUX_ZOOM_TOGGLE  (1 << 3)   /* R3: cycle the overworld FOV/zoom mode (see game_main.c) */
#define AUX_FAST_FORWARD (1 << 4)   /* Tab: toggle 4x speed */
#define AUX_DEBUG_TOGGLE (1 << 5)   /* `: toggle debug mode */
#define AUX_SAVESTATE    (1 << 6)   /* F6: request a torn-safe savestate capture */
#define AUX_LOAD_STATE   (1 << 7)   /* F7: restore the last savestate */

bool platform_input_init(void);
void platform_input_shutdown(void);
void platform_input_poll(void);                       /* sample hardware — call once per frame */
uint16_t platform_input_get_pad(void);                /* bitmask of currently held buttons (pad.h PAD_* flags) */
uint16_t platform_input_get_pad_new(void);            /* bitmask of buttons pressed this frame (not held last frame) */
uint16_t platform_input_get_aux(void);                /* bitmask of currently held aux buttons (AUX_* flags) */
bool platform_input_quit_requested(void);             /* user wants to exit (e.g. window close, Escape) */
void platform_request_quit(void);                     /* programmatic quit (e.g. from game menu) */

/*
 * Audio
 *
 * The audio subsystem runs the SPC700/DSP emulator on a separate thread
 * (desktop) or is disabled entirely (embedded without audio hardware).
 * platform_audio_lock/unlock() guard shared state between the audio
 * callback thread and the main game thread (e.g. when writing APU ports).
 */
bool platform_audio_init(void);
void platform_audio_shutdown(void);
void platform_audio_lock(void);                       /* acquire audio mutex (pairs with unlock) */
void platform_audio_unlock(void);

/*
 * MSU1 audio — optional, off unless a pack is loaded
 *
 * MSU1 is a hardware trick real SNES romhacks use to stream CD-quality PCM
 * music from an SD card/flash cart, replacing the SPC700's chiptune output
 * per-track. This port has no such hardware constraint, but community MSU1
 * packs (PCM files named <pack>-N.pcm, N = the same music track ID this
 * port already uses) are a real, existing asset format worth being able to
 * play. change_music() calls platform_audio_msu_play() before sending the
 * SPC700 the play-track command; if a pack is loaded and covers this
 * track, the platform streams that PCM instead and the caller skips the
 * SPC700 command (sound effects still go through the SPC700/DSP
 * emulation as normal -- only the music sequence trigger is skipped, so
 * this doesn't need any per-channel muting inside the emulated APU).
 * No-op (always returns false) when no pack is loaded, e.g. on ports that
 * never call platform_audio_msu_load(). */
bool platform_audio_msu_play(uint16_t track_id);  /* true if this track is covered by the loaded pack */
void platform_audio_msu_stop(void);               /* stop any currently-streaming MSU track */

/*
 * Save data — persistent storage
 *
 * The game calls platform_save_read/write with byte offsets and sizes
 * into a flat SAVE_FILE_SIZE byte buffer (7680 bytes = 3 slots × 2 copies
 * × 1280 bytes). On desktop this is a file; on SNES it's SRAM; on
 * embedded it might be flash.
 *
 * platform_save_init() is called once at startup. Returns false if the
 * storage backend can't be initialized.
 *
 * platform_save_read() returns the number of bytes actually read (0 if
 * no save exists). platform_save_write() returns true on success.
 */
bool platform_save_init(void);
size_t platform_save_read(void *dst, size_t offset, size_t size);
bool platform_save_write(const void *src, size_t offset, size_t size);

/*
 * Engine settings — small persistent C-port preferences (this port's own
 * addition; NOT part of the original ROM's SRAM save format, and
 * deliberately kept separate from platform_save_* above, which mirrors the
 * real cartridge's 7680-byte battery SRAM byte-for-byte). Things like the
 * sprint-speed toggle live here instead: they're an engine/device
 * preference that should survive independent of which save slot/file is
 * active, not game save data.
 *
 * Whole-blob read/write (no offset) since the settings struct is tiny --
 * see game/settings.h for its shape. platform_settings_read() returns the
 * number of bytes actually read (0 = no settings file yet, e.g. first run).
 * On desktop this is a small file; embedded targets get a safe no-op stub
 * (src/platform/settings_backend_stub.c) until a real backend exists,
 * exactly like platform_savestate_* above.
 */
size_t platform_settings_read(void *dst, size_t size);
bool platform_settings_write(const void *src, size_t size);

/*
 * Savestate storage — large suspend/resume snapshots (build-order item #5).
 *
 * Distinct from platform_save_* (the 7680-byte battery SRAM). A savestate is the
 * full run-to-completion snapshot (~139 KiB) used for power-off suspend/resume.
 * Two ping-pong SLOTS provide crash-safety: a write targets the inactive slot and
 * is durable only after _commit(), so a power loss mid-write leaves the prior slot
 * intact. The embedded firmware power-off handler requests a capture
 * (host_request_capture) and holds the power rail until the root boundary's
 * state_dump_save_slots() — which calls _begin/_write/_commit — returns. On desktop
 * the slots are two files; on embedded they are two flash regions.
 *
 * Offset/slot-addressed, caller-owned buffers, no malloc (same contract as
 * platform_save_*):
 *   _begin(slot)              start a fresh, truncating write to `slot`
 *   _write(slot,off,src,size) write `size` bytes at `off` within the slot
 *   _commit(slot)             flush the slot durably (fsync / flash commit)
 *   _read(slot,off,dst,size)  random read; returns bytes actually read (0 = absent)
 */
#define SAVESTATE_SLOTS 2
bool   platform_savestate_begin(int slot);
bool   platform_savestate_write(int slot, size_t offset, const void *src, size_t size);
bool   platform_savestate_commit(int slot);
size_t platform_savestate_read(int slot, size_t offset, void *dst, size_t size);

/*
 * Suspend/resume audio *output* around the root-boundary save/load I/O (the
 * ~0.6-1 s of blocking slot reads/writes during which the game loop, and thus
 * the port's audio producer, is stalled). A port whose audio is pulled by a
 * DMA/callback that keeps running while the loop is blocked (e.g. the G&W SAI
 * ring) will otherwise drain its buffer and then repeat/underrun for the rest
 * of the stall. freeze(true) is called once immediately before the blocking
 * state_dump, freeze(false) once after it returns (both branches). It is only
 * ever called at the root boundary, where the producer is quiescent, so a port
 * may safely gate its output callback here without deadlocking the producer.
 * Optional: the default (weak) implementation is a no-op for ports that don't
 * need it. Must be balanced and reentrancy-free (never nested).
 */
void platform_savestate_freeze_audio(bool freeze);

/*
 * Transient scratch RAM for the savestate (de)compressor — the LZ window, the
 * tamp working struct, and the compressed-byte I/O staging buffer. The payload
 * is stored as a tamp stream (see state_dump.c); compress (save) and decompress
 * (load) both run only at the root boundary, where the port can lend a large
 * already-allocated buffer it isn't using (the G&W lends its idle third
 * framebuffer; desktop returns a static buffer). Returns the buffer and writes
 * its byte size to *out_bytes. The caller treats it as volatile scratch and owns
 * no state across calls. Must be >= 512 bytes; a bigger region just enables
 * chunkier I/O staging. Never NULL.
 */
void  *platform_savestate_scratch(size_t *out_bytes);

/* Debug dumps (desktop: write files to debug/; embedded: no-op) */
void platform_debug_dump_ppu(const pixel_t *framebuffer);
void platform_debug_dump_vram_image(void);

/*
 * Self-update — desktop (port/unix) builds only, and only when built with a
 * private release feed configured (EB_UPDATER_REPO/EB_UPDATER_TOKEN CMake
 * cache vars; both empty = feature entirely absent from the binary). NOT
 * part of the universal cross-port contract the way video/input/audio are
 * — see docs/porting-guide.md's "No File I/O" precedent, which treats
 * persistent-storage backends the same way (real on desktop, safe no-op
 * stub elsewhere: src/platform/updater_backend_stub.c mirrors
 * src/platform/settings_backend_stub.c's pattern exactly). Callers never
 * need '#ifdef EB_EMBEDDED' at the call site — platform_update_supported()
 * just reports false and every other call becomes a no-op.
 *
 * Fully non-blocking, by design: the mode-stack game loop steps one frame
 * at a time with no blocking calls anywhere, so a synchronous HTTP request
 * here would freeze rendering. _check_start() and _download_start() spawn a
 * background thread and return immediately; the caller (src/intro/
 * update_screen.c) calls _poll() once per frame to read current status —
 * it never blocks, even mid-download.
 */
typedef enum {
    EB_UPDATE_IDLE = 0,
    EB_UPDATE_CHECKING,      /* background thread is contacting the release feed */
    EB_UPDATE_UP_TO_DATE,    /* checked: EB_VERSION_STRING already matches the latest release tag */
    EB_UPDATE_AVAILABLE,     /* checked: latest_version names a newer release than this build */
    EB_UPDATE_DOWNLOADING,   /* download + checksum verify + install all happen on the thread; progress_percent updates */
    EB_UPDATE_DONE,          /* installed and verified; a relaunch has already been requested */
    EB_UPDATE_ERROR,         /* any failure at any stage; the running binary was never touched -- see error_message */
    EB_UPDATE_UNSUPPORTED,   /* no backend for this build (embedded target, or feed not configured at build time) */
} EbUpdateStatus;

typedef struct {
    EbUpdateStatus status;
    char latest_version[32];   /* valid once status >= EB_UPDATE_AVAILABLE; git-describe-length tags, not free text */
    int  progress_percent;     /* valid during EB_UPDATE_DOWNLOADING, 0-100 */
    /* Deliberately short: this struct is embedded by value in ModeState
     * (mode_stack.h's UpdateCheckState), a union shared by every mode-stack
     * level (MODE_STACK_MAX=24) -- no other member there comes close to a
     * 256-byte string, and a generously-sized error buffer here would bloat
     * every single level's storage (and the savestate ABI) just to cover a
     * message this port's own small WINDOW_UPDATE_CHECK (24 tiles wide)
     * couldn't display in full anyway. Backends must produce a short,
     * pre-truncated message, not rely on this buffer to hold one. */
    char error_message[64];    /* valid when status == EB_UPDATE_ERROR */
} EbUpdateProgress;

bool platform_update_supported(void);        /* true only if this build has a real backend configured */
void platform_update_check_start(void);      /* non-blocking; kicks off a background version check */
void platform_update_download_start(void);   /* non-blocking; only valid once poll() reports EB_UPDATE_AVAILABLE */
void platform_update_poll(EbUpdateProgress *out); /* call once per frame; never blocks */

/*
 * Timer — frame pacing
 *
 * Call platform_timer_frame_start() at the top of the game loop and
 * platform_timer_frame_end() at the bottom. frame_end() sleeps or
 * busy-waits to maintain ~60 fps (NTSC timing).
 *
 * platform_timer_ticks() / ticks_per_sec() provide a monotonic clock
 * for measuring elapsed time. get_fps_tenths() returns a smoothed FPS
 * value multiplied by 10 (e.g. 600 = 60.0 fps) for the FPS overlay.
 */
bool platform_timer_init(void);
void platform_timer_shutdown(void);
void platform_timer_frame_start(void);
void platform_timer_frame_end(void);
void platform_timer_update_fps(void);               /* update IIR FPS filter (call every frame) */
void platform_timer_sleep_until(uint64_t deadline);  /* sleep until the given tick value */
uint64_t platform_timer_ticks(void);
uint64_t platform_timer_ticks_per_sec(void);
uint32_t platform_timer_get_fps_tenths(void);

/*
 * Optional host-paced frame-skip hook.
 *
 * When EB_HOST_PACED_FRAMESKIP is defined, host_process_frame() delegates
 * the per-frame render/skip decision to the platform instead of using its own
 * deadline-based dynamic frame-skip. Platforms that pace the game loop against
 * an external clock implement this — e.g. the Game & Watch port locks the loop
 * to its audio DMA: platform_timer_should_render() runs the shared retro-go
 * frame-loop controller (which records this frame's skip state so the matching
 * platform_timer_sleep_until() can rate-match and phase-lock audio), and
 * returns whether this frame should be rendered. Called once per frame, at the
 * top of host_process_frame(), in place of the built-in skip logic.
 */
#ifdef EB_HOST_PACED_FRAMESKIP
bool platform_timer_should_render(void);
#endif

#endif /* PLATFORM_H */
