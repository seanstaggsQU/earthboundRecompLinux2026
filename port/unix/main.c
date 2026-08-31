/* Keep plain int main() as the real entry point on every platform, including
 * Windows -- without this, SDL.h's <SDL_main.h> `#define main SDL_main`
 * kicks in on Windows/MinGW and expects to link against SDL2main's WinMain
 * shim, which this port doesn't link (no GUI-subsystem console juggling
 * needed; earthbound.exe is a normal console-subsystem executable). */
#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#include <unistd.h>
#include <libgen.h>
#elif defined(__linux__)
#include <unistd.h>
#include <libgen.h>
#elif defined(_WIN32)
#include <windows.h>
#include <direct.h>    /* _mkdir, for migrate_legacy_saves_to_saves_folder() below */
#endif
#include <sys/stat.h>  /* mkdir/stat -- mingw provides this on Windows too, just needs the include */
#ifdef _WIN32
#define MKDIR(path) _mkdir(path)
#else
#define MKDIR(path) mkdir(path, 0755)
#endif

#include "platform/platform.h"
#include "embedded_helper.h"
#include "game_main.h"
#include "game/audio.h"
#include "game/game_state.h"
#include "game/settings.h"
#include "core/log.h"
#include "core/state_dump.h"
#include "version.h"    /* EB_VERSION_STRING -- generated, see CMakeLists.txt.
                          * Unconditionally generated (not gated behind
                          * EB_UPDATER_ENABLED the way sdl2_updater.c's own
                          * #include of it is), so platform_get_version_string()
                          * lives here rather than there -- it needs to work
                          * even when the updater itself isn't configured. */

const char *platform_get_version_string(void) {
    return EB_VERSION_STRING;
}

#ifdef EB_RUNTIME_ASSETS
#include "data/runtime_assets.h"
#include "data/rom_extract.h"
#else
#include "data/pak_export.h"
#endif

/* Unix-specific: set save file path (defined in sdl2_save.c) */
void platform_save_set_path(const char *path);
#include "verify/verify.h"

/* Unix-specific: dump the window's actual composited output on the next
 * present (defined in sdl2_video.c) */
void platform_video_request_screenshot(const char *path);

#ifdef EB_ENABLE_AUDIO
/* Unix-specific: point at an MSU1 pack directory (defined in msu_audio.c) */
void platform_audio_msu_load(const char *dir, const char *pack_name);
bool platform_audio_msu_autodetect_name(const char *dir, char *out_name, size_t out_size);
#endif

/* chdir() into the directory the running executable actually lives in, so
 * every relative path this port uses (msu/, settings.dat, savestate.bin.N,
 * earthbound.srm, a --log-file default, ...) resolves relative to "beside
 * the binary" regardless of how the process was launched -- a shell already
 * cd'd there (today's only reliable case), a desktop icon/shortcut with an
 * unrelated "start in" folder, or (the case that motivated this) a
 * double-clicked macOS .app bundle, which Finder always launches with CWD
 * set to the user's home directory, not Contents/MacOS/ where the real
 * binary and its bundled assets live. Windows already sets CWD correctly
 * for a plain double-clicked .exe, and every Linux launch so far has relied
 * on the caller's CWD (a shell, or a .desktop file's Path= key) -- this
 * makes that no longer a requirement on any of the three, without changing
 * behavior for anyone who already cd's there themselves (chdir to the
 * directory you're already in is a no-op). Best-effort: on any failure this
 * silently leaves CWD as inherited, matching every build before this fix
 * existed, so it can't turn a working launch into a broken one. */
static void chdir_to_executable_dir(void) {
    char path[4096];
#ifdef __APPLE__
    uint32_t size = sizeof(path);
    if (_NSGetExecutablePath(path, &size) != 0)
        return; /* buffer too small (shouldn't happen at 4096) -- leave CWD alone */
    char real[4096];
    if (!realpath(path, real))
        return;
    if (chdir(dirname(real)) != 0)
        return;
#elif defined(__linux__)
    ssize_t len = readlink("/proc/self/exe", path, sizeof(path) - 1);
    if (len <= 0)
        return;
    path[len] = '\0';
    if (chdir(dirname(path)) != 0)
        return;
#elif defined(_WIN32)
    DWORD len = GetModuleFileNameA(NULL, path, (DWORD)sizeof(path));
    if (len == 0 || len >= sizeof(path))
        return;
    /* Trim the filename back to its directory (no libgen.h on MinGW's
     * default headers here -- do it inline instead of adding a dependency). */
    for (DWORD i = len; i-- > 0; ) {
        if (path[i] == '\\' || path[i] == '/') {
            path[i] = '\0';
            SetCurrentDirectoryA(path);
            return;
        }
    }
#endif
}

/* Shared by --log-file and platform_log_set_enabled() (the Config menu's
 * "Logging" row, see settings.h's engine_logging) -- both just want
 * stdout+stderr pointed at a file instead of the (possibly nonexistent,
 * e.g. launched from a desktop icon) console. */
static bool log_redirected = false; /* guards against a second freopen() clobbering the first target */

static void redirect_output_to_log(const char *path) {
    if (!freopen(path, "w", stdout) || !freopen(path, "a", stderr)) {
        fprintf(stderr, "Warning: could not redirect output to %s: %s\n", path, strerror(errno));
        return;
    }
    setvbuf(stdout, NULL, _IOLBF, 0); /* line-buffered: readable if the game hangs/crashes mid-run */
    setvbuf(stderr, NULL, _IOLBF, 0);
    log_redirected = true;
}

/* platform_log_set_enabled() -- see platform.h for the full contract
 * (desktop-only, one-way, safe to call every frame). Fixed filename/
 * location (next to the executable, same as platform_debug_mark_screenshot()'s
 * F4 screenshots) since a Config-menu toggle has no place to type a path,
 * unlike --log-file. If --log-file already redirected to a different path
 * this run, log_redirected is already true and this is a no-op -- an
 * explicit command-line choice wins over the persisted setting. */
void platform_log_set_enabled(bool enabled) {
    if (!enabled || log_redirected) return;
    redirect_output_to_log("eb_debug.log");
}

/* Cleanup handler registered with atexit() — runs on any exit() call. */
static void platform_cleanup(void) {
    platform_timer_shutdown();
    platform_audio_shutdown();
    platform_input_shutdown();
    platform_video_shutdown();
    SDL_Quit();
}

/* Self-update relaunch staging. The updater (sdl2_updater.c) calls
 * platform_update_stage_relaunch() after it has already swapped the new
 * binary into place on disk, then requests a normal quit -- the actual
 * execv() happens here, as an atexit handler, not inline in the updater's
 * own code, for one specific reason: it must run *after* platform_cleanup()
 * (SDL_Quit, closing the audio device, tearing down the window) has fully
 * torn down SDL, never while a window/audio device is still open. main()
 * itself has no statement that runs after cleanup -- platform_cleanup() is
 * itself only reached via atexit(), triggered by the loop's normal `return
 * 0` at the bottom of main() -- so ordering has to be expressed through
 * atexit()'s own LIFO (last-registered-runs-first) contract instead: this
 * handler is registered *before* atexit(platform_cleanup) below, so it
 * naturally runs *after* platform_cleanup on the way out, no matter which
 * exit()/return path is taken (normal quit, --frames limit, any of the
 * exit() calls in the --selftest-savestate branch, etc.) -- all of them.
 *
 * g_relaunch_pending stays false on every ordinary run (the overwhelming
 * common case), so this is a no-op until the updater actually sets it. */
static bool g_relaunch_pending = false;
static char g_relaunch_exe_path[4096];

static void maybe_relaunch_atexit(void) {
    if (!g_relaunch_pending)
        return;
    char *relaunch_argv[] = { g_relaunch_exe_path, NULL };
    execv(g_relaunch_exe_path, relaunch_argv);
    /* execv() only returns on failure. Nothing more we can do here -- SDL is
     * already fully torn down by this point -- so this is the same
     * plain-stderr fallback the rest of this file uses for init failures. */
    fprintf(stderr, "Update installed, but relaunching %s failed: %s\n"
                     "Please start the game again manually.\n",
            g_relaunch_exe_path, strerror(errno));
}

/* Everything below, down to draw_text5x7(), backs the first-launch setup
 * helper's progress window -- only reachable from the #ifdef
 * EB_RUNTIME_ASSETS block further down, so guard the definitions
 * themselves too rather than leaving them as dead code in an
 * EB_RUNTIME_ASSETS=OFF (compile-time-embedded) build. */
#ifdef EB_RUNTIME_ASSETS

/* Runs the bundled setup helper (a real, possibly multi-minute ebtools
 * extract/pack-all/pack-assets pass) on a background thread so the main
 * thread can keep the OS's "still alive" window pump going instead of
 * looking hung -- see the SDL_CreateThread call below. The helper's exit
 * status IS captured (g_setup_helper_exit_status below) so a crash partway
 * through -- confirmed live: a donor ROM whose dialogue hits a jump target
 * outside every mapped text block makes the bundled Python helper's own
 * text decoder throw and abort the whole run -- can be told apart from a
 * clean run and trigger the native rom_extract_build_pak() fallback right
 * after, instead of just leaving assets_result as EB_ASSETS_MISSING and
 * telling a player who supplied a perfectly good ROM "couldn't find your
 * game data". */
static SDL_atomic_t g_setup_helper_done;
static SDL_atomic_t g_setup_helper_exit_status;

static int run_setup_helper_thread_fn(void *cmd_ptr) {
    int status = system((const char *)cmd_ptr);
    SDL_AtomicSet(&g_setup_helper_exit_status, status);
    SDL_AtomicSet(&g_setup_helper_done, 1);
    return 0;
}

/* Tiny built-in 5x7 pixel font (this project's own, not derived from the
 * ROM or any external font file) for the setup-progress window's message
 * below -- the game's real font is itself one of the assets being
 * generated at this point, so nothing else is available yet. Covers only
 * what that message actually uses: A-Z, space, '.', ','. Each row is the
 * low 5 bits of the byte, MSB-of-those-5 = leftmost column. */
typedef struct {
    char c;
    uint8_t rows[7];
} Glyph5x7;

static const Glyph5x7 g_font5x7[] = {
    {' ', {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}},
    {'.', {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04}},
    {',', {0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x08}},
    {'A', {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
    {'B', {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E}},
    {'C', {0x0F, 0x10, 0x10, 0x10, 0x10, 0x10, 0x0F}},
    {'D', {0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E}},
    {'E', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}},
    {'F', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10}},
    {'G', {0x0F, 0x10, 0x10, 0x13, 0x11, 0x11, 0x0F}},
    {'H', {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
    {'I', {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x1F}},
    {'J', {0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C}},
    {'K', {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}},
    {'L', {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F}},
    {'M', {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11}},
    {'N', {0x11, 0x19, 0x15, 0x15, 0x13, 0x11, 0x11}},
    {'O', {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}},
    {'P', {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}},
    {'Q', {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D}},
    {'R', {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11}},
    {'S', {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}},
    {'T', {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}},
    {'U', {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}},
    {'V', {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04}},
    {'W', {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A}},
    {'X', {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11}},
    {'Y', {0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04}},
    {'Z', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F}},
};

static const uint8_t *find_glyph5x7(char c) {
    if (c >= 'a' && c <= 'z') {
        c = (char)(c - 'a' + 'A'); /* fold lowercase -- message is authored all-caps anyway */
    }
    for (size_t i = 0; i < sizeof(g_font5x7) / sizeof(g_font5x7[0]); i++) {
        if (g_font5x7[i].c == c) {
            return g_font5x7[i].rows;
        }
    }
    return NULL; /* unsupported char -- drawn as a blank cell */
}

/* Draws one line of text at (x, y) in surf's own pixel format, each font
 * pixel scaled up to a `scale`x`scale` filled square. Returns the pixel
 * width consumed (6*scale per glyph cell, monospace). */
static int draw_text5x7(SDL_Surface *surf, int x, int y, const char *text, int scale, Uint32 color) {
    int cursor_x = x;
    for (const char *p = text; *p; p++) {
        const uint8_t *rows = find_glyph5x7(*p);
        if (rows) {
            for (int row = 0; row < 7; row++) {
                for (int col = 0; col < 5; col++) {
                    if (rows[row] & (1 << (4 - col))) {
                        SDL_Rect px = { cursor_x + col * scale, y + row * scale, scale, scale };
                        SDL_FillRect(surf, &px, color);
                    }
                }
            }
        }
        cursor_x += 6 * scale;
    }
    return cursor_x - x;
}

#endif /* EB_RUNTIME_ASSETS */

/* Unix-specific: called by the updater (sdl2_updater.c) once it has
 * verified and swapped the new binary into place, right before it requests
 * a quit. Mirrors platform_save_set_path()'s existing pattern of a
 * port/unix-local setter forward-declared in main.c rather than added to
 * the shared platform.h contract -- this is deliberately not part of the
 * cross-port platform interface (see docs/porting-guide.md's "No File I/O"
 * precedent: self-update is desktop-only infra, not a universal capability). */
void platform_update_stage_relaunch(const char *new_exe_path) {
    strncpy(g_relaunch_exe_path, new_exe_path, sizeof(g_relaunch_exe_path) - 1);
    g_relaunch_exe_path[sizeof(g_relaunch_exe_path) - 1] = '\0';
    g_relaunch_pending = true;
}

/* Copies old_path into new_path if new_path doesn't already exist and
 * old_path does -- leaves old_path in place untouched either way (never
 * moves/deletes), so it stays available as an extra safety copy. Never
 * overwrites an existing new_path: idempotent across repeated launches,
 * and safe even if the player has already made new progress at the
 * migrated location by the time this runs again. Best-effort: any I/O
 * failure partway through just leaves old_path as the intact source of
 * truth (this function's whole point is to avoid data loss, not risk
 * causing it). */
static void copy_file_if_missing(const char *old_path, const char *new_path) {
    FILE *dst_check = fopen(new_path, "rb");
    if (dst_check) { fclose(dst_check); return; /* already migrated */ }

    FILE *src = fopen(old_path, "rb");
    if (!src) return; /* nothing to migrate */

    MKDIR("saves");
    FILE *dst = fopen(new_path, "wb");
    if (!dst) { fclose(src); return; }

    char buf[8192];
    size_t n;
    bool ok = true;
    while ((n = fread(buf, 1, sizeof(buf), src)) > 0) {
        if (fwrite(buf, 1, n, dst) != n) { ok = false; break; }
    }
    fclose(src);
    fclose(dst);
    if (!ok)
        remove(new_path); /* don't leave a truncated/partial copy sitting at the new path */
}

/* One-time migration: earlier versions stored save data as flat files
 * directly next to the executable (earthbound.srm, savestate.bin.0/.1).
 * This version moves the default locations into a dedicated saves/
 * subfolder -- partly for tidiness, but mainly so a mistake that touches
 * "whatever's in the game's working directory" (a self-test, a stray
 * debug write, ...) is far less likely to land on the exact same
 * easily-collided filename as the real save. Confirmed the hard way this
 * session: a self-test run directly in a live deployed game directory
 * once destroyed a real player's save because it was a flat file sitting
 * right next to everything else, with no dedicated, more-protected home.
 * See copy_file_if_missing()'s doc comment for the copy-not-move
 * semantics. Runs unconditionally at startup, before any --save override
 * is parsed or platform_save_init()/settings_load() run, using the fixed
 * legacy default paths regardless of this launch's own --save value --
 * the point is making sure a FUTURE non-overridden launch finds its data
 * already migrated, not reacting to this specific invocation's flags. */
static void migrate_legacy_saves_to_saves_folder(void) {
    copy_file_if_missing("earthbound.srm", "saves/earthbound.srm");
    copy_file_if_missing("savestate.bin.0", "saves/savestate.bin.0");
    copy_file_if_missing("savestate.bin.1", "saves/savestate.bin.1");
}

int main(int argc, char *argv[]) {
    chdir_to_executable_dir(); /* before anything below touches a relative path */
    migrate_legacy_saves_to_saves_folder(); /* before any --save override or save/settings read */
    SDL_SetMainReady(); /* pairs with SDL_MAIN_HANDLED above */
    const char *verify_rom_path = NULL;
    bool savestate_selftest = false;
    bool keyitems_selftest = false;
    bool joinlevel_selftest = false;
    bool threed_zombie_flag_check = false;
    bool threed_zombie_flag_fix = false;
    bool update_now = false; /* --update-now: drive a real check+download+install synchronously, then exit -- see its own comment below */
    bool load_state_at_boot = false; /* --load-state: resume from savestate.bin.0/.1 in CWD instead of a fresh boot */
    int dump_flags_frame = -1; /* --dump-flags N: print a hardcoded event-flag debug list on frame N */
    int dump_frame = -1; /* --dump-frame N: write screenshot.bmp (final windowed output) on frame N, then continue */
    const char *log_file_path = NULL; /* --log-file PATH: redirect stdout+stderr there (no terminal when launched via desktop icon) */
#ifdef EB_ENABLE_AUDIO
    const char *msu_dir = NULL;   /* --msu-dir PATH: directory containing <name>-N.pcm files */
    const char *msu_name = NULL;  /* --msu-name NAME: pack basename; NULL = autodetect (see below) */
#endif
#ifdef EB_RUNTIME_ASSETS
    const char *assets_path = NULL;
#else
    const char *export_pak_path = NULL; /* --export-pak PATH: write this binary's own compiled-in assets out as a pak, then exit */
#endif

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--verify") == 0 && i + 1 < argc) {
            verify_rom_path = argv[++i];
#ifdef EB_RUNTIME_ASSETS
        } else if (strcmp(argv[i], "--assets") == 0 && i + 1 < argc) {
            assets_path = argv[++i];
#else
        } else if (strcmp(argv[i], "--export-pak") == 0 && i + 1 < argc) {
            export_pak_path = argv[++i];
#endif
        } else if (strcmp(argv[i], "--update-now") == 0) {
            update_now = true;
            platform_headless = true;
        } else if (strcmp(argv[i], "--selftest-savestate") == 0) {
            /* Run the savestate save->load->save round-trip self-test and exit.
             * Implies headless so it neither opens a window nor plays the game. */
            savestate_selftest = true;
            platform_headless = true;
        } else if (strcmp(argv[i], "--selftest-keyitems") == 0) {
            /* Key Items pool feature: migration + round-trip + the
             * ShrineFox-mod-bug-class regression check. Uses save_game()/
             * load_game() (slot 0) via the .srm path, so run with a scratch
             * --save FILE. Implies headless. */
            keyitems_selftest = true;
            platform_headless = true;
        } else if (strcmp(argv[i], "--selftest-joinlevel") == 0) {
            /* Join-level scaling (Paula/Jeff/Poo scaled to Ness's current
             * level, see add_char_to_party()'s doc comment, inventory.c).
             * Pure in-memory, no save file touched. Implies headless. */
            joinlevel_selftest = true;
            platform_headless = true;
        } else if (strcmp(argv[i], "--check-threed-zombie-flag") == 0) {
            /* One-off, read-only data-repair diagnostic (not a permanent
             * CLI surface): reports FLG_THRK_BIKINIZOMBI_F_APPEAR/
             * _P_APPEAR/FLG_THRK_HOTELZOMBI_APPEAR (296/297/298) for every
             * populated save slot in the .srm at CWD (or --save FILE).
             * See cr_movement_cmd_set_event_flag()'s fix (callroutine_
             * movement.c) -- that bug meant a save made before the fix can
             * have flag 296 stuck permanently set even after the Threed
             * hotel-zombie quest was completed, which this exists to
             * confirm before --check-threed-zombie-flag's write-capable
             * sibling below touches anything. Implies headless, writes
             * nothing. */
            threed_zombie_flag_check = true;
            platform_headless = true;
        } else if (strcmp(argv[i], "--fix-threed-zombie-flag") == 0) {
            /* Write-capable sibling of --check-threed-zombie-flag above:
             * for every save slot where flag 296 (FLG_THRK_BIKINIZOMBI_
             * F_APPEAR) is stuck set, clears it and re-saves that slot via
             * the normal save_game() path (same checksum'd format, same
             * function real gameplay uses) -- a one-time repair for a save
             * made before cr_movement_cmd_set_event_flag()'s fix, not
             * something a fixed-going-forward save ever needs again. Back
             * up the .srm before running this. Implies headless. */
            threed_zombie_flag_fix = true;
            platform_headless = true;
        } else if (strcmp(argv[i], "--headless") == 0) {
            platform_headless = true;
        } else if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            platform_max_frames = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--save") == 0 && i + 1 < argc) {
            platform_save_set_path(argv[++i]);
#ifdef EB_ENABLE_AUDIO
        } else if (strcmp(argv[i], "--msu-dir") == 0 && i + 1 < argc) {
            msu_dir = argv[++i];
        } else if (strcmp(argv[i], "--msu-name") == 0 && i + 1 < argc) {
            msu_name = argv[++i];
#endif
        } else if (strcmp(argv[i], "--dump-frame") == 0 && i + 1 < argc) {
            dump_frame = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--load-state") == 0) {
            load_state_at_boot = true;
        } else if (strcmp(argv[i], "--log-file") == 0 && i + 1 < argc) {
            log_file_path = argv[++i];
        } else if (strcmp(argv[i], "--dump-flags") == 0 && i + 1 < argc) {
            dump_flags_frame = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--windowed") == 0) {
            platform_force_windowed = true;
        } else if (strcmp(argv[i], "--skip-intro") == 0) {
            platform_skip_intro = true;
        } else if (strcmp(argv[i], "--verbose") == 0) {
            verbose_level++;
        } else if (argv[i][0] == '-' && argv[i][1] == 'v' && argv[i][1] != '-') {
            /* Count v's: -v = 1, -vv = 2, -vvv = 3 */
            for (const char *p = &argv[i][1]; *p == 'v'; p++)
                verbose_level++;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            fprintf(stderr, "Usage: %s [--save FILE] [--headless] [--windowed] [--frames N] [--dump-frame N] [--load-state] [--log-file PATH]"
#ifdef EB_ENABLE_AUDIO
                            " [--msu-dir PATH] [--msu-name NAME]"
#endif
                            " [--verbose] [--verify ROM]"
#ifdef EB_RUNTIME_ASSETS
                            " [--assets FILE]"
#else
                            " [--export-pak FILE]"
#endif
                            " [--selftest-savestate] [--selftest-keyitems] [--selftest-joinlevel]\n",
                    argv[0]);
            return 1;
        }
    }

