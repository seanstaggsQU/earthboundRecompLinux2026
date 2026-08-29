#ifndef CORE_LOG_H
#define CORE_LOG_H

/*
 * Logging facade.
 *
 * Desktop builds route LOG_WARN / LOG_TRACE / FATAL through fprintf(stderr, ...)
 * gated by `verbose_level`. Embedded builds (-DEB_EMBEDDED) make LOG_WARN and
 * LOG_TRACE no-ops (drops a stdio dependency, lets the linker garbage-collect
 * the format strings) and turn FATAL into a CPU trap.
 *
 * Ports that want their own diagnostic sink can override by defining
 * EB_EMBEDDED and providing a wrapper header force-included before this one.
 */

#ifdef EB_EMBEDDED

#define LOG_WARN(...)  ((void)0)
#define LOG_TRACE(...) ((void)0)
#define LOG_EVENT(...) ((void)0)
#define FATAL(...)     __builtin_trap()

#else /* !EB_EMBEDDED */

#include <stdio.h>
#include <stdlib.h>

/* Verbosity levels: 0=errors only, 1=+warnings, 2=+trace */
extern int verbose_level;

#define LOG_WARN(...)  do { if (verbose_level >= 1) fprintf(stderr, __VA_ARGS__); } while (0)
#define LOG_TRACE(...) do { if (verbose_level >= 2) fprintf(stderr, __VA_ARGS__); } while (0)

/* LOG_EVENT: like LOG_WARN but always on, no -v needed. For rare,
 * high-value lifecycle events (not per-frame noise) where requiring a
 * launch-option flag before a report is even possible isn't acceptable --
 * e.g. the delivery system's checkpoints, see overworld_spawn.c/
 * callroutine.c. Combined with the existing "Logging" setting (Config menu
 * / platform_log_set_enabled(), which redirects stdout+stderr to
 * eb_debug.log next to the executable), this lands in that file with no
 * extra setup on the player's part. */
#define LOG_EVENT(...) fprintf(stderr, __VA_ARGS__)

/* Hard failure for unimplemented/unknown code paths, prints and aborts */
#define FATAL(...) do { fprintf(stderr, "FATAL: " __VA_ARGS__); abort(); } while (0)

#endif /* EB_EMBEDDED */

#endif /* CORE_LOG_H */
