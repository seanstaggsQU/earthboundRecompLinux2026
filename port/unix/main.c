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
#endif

#include "platform/platform.h"
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

int main(int argc, char *argv[]) {
    chdir_to_executable_dir(); /* before anything below touches a relative path */
    SDL_SetMainReady(); /* pairs with SDL_MAIN_HANDLED above */
    const char *verify_rom_path = NULL;
    bool savestate_selftest = false;
    bool keyitems_selftest = false;
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
#endif

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--verify") == 0 && i + 1 < argc) {
            verify_rom_path = argv[++i];
#ifdef EB_RUNTIME_ASSETS
        } else if (strcmp(argv[i], "--assets") == 0 && i + 1 < argc) {
            assets_path = argv[++i];
#endif
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
#endif
                            " [--selftest-savestate] [--selftest-keyitems]\n",
                    argv[0]);
            return 1;
        }
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
            } else if (msu_dir) {
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