#ifndef EB_RUNTIME_ASSETS
    /* Writes this binary's own compiled-in assets out as a pak, byte-for-byte
     * identical to one built from a matching ROM (same source bytes, same
     * layout order -- see rom_extract_build_pak()), then exits. Used by
     * the self-updater to hand a runtime-assets build everything it needs
     * with no ROM required, and available by hand for anyone updating via
     * a fresh download instead of "Check for Updates". */
    if (export_pak_path) {
        bool ok = pak_export_write(export_pak_path);
        fprintf(stderr, "%s %s\n", ok ? "Wrote" : "Failed to write", export_pak_path);
        return ok ? 0 : 1;
    }
#endif

    /* Maintainer-only test hook: drives a real check -> download -> install
     * synchronously (the normal path is GUI-triggered from the pause/
     * file-select menu, async, polled once per frame) and exits, so the
     * real update flow can be exercised headlessly -- e.g. against a
     * pre-release via EB_UPDATER_TEST_TAG (see sdl2_updater.c), without a
     * real tester's app ever seeing it. Not documented in --help. */
    if (update_now) {
        if (!platform_update_supported()) {
            fprintf(stderr, "Updater not supported in this build.\n");
            return 1;
        }
        /* SDL_CreateThread/SDL_Delay below don't strictly require a prior
         * SDL_Init on most platforms, but every other SDL_Thread use in this
         * codebase runs after SDL_Init -- match that instead of relying on
         * an unenforced platform quirk. SDL_Init(0) initializes no
         * subsystems, just SDL's internal bookkeeping. */
        SDL_Init(0);
        EbUpdateProgress p = {0};
        platform_update_check_start();
        do {
            SDL_Delay(200);
            platform_update_poll(&p);
        } while (p.status == EB_UPDATE_IDLE || p.status == EB_UPDATE_CHECKING);

        if (p.status == EB_UPDATE_UP_TO_DATE) {
            fprintf(stderr, "Already up to date (%s).\n", p.latest_version);
            return 0;
        }
        if (p.status != EB_UPDATE_AVAILABLE) {
            fprintf(stderr, "Check failed: %s\n", p.error_message);
            return 1;
        }
        fprintf(stderr, "Update available: %s -- downloading...\n", p.latest_version);

        platform_update_download_start();
        int last_percent = -1;
        do {
            SDL_Delay(200);
            platform_update_poll(&p);
            if (p.status == EB_UPDATE_DOWNLOADING && p.progress_percent != last_percent) {
                last_percent = p.progress_percent;
                fprintf(stderr, "  %d%%\n", last_percent);
            }
        } while (p.status == EB_UPDATE_DOWNLOADING);

        if (p.status == EB_UPDATE_DONE) {
            fprintf(stderr, "Installed. Relaunching...\n");
            /* The install step already staged the relaunch (see
             * platform_update_stage_relaunch()), but it only actually fires
             * from maybe_relaunch_atexit(), normally registered via atexit()
             * much later in main() -- this early-exit path never reaches
             * that registration. Call it directly instead; it's safe here
             * because this test-driver path never initializes SDL/audio/
             * video/input (no platform_cleanup() chain to skip). */
            maybe_relaunch_atexit();
            return 0;
        }
        fprintf(stderr, "Install failed: %s\n", p.error_message);
        return 1;
    }

    /* --log-file: redirect stdout+stderr to a file. Mainly for launches with
     * no attached terminal (desktop icon, Steam shortcut) -- without this,
     * crash/error output (including LOG_WARN/FATAL, core/log.h) has nowhere
     * to go. Done as early as possible so it also catches SDL init failures. */
    if (log_file_path) {
        redirect_output_to_log(log_file_path);
    }

#ifdef EB_RUNTIME_ASSETS
    /* Load assets.pak before anything else touches ASSET_DATA/ASSET_SIZE.
     * Precedence: --assets flag, then EB_ASSETS_PAK env var, then the
     * platform-conventional data dir (see eb_runtime_assets_default_path). */
    char default_assets_path[1024];
    if (assets_path == NULL) {
        assets_path = getenv("EB_ASSETS_PAK");
    }
    if (assets_path == NULL) {
        assets_path = eb_runtime_assets_default_path(default_assets_path, sizeof(default_assets_path))
                          ? default_assets_path
                          : "assets.pak";
    }
    EbAssetLoadResult assets_result = eb_runtime_assets_load(assets_path);

    /* No pak yet? Look for a ROM sitting next to the executable (any
     * .sfc/.smc file -- matched by its own checksum/title, not by
     * filename, so it doesn't matter what the player named it), and if
     * the bundled setup helper is also there, run it silently to build
     * one. The helper is the real ebtools extract/pack-all/pack-assets
     * pipeline (see ebtools/cli/setup.py), bundled as a standalone
     * executable (no Python needed on the player's machine) -- it's the
     * authoritative pak builder, not a from-scratch reimplementation, so
     * it's guaranteed to match what a normal build produces (including
     * this project's own custom content, e.g. the naming screen's names).
     * Only bother the player if that doesn't pan out.
     *
     * found_rom_no_helper: set when a ROM was found but the helper wasn't --
     * distinct from "nothing found at all" so the error message below can
     * say exactly what's still missing instead of just "no ROM", which is
     * actively misleading in this case (a tester who updated from a
     * pre-runtime-assets build has a ROM-less folder AND no helper, since
     * the old build never shipped one and the updater still doesn't fetch
     * one -- telling them to add a ROM when they already have one, or when
     * that alone won't fix it, just sends them in circles). */
    bool found_rom_no_helper = false;
    if (assets_result == EB_ASSETS_MISSING) {
        char rom_path[4096];
        bool rom_found = rom_extract_find_rom(".", rom_path, sizeof(rom_path));
#ifdef __APPLE__
        /* chdir_to_executable_dir() above put us inside the .app bundle
         * itself (Contents/MacOS/, where the real binary lives -- see its
         * own doc comment). Two other spots are worth trying beyond that:
         *
         *  - "..": Contents/ itself. A player who right-clicks the bundle
         *    and chooses "Show Package Contents" lands here directly --
         *    dropping the ROM (and the msu folder, see the mirrored search
         *    below) right inside Contents/ keeps everything in one single
         *    .app to move around, which is the cleaner option for anyone
         *    willing to do that once. Tried before the bundle-exterior
         *    fallback below since it's still "inside the .app", closer in
         *    spirit to "." than to the visible-icon folder.
         *  - "../../..": beside the .app icon itself (MacOS -> Contents ->
         *    EarthBound.app -> its containing folder). "Put your ROM here.txt"
         *    (the public zip's own instructions, right next to EarthBound.app,
         *    not inside it) tells the player to drop their ROM here -- the
         *    natural place to drag a file onto a visible Finder icon. A ROM
         *    placed exactly where the instructions say was never actually
         *    found by the "." search alone -- reported live as "can't find
         *    my ROM" with a ROM genuinely sitting right there.
         *
         * Both are only tried after "." finds nothing, so a ROM someone
         * deliberately placed in Contents/MacOS/ itself (matching every
         * other platform's plain "beside the binary" convention) still
         * takes priority over either bundle-relative fallback. */
        if (!rom_found) {
            rom_found = rom_extract_find_rom("..", rom_path, sizeof(rom_path));
        }
        if (!rom_found) {
            rom_found = rom_extract_find_rom("../../..", rom_path, sizeof(rom_path));
        }
#endif
        if (rom_found) {
            /* "./" prefix: POSIX shells (unlike Windows' CreateProcess-style
             * search) don't look in the current directory by default, and
             * system() always goes through a shell. */
            const char *helper_name =
#ifdef _WIN32
                "ebtools-setup.exe";
#else
                "./ebtools-setup";
#endif
            struct stat helper_st;
            bool helper_ready = (stat(helper_name, &helper_st) == 0);
            if (!helper_ready) {
                /* No on-disk copy (the common case now -- the game
                 * executable itself carries this, see
                 * EB_EMBED_SETUP_HELPER_PATH in CMakeLists.txt) -- extract
                 * it once to the same spot the stat() above just checked,
                 * so every launch after this one finds it there directly
                 * without going through this path again. A build with
                 * nothing embedded (a plain dev build) gets NULL/0 back and
                 * falls through to found_rom_no_helper below, same as
                 * before this existed. */
                size_t blob_size = 0;
                const unsigned char *blob = eb_embedded_setup_helper_data(&blob_size);
                if (blob && blob_size > 0) {
                    FILE *out = fopen(helper_name, "wb");
                    if (out) {
                        size_t written = fwrite(blob, 1, blob_size, out);
                        fclose(out);
                        if (written == blob_size) {
#ifndef _WIN32
                            chmod(helper_name, 0755);
#endif
                            helper_ready = true;
                        } else {
                            remove(helper_name); /* partial write (disk full?) -- don't leave a broken file behind */
                        }
                    }
                }
            }
            if (helper_ready) {
                char cmd[8192];
                /* system() on Windows runs the string via `cmd.exe /c`,
                 * which has a well-known quirk: if the command starts with
                 * a quoted token and has more quoted tokens after it, cmd
                 * mis-parses unless the whole line is wrapped in one more
                 * pair of quotes (cmd strips exactly one leading+trailing
                 * quote pair before parsing args). POSIX shells don't have
                 * this problem, so only wrap on Windows. */
#ifdef _WIN32
                int n = snprintf(cmd, sizeof(cmd), "\"\"%s\" \"%s\" --out \"%s\"\"", helper_name, rom_path, assets_path);
#else
                int n = snprintf(cmd, sizeof(cmd), "\"%s\" \"%s\" --out \"%s\"", helper_name, rom_path, assets_path);
#endif
                if (n > 0 && (size_t)n < sizeof(cmd)) {
                    /* This is a real multi-step pipeline (extract, pack-all,
                     * pack-assets) against a ~3MB ROM -- observed 30-60s on
                     * an SSD, longer on slower disks/first-run antivirus
                     * scanning on Windows. With no window up yet, the OS
                     * would otherwise flag the process as unresponsive
                     * within seconds. Show a small window with the message
                     * drawn via the built-in 5x7 font below (the OS window
                     * chrome truncates long title-bar text, so the title
                     * alone isn't enough -- the game's own font can't be
                     * used here since it's itself one of the assets being
                     * generated) and pump events on the background
                     * thread's behalf so it stays responsive throughout. */
                    SDL_Init(SDL_INIT_VIDEO);
                    /* Two lines, drawn with the built-in 5x7 font below (the
                     * window title alone gets truncated by the OS chrome on
                     * most platforms, so the real message needs to live in
                     * the window body). Window width sized to fit the
                     * longer line (48 chars * 6px/cell * 2x scale = 576px)
                     * with margin, rather than shrinking the text further. */
                    const char *line1 = "GENERATING GAME ASSETS...";
                    const char *line2 = "THIS ONLY HAPPENS ONCE, MAY TAKE A MINUTE OR TWO";
                    int scale1 = 3, scale2 = 2;
                    SDL_Window *setup_win = SDL_CreateWindow(
                        "EarthBound - Setting Up",
                        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 640, 160, SDL_WINDOW_SHOWN);
                    SDL_Surface *setup_surf = setup_win ? SDL_GetWindowSurface(setup_win) : NULL;

                    int width1 = (int)strlen(line1) * 6 * scale1;
                    int width2 = (int)strlen(line2) * 6 * scale2;

                    SDL_AtomicSet(&g_setup_helper_done, 0);
                    SDL_AtomicSet(&g_setup_helper_exit_status, -1);
                    SDL_Thread *helper_thread = SDL_CreateThread(run_setup_helper_thread_fn, "eb_setup_helper", cmd);
                    if (helper_thread == NULL) {
                        /* Thread creation failed (very unlikely) -- fall back
                         * to the original blocking call rather than silently
                         * skipping setup. */
                        SDL_AtomicSet(&g_setup_helper_exit_status, system(cmd));
                    } else {
                        Uint32 pulse_start = SDL_GetTicks();
                        while (!SDL_AtomicGet(&g_setup_helper_done)) {
                            SDL_Event ev;
                            while (SDL_PollEvent(&ev)) { /* swallow input, keep the OS's event pump happy */
                            }
                            if (setup_surf) {
                                /* Simple back-and-forth grey background pulse
                                 * behind the text so the window visibly isn't
                                 * frozen even during a long silent stretch. */
                                Uint32 phase = (SDL_GetTicks() - pulse_start) % 1000;
                                Uint8 v = (Uint8)(phase < 500 ? 30 + phase / 20 : 55 - (phase - 500) / 20);
                                SDL_FillRect(setup_surf, NULL, SDL_MapRGB(setup_surf->format, v, v, v));
                                Uint32 white = SDL_MapRGB(setup_surf->format, 255, 255, 255);
                                draw_text5x7(setup_surf, (setup_surf->w - width1) / 2, 55, line1, scale1, white);
                                draw_text5x7(setup_surf, (setup_surf->w - width2) / 2, 95, line2, scale2, white);
                                SDL_UpdateWindowSurface(setup_win);
                            }
                            SDL_Delay(30);
                        }
                        SDL_WaitThread(helper_thread, NULL);
                    }

                    if (setup_win) {
                        SDL_DestroyWindow(setup_win);
                    }
                    SDL_QuitSubSystem(SDL_INIT_VIDEO);

                    assets_result = eb_runtime_assets_load(assets_path);

                    if (assets_result != EB_ASSETS_OK) {
                        /* The bundled helper ran but didn't leave a usable
                         * pak behind -- either it exited non-zero (a crash
                         * partway through, seen live on a real donor ROM)
                         * or it exited clean but wrote something
                         * eb_runtime_assets_load() still rejects. Either
                         * way, the ROM itself is right here and already
                         * confirmed readable (rom_found, above) -- fall
                         * back to building the pak directly in C instead of
                         * telling a player who supplied a perfectly good
                         * ROM that their game data is missing. */
                        fprintf(stderr,
                            "Setup helper didn't produce usable game data "
                            "(exit status %d) -- falling back to the built-in "
                            "ROM extractor.\n",
                            SDL_AtomicGet(&g_setup_helper_exit_status));
                        EbRomExtractResult fallback = rom_extract_build_pak(rom_path, assets_path);
                        if (fallback == EB_ROM_EXTRACT_OK) {
                            assets_result = eb_runtime_assets_load(assets_path);
                        }
                    }
                }
            } else {
                found_rom_no_helper = true;
            }
        }
    }

    if (assets_result != EB_ASSETS_OK) {
        const char *reason = "unknown error";
        switch (assets_result) {
            case EB_ASSETS_MISSING: reason = "file not found"; break;
            case EB_ASSETS_BAD_MAGIC: reason = "not a valid assets.pak"; break;
            case EB_ASSETS_VERSION_MISMATCH: reason = "unsupported assets.pak version"; break;
            case EB_ASSETS_LAYOUT_MISMATCH: reason = "assets.pak was built for a different version of this game"; break;
            case EB_ASSETS_COUNT_MISMATCH: reason = "assets.pak doesn't match this build (wrong asset count)"; break;
            case EB_ASSETS_IO_ERROR: reason = "I/O error (truncated or corrupt file)"; break;
            case EB_ASSETS_OK: break;
        }
        fprintf(stderr, "Failed to load assets from %s: %s\n", assets_path, reason);

        /* The game's own text rendering needs the very assets that are
         * missing (the font is itself a ROM-derived asset), so there's no
         * way to explain this in-game -- a plain SDL message box is the
         * only UI available at this point. Needs SDL_INIT_VIDEO, nothing
         * else the game itself would need. */
        SDL_Init(SDL_INIT_VIDEO);
        char message[512];
        if (found_rom_no_helper) {
            /* Distinct message: telling this player to add a ROM would be
             * actively wrong, they already have one. What's missing is the
             * setup helper -- typically a tester who updated in-app from a
             * build old enough to predate it (the updater only replaces the
             * game binary itself, never installs the helper alongside it,
             * so anyone whose folder never had one still doesn't after
             * updating). Names the exact file so there's no guessing. */
            snprintf(message, sizeof(message),
                "EarthBound found your ROM, but not its setup helper "
                "(ebtools-setup%s).\n\n"
                "Download that file from the same place you got this update "
                "and put it in this same folder, then launch again -- it'll "
                "set itself up automatically from here, just this once.",
#ifdef _WIN32
                ".exe"
#else
                ""
#endif
            );
        } else {
            snprintf(message, sizeof(message),
                "EarthBound couldn't find your game data (%s).\n\n"
                "Put your EarthBound (USA) ROM file in the same folder as this "
                "program (any filename ending in .sfc or .smc works) and launch "
                "it again -- it'll set itself up automatically, just this once.",
                reason);
        }
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, "EarthBound - Setup Needed", message, NULL);
        return 1;
    }
#endif

    /* Declare per-monitor DPI awareness before SDL_Init -- Windows-only hint
     * (harmless no-op elsewhere). Without this, an unaware app gets DPI
     * *virtualized* by Windows on any display above 100% scaling (the
     * default on most modern laptops/monitors): Windows reports a smaller
     * "virtual" desktop resolution, SDL_WINDOW_FULLSCREEN_DESKTOP sizes the
     * window to that shrunken virtual mode instead of the real one, and the
     * OS then bitmap-stretches the whole already-scaled result back up to
     * fill the real screen -- a second, blurry scaling pass stacked on top
     * of platform_video_init()'s own SDL_RenderSetLogicalSize() widescreen
     * scaling. This is almost certainly why widescreen/dynamic scaling
     * looked broken specifically on Windows and nowhere else: the two
     * scaling passes fighting each other, not a bug in the scaling logic
     * itself (sdl2_video.c is platform-agnostic and already correct). */
    SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2");

    /* Initialize SDL */
    uint32_t sdl_flags = SDL_INIT_TIMER;
    if (!platform_headless) {
        sdl_flags |= SDL_INIT_VIDEO;
#ifdef EB_ENABLE_AUDIO
        sdl_flags |= SDL_INIT_AUDIO;
#endif
    }
    if (SDL_Init(sdl_flags) < 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    /* Initialize subsystems */
    if (!platform_video_init()) {
        fprintf(stderr, "Video init failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    platform_input_init();
    platform_save_init();
    settings_load();
    /* Resume logging automatically if a previous session left the Config
     * menu's "Logging" row on -- a tester shouldn't have to re-enable it
     * after every relaunch (including the updater's own self-relaunch) to
     * keep capturing a bug that only shows up after several launches. */
    if (engine_logging == LOGGING_ON)
        platform_log_set_enabled(true);
    /* Modern Alternative Visuals defaulting gameplay to the zoomed-out FOV
     * (settings.h) is NOT applied here at raw boot -- tried that first; it
     * left ow.zoom_mode non-OFF by the time the title screen itself
     * rendered (title/file-select is reached immediately after boot),
     * which broke the version overlay there (reported live). Applied
     * instead in game_main.c's host_process_frame(), on the frame the
     * player actually leaves title/file-select for gameplay -- see that
     * function's own doc comment for the full reasoning; that site
     * subsumes both "already configured at boot" and "switched mid-game"
     * without this file needing to know about either. */
    /* Registered first so it runs LAST (atexit is LIFO): the relaunch
     * execv(), if the updater ever staged one, must happen after
     * platform_cleanup()'s SDL_Quit() below, never before. See
     * maybe_relaunch_atexit()'s comment above for why this ordering can't
     * be expressed as plain sequential code in main() instead. */
    atexit(maybe_relaunch_atexit);
    atexit(platform_cleanup);
    platform_timer_init();

    /* Initialize game systems */
    game_init();

    /* --load-state: resume from savestate.bin.0/.1 in the CWD instead of a
     * fresh boot. Just requests it here -- the normal per-frame
     * host_process_frame()/host_root_boundary() pump below does the actual
     * free-run-to-root-boundary and restore over the next frame or two. */
    if (load_state_at_boot)
        host_request_load();

    /* Savestate round-trip self-test: prove state_dump_load() reads back exactly
     * what state_dump_save() wrote (byte-identical), then exit. Runs against the
     * post-game_init boot state. */
    if (savestate_selftest) {
        bool ok = state_dump_roundtrip_test();
        fprintf(stderr, "savestate round-trip self-test: %s\n", ok ? "PASS" : "FAIL");
        bool ok_pert = state_dump_perturb_test();
        fprintf(stderr, "savestate perturb (cold-load) self-test: %s\n", ok_pert ? "PASS" : "FAIL");
        bool ok_cs = state_dump_crashsafe_test();
        fprintf(stderr, "savestate crash-safe self-test: %s\n", ok_cs ? "PASS" : "FAIL");
        bool ok_ap = state_dump_asset_pointer_test();
        fprintf(stderr, "savestate asset-pointer self-test: %s\n", ok_ap ? "PASS" : "FAIL");
        exit((ok && ok_pert && ok_cs && ok_ap) ? 0 : 1);
    }

    if (keyitems_selftest) {
        bool ok_ki = key_items_selftest();
        fprintf(stderr, "key items pool self-test: %s\n", ok_ki ? "PASS" : "FAIL");
        exit(ok_ki ? 0 : 1);
    }

    if (joinlevel_selftest) {
        bool ok_jl = join_level_scaling_selftest();
        fprintf(stderr, "join-level scaling self-test: %s\n", ok_jl ? "PASS" : "FAIL");
        exit(ok_jl ? 0 : 1);
    }

    if (threed_zombie_flag_check || threed_zombie_flag_fix) {
        /* See --check-threed-zombie-flag/--fix-threed-zombie-flag's own
         * doc comments above for the full story. Read-only unless -fix
         * is the one that was passed. */
        static const struct { const char *name; uint16_t id; } zf[] = {
            {"FLG_THRK_BIKINIZOMBI_F_APPEAR", 296},
            {"FLG_THRK_BIKINIZOMBI_P_APPEAR", 297},
            {"FLG_THRK_HOTELZOMBI_APPEAR",    298},
        };
        for (int slot = 0; slot < SAVE_COUNT; slot++) {
            if (!load_game(slot)) {
                fprintf(stderr, "slot %d: empty/unreadable, skipping\n", slot + 1);
                continue;
            }
            fprintf(stderr, "slot %d (before):", slot + 1);
            for (size_t fi = 0; fi < sizeof(zf) / sizeof(zf[0]); fi++)
                fprintf(stderr, " %s=%d", zf[fi].name, event_flag_get(zf[fi].id));
            fprintf(stderr, "\n");
            if (threed_zombie_flag_fix) {
                bool changed = false;
                for (size_t fi = 0; fi < sizeof(zf) / sizeof(zf[0]); fi++) {
                    if (event_flag_get(zf[fi].id)) {
                        event_flag_clear(zf[fi].id);
                        changed = true;
                    }
                }
                if (changed) {
                    bool ok = save_game(slot);
                    fprintf(stderr, "slot %d: cleared, re-save %s\n", slot + 1,
                            ok ? "OK" : "FAILED");
                } else {
                    fprintf(stderr, "slot %d: nothing stuck, left untouched\n", slot + 1);
                }
            }
        }
        exit(0);
    }

    /* Initialize audio (loads audio packs from embedded assets) */
    if (!platform_headless)
        platform_audio_init();

#ifdef EB_ENABLE_AUDIO
    {
        /* No --msu-dir given: fall back to the conventional default, a
         * "msu" folder relative to the CWD -- the same directory
         * settings.dat, savestate.bin.N, and earthbound.srm already live in
         * for every launch method this port supports. This is what makes a
         * dropped-in MSU1 pack work with zero configuration: no
         * --msu-dir/--msu-name flags, and nothing to add to a Steam
         * shortcut's Launch Options (editing those is real friction, and
         * has to be redone every time the pack changes). */
        const char *dir = msu_dir ? msu_dir : "msu";
        char detected_name[128];
        const char *name = msu_name;

        if (!name) {
            /* No --msu-name given: recover the pack's <name> prefix by
             * inspecting one of its own track filenames, so the exact
             * naming convention a given pack ships with never has to be
             * known ahead of time. */
            if (platform_audio_msu_autodetect_name(dir, detected_name, sizeof(detected_name))) {
                name = detected_name;
            }
#ifdef __APPLE__
            /* Same "beside the .app, not beside the real binary" gap as
             * the ROM search above (see its doc comment) -- and the same
             * two bundle-relative spots worth trying, in the same order:
             * "../msu" (Contents/msu, for a player who opened "Show
             * Package Contents" and dropped the msu folder straight into
             * Contents/, right alongside a ROM placed the same way) before
             * "../../../msu" (beside the .app icon itself, where "Put your
             * ROM here.txt" tells everyone to drop it). Only tried when the
             * plain "msu" lookup found nothing and no --msu-dir override
             * was given (an explicit override is respected as-is, no
             * bundle guessing). Reported live as HQ Audio showing N/A
             * despite a pack genuinely being present. */
            else if (!msu_dir &&
                     platform_audio_msu_autodetect_name("../msu", detected_name, sizeof(detected_name))) {
                dir = "../msu";
                name = detected_name;
            }
            else if (!msu_dir &&
                     platform_audio_msu_autodetect_name("../../../msu", detected_name, sizeof(detected_name))) {
                dir = "../../../msu";
                name = detected_name;
            }
#endif
            else if (msu_dir) {
                /* --msu-dir was explicit but has no recognizable pack in
                 * it -- likely a real misconfiguration, not "no pack here
                 * at all", so still attempt the load (per-track lookups
                 * will just fail, same as an unset --msu-name always has). */
                name = "";
            }
        }

        if (name)
            platform_audio_msu_load(dir, name);
        /* else: defaulted to "msu" and found nothing there -- no pack
         * configured, exactly as if --msu-dir were never passed. */
    }
#endif

#ifdef EB_ENABLE_VERIFY
    if (verify_rom_path) {
        if (!verify_init(verify_rom_path)) {
            fprintf(stderr, "Failed to initialize verification with ROM: %s\n", verify_rom_path);
        }
    }
#else
    (void)verify_rom_path;
#endif

    /* Start frame timer, then run the single top-level game loop:
     * advance the game one frame (game_loop_step), then perform the one
     * per-frame host yield (host_process_frame). Restart-on-game-over is
     * handled inside the step machine, so there is no restart wrapper.
     * See docs/plans/savestate-unified-loop.md. */
    platform_timer_frame_start();

    for (int frame_num = 0; !platform_input_quit_requested(); frame_num++) {
        if (dump_frame >= 0 && frame_num == dump_frame)
            platform_video_request_screenshot("screenshot.bmp");
        game_loop_step();
        host_process_frame();
        host_root_boundary(); /* root boundary: perform any pending torn-safe capture */
        if (dump_flags_frame >= 0 && frame_num == dump_flags_frame) {
            /* Debug: hardcoded event-flag dump, temporary investigation aid. */
            static const struct { const char *name; uint16_t id; } debug_flags[] = {
                {"FLG_MYHOME_POKEY_APPEAR", 301},   {"FLG_MYHOME_POKEY_ENTER", 97},
                {"FLG_MYHOME_POKEY_DISAPPEAR", 30}, {"FLG_MYHOME_POKEY2_APPEAR", 310},
                {"FLG_MYHOME_KNOCK_APPEAR", 295},   {"FLG_MYHOME_START", 375},
                {"FLG_YAZIUMA_DISAPPEAR", 469},     {"FLG_MYHOME_2F_1F", 513},
                {"FLG_MYHOME_LIGHT_ON", 517},       {"FLG_MYHOME_SLEEPNES_APPEAR", 477},
                {"FLG_MYHOME_TO_BE", 763},          {"FLG_ITEM_BOX_006", 805},
                {"FLG_MYHOME_DOOR_CLOSE", 467},     {"FLG_MYHOME_1F_TRACY_APPEAR", 531},
                {"FLG_INSEKI_STOPPER_APPEAR", 476}, {"FLG_MYHOME_MAMA_YEAH", 94},
            };
            fprintf(stderr, "--- event flags @ frame %d ---\n", frame_num);
            for (size_t fi = 0; fi < sizeof(debug_flags) / sizeof(debug_flags[0]); fi++)
                fprintf(stderr, "  %-28s (%3u) = %d\n", debug_flags[fi].name, debug_flags[fi].id,
                        event_flag_get(debug_flags[fi].id));
        }
    }

#ifdef EB_ENABLE_VERIFY
    verify_shutdown();
#endif

    return 0;
}
