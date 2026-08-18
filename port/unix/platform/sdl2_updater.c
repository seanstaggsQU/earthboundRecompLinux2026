/*
 * Self-update backend — desktop (port/unix) only.
 *
 * Real implementation is gated behind EB_UPDATER_ENABLED, a compile
 * definition CMakeLists.txt sets only when both EB_UPDATER_REPO and
 * EB_UPDATER_TOKEN are supplied at configure time (see port/unix/
 * CMakeLists.txt) -- a private GitHub repo holding nothing but release
 * assets, never source code (this project's public repo embeds ROM-derived
 * assets into compiled binaries, so the update feed itself has to live
 * somewhere private).
 *
 * When EB_UPDATER_ENABLED is *not* defined (the default -- matches every
 * build before this feature existed), this file provides the same no-op
 * behavior as src/platform/updater_backend_stub.c's embedded stub, just
 * scoped to "desktop build, feature not configured" instead of "no backend
 * exists at all".
 *
 * RELEASE ASSET FORMAT: deliberately NOT a zip. Unpacking a zip on this
 * side would need a vendored inflate/zip-reader library and its own
 * member-name allowlist logic; instead, the release process publishes the
 * raw platform binary (and, on macOS, its paired dylib) as individually
 * named GitHub release assets, alongside the existing user-facing zips
 * (which are for a *fresh* download, not what this updater fetches):
 *   - "earthbound-macos" + "libSDL2-2.0.0.dylib" (macOS)
 *   - "earthbound-linux" (Linux)
 *   - "checksums.txt" (sha256sum-format: "<hex>  <filename>" per line,
 *     covering the exact raw asset names above)
 * The updater only ever looks for these exact, hardcoded names -- there is
 * no path where an unexpected file in the release could be written
 * anywhere, unlike a zip-member allowlist which still has to trust the
 * zip's own internal member names.
 *
 * GITHUB PRIVATE-ASSET DOWNLOAD QUIRK: GET .../releases/assets/{id} with
 * Accept: application/octet-stream 302-redirects to a pre-signed S3 URL.
 * That signed URL must be fetched WITHOUT our Authorization header --
 * forwarding it can trip "Only one auth mechanism allowed" on S3's side,
 * and it's unnecessary since the signature in the URL's own query string
 * already grants access. http_get() below never auto-follows redirects
 * (CURLOPT_FOLLOWLOCATION stays off); the one call site that can redirect
 * (fetch_release_asset()) reads CURLINFO_REDIRECT_URL and reissues a fresh,
 * unauthenticated request itself.
 */
#include "platform/platform.h"

#ifdef EB_UPDATER_ENABLED

#include <SDL.h>
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>
#include "cJSON.h"
#include "sha256.h"
#include "version.h"          /* EB_VERSION_STRING -- generated, see CMakeLists.txt */
#include "updater_secret.h"   /* EB_UPDATER_REPO_STRING / EB_UPDATER_TOKEN_STRING -- generated, gitignored */

/* Unix-specific: defined in main.c, called once the new binary is verified
 * and swapped into place. Mirrors platform_save_set_path()'s existing
 * pattern of a port/unix-local entry point forward-declared where it's
 * used, not added to the shared platform.h contract. */
void platform_update_stage_relaunch(const char *new_exe_path);

#if defined(__APPLE__)
#define EB_UPDATER_ASSET_NAME "earthbound-macos"
#define EB_UPDATER_DYLIB_NAME "libSDL2-2.0.0.dylib"
#elif defined(__linux__)
#define EB_UPDATER_ASSET_NAME "earthbound-linux"
#else
#error "sdl2_updater.c: unhandled platform (Windows self-update is a separate, deferred backend)"
#endif

/* ------------------------------------------------------------------------
 * Shared state -- written only by the background thread(s), read only by
 * platform_update_poll() on the main thread, always under g_mutex. Never
 * held across a network call.
 * ---------------------------------------------------------------------- */
static SDL_mutex *g_mutex;
static EbUpdateProgress g_progress;

/* Populated by the check thread, consumed by the download thread. Fixed
 * against a TOCTOU/race concern: the download thread only ever starts
 * after platform_update_download_start() is called, which only happens
 * after the UI has already observed EB_UPDATE_AVAILABLE from a completed
 * check -- so these are stable (write-once, from one thread, before the
 * second thread is even created) by the time download_thread_fn reads them. */
static char g_asset_api_url[512];
static char g_checksums_api_url[512];

static SDL_mutex *updater_mutex(void) {
    if (!g_mutex)
        g_mutex = SDL_CreateMutex();
    return g_mutex;
}

static void set_progress(const EbUpdateProgress *p) {
    SDL_LockMutex(updater_mutex());
    g_progress = *p;
    SDL_UnlockMutex(updater_mutex());
}

static void set_error(const char *msg) {
    EbUpdateProgress p = {0};
    p.status = EB_UPDATE_ERROR;
    snprintf(p.error_message, sizeof(p.error_message), "%s", msg);
    set_progress(&p);
}

/* ------------------------------------------------------------------------
 * HTTP helpers
 * ---------------------------------------------------------------------- */
typedef struct {
    char *data;
    size_t len;
} MemBuf;

static size_t membuf_write_cb(void *ptr, size_t size, size_t nmemb, void *userdata) {
    MemBuf *mb = (MemBuf *)userdata;
    size_t add = size * nmemb;
    char *grown = realloc(mb->data, mb->len + add + 1);
    if (!grown) return 0; /* signals error to curl */
    mb->data = grown;
    memcpy(mb->data + mb->len, ptr, add);
    mb->len += add;
    mb->data[mb->len] = '\0';
    return add;
}

typedef struct {
    FILE *f;
    int *progress_percent_out; /* NULL = don't report */
} FileWriteCtx;

static size_t file_write_cb(void *ptr, size_t size, size_t nmemb, void *userdata) {
    FileWriteCtx *ctx = (FileWriteCtx *)userdata;
    return fwrite(ptr, size, nmemb, ctx->f);
}

static int xfer_progress_cb(void *clientp, curl_off_t dltotal, curl_off_t dlnow,
                             curl_off_t ultotal, curl_off_t ulnow) {
    (void)ultotal; (void)ulnow;
    FileWriteCtx *ctx = (FileWriteCtx *)clientp;
    if (ctx->progress_percent_out && dltotal > 0) {
        int pct = (int)((dlnow * 100) / dltotal);
        EbUpdateProgress p;
        SDL_LockMutex(updater_mutex());
        p = g_progress;
        SDL_UnlockMutex(updater_mutex());
        p.progress_percent = pct;
        set_progress(&p);
    }
    return 0; /* non-zero would abort the transfer */
}

/* One request, no auto-follow. `accept`/`send_auth` may be NULL/false.
 * On success, either `mem` or `file_ctx` (whichever is non-NULL) received
 * the body. Returns the HTTP status code, or -1 on a transport-level
 * failure (curl_err, if non-NULL, gets a short description). */
static long http_get(const char *url, bool send_auth, const char *accept,
                      MemBuf *mem, FileWriteCtx *file_ctx, char *curl_err, size_t curl_err_size) {
    CURL *curl = curl_easy_init();
    if (!curl) {
        if (curl_err) snprintf(curl_err, curl_err_size, "curl init failed");
        return -1;
    }

    struct curl_slist *headers = NULL;
    if (send_auth) {
        char auth_hdr[300];
        snprintf(auth_hdr, sizeof(auth_hdr), "Authorization: token %s", EB_UPDATER_TOKEN_STRING);
        headers = curl_slist_append(headers, auth_hdr);
    }
    if (accept) {
        char accept_hdr[128];
        snprintf(accept_hdr, sizeof(accept_hdr), "Accept: %s", accept);
        headers = curl_slist_append(headers, accept_hdr);
    }
    headers = curl_slist_append(headers, "User-Agent: EarthBound-Updater");

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L); /* see file header comment */
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L); /* never disable -- see CLAUDE.md safety notes */
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);

    if (mem) {
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, membuf_write_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, mem);
    } else if (file_ctx) {
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, file_write_cb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, file_ctx);
        if (file_ctx->progress_percent_out) {
            curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
            curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, xfer_progress_cb);
            curl_easy_setopt(curl, CURLOPT_XFERINFODATA, file_ctx);
        }
    }

    CURLcode res = curl_easy_perform(curl);
    long http_code = -1;
    if (res != CURLE_OK) {
        if (curl_err) snprintf(curl_err, curl_err_size, "%s", curl_easy_strerror(res));
    } else {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return res == CURLE_OK ? http_code : -1;
}

/* Fetches a release asset by its API "url" (.../releases/assets/{id}),
 * following exactly one redirect hop to the pre-signed download URL
 * WITHOUT forwarding our Authorization header (see file header comment). */
static bool fetch_release_asset(const char *asset_api_url, MemBuf *mem, FileWriteCtx *file_ctx,
                                 char *err, size_t err_size) {
    CURL *probe = curl_easy_init();
    if (!probe) { snprintf(err, err_size, "curl init failed"); return false; }

    struct curl_slist *headers = NULL;
    char auth_hdr[300];
    snprintf(auth_hdr, sizeof(auth_hdr), "Authorization: token %s", EB_UPDATER_TOKEN_STRING);
    headers = curl_slist_append(headers, auth_hdr);
    headers = curl_slist_append(headers, "Accept: application/octet-stream");
    headers = curl_slist_append(headers, "User-Agent: EarthBound-Updater");

    curl_easy_setopt(probe, CURLOPT_URL, asset_api_url);
    curl_easy_setopt(probe, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(probe, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(probe, CURLOPT_TIMEOUT, 60L);
    curl_easy_setopt(probe, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(probe, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(probe, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(probe, CURLOPT_NOBODY, 1L); /* HEAD-like: we only want the redirect, not this response's body */

    CURLcode res = curl_easy_perform(probe);
    if (res != CURLE_OK) {
        snprintf(err, err_size, "%s", curl_easy_strerror(res));
        curl_slist_free_all(headers);
        curl_easy_cleanup(probe);
        return false;
    }
    long code = 0;
    char *redirect_url = NULL;
    curl_easy_getinfo(probe, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_getinfo(probe, CURLINFO_REDIRECT_URL, &redirect_url);

    bool ok = false;
    if (code >= 300 && code < 400 && redirect_url) {
        /* Second, unauthenticated request to the signed URL -- the actual
         * body transfer happens here. */
        char redirect_url_copy[2048];
        snprintf(redirect_url_copy, sizeof(redirect_url_copy), "%s", redirect_url);
        curl_slist_free_all(headers);
        curl_easy_cleanup(probe);

        long final_code = http_get(redirect_url_copy, false, NULL, mem, file_ctx, err, err_size);
        ok = (final_code >= 200 && final_code < 300);
        if (!ok && err[0] == '\0')
            snprintf(err, err_size, "download failed (HTTP %ld)", final_code);
        return ok;
    }

    /* No redirect -- either an error, or (unexpectedly) the body came back
     * directly. Treat anything outside 2xx as failure; re-fetch the body
     * for the 2xx case since CURLOPT_NOBODY above discarded it. */
    curl_slist_free_all(headers);
    curl_easy_cleanup(probe);
    if (code >= 200 && code < 300) {
        long final_code = http_get(asset_api_url, true, "application/octet-stream", mem, file_ctx, err, err_size);
        return final_code >= 200 && final_code < 300;
    }
    snprintf(err, err_size, "unexpected response (HTTP %ld)", code);
    return false;
}

/* ------------------------------------------------------------------------
 * Check thread
 * ---------------------------------------------------------------------- */
static int check_thread_fn(void *unused) {
    (void)unused;
    char url[256];
    snprintf(url, sizeof(url), "https://api.github.com/repos/%s/releases/latest", EB_UPDATER_REPO_STRING);

    MemBuf body = {0};
    char err[128] = {0};
    long code = http_get(url, true, "application/vnd.github+json", &body, NULL, err, sizeof(err));

    if (code < 200 || code >= 300) {
        char msg[64];
        if (code == 404)
            snprintf(msg, sizeof(msg), "No releases found");
        else if (code == -1)
            snprintf(msg, sizeof(msg), "%s", err[0] ? err : "Network error");
        else
            snprintf(msg, sizeof(msg), "Server error (HTTP %ld)", code);
        set_error(msg);
        free(body.data);
        return 0;
    }

    cJSON *root = cJSON_ParseWithLength(body.data, body.len);
    free(body.data);
    if (!root) {
        set_error("Bad response from update server");
        return 0;
    }

    cJSON *tag = cJSON_GetObjectItemCaseSensitive(root, "tag_name");
    if (!cJSON_IsString(tag)) {
        set_error("Bad response from update server");
        cJSON_Delete(root);
        return 0;
    }

    if (strcmp(tag->valuestring, EB_VERSION_STRING) == 0) {
        EbUpdateProgress p = {0};
        p.status = EB_UPDATE_UP_TO_DATE;
        set_progress(&p);
        cJSON_Delete(root);
        return 0;
    }

    /* Find this platform's binary asset and checksums.txt among the
     * release's assets, by exact name -- see file header comment on why
     * this is exact-name matching against a small fixed set, not a zip
     * member scan. */
    cJSON *assets = cJSON_GetObjectItemCaseSensitive(root, "assets");
    const char *asset_url = NULL;
    const char *checksums_url = NULL;
    cJSON *asset;
    cJSON_ArrayForEach(asset, assets) {
        cJSON *name = cJSON_GetObjectItemCaseSensitive(asset, "name");
        cJSON *api_url = cJSON_GetObjectItemCaseSensitive(asset, "url");
        if (!cJSON_IsString(name) || !cJSON_IsString(api_url)) continue;
        if (strcmp(name->valuestring, EB_UPDATER_ASSET_NAME) == 0)
            asset_url = api_url->valuestring;
        else if (strcmp(name->valuestring, "checksums.txt") == 0)
            checksums_url = api_url->valuestring;
    }

    if (!asset_url || !checksums_url) {
        set_error("No release available for this platform");
        cJSON_Delete(root);
        return 0;
    }

    snprintf(g_asset_api_url, sizeof(g_asset_api_url), "%s", asset_url);
    snprintf(g_checksums_api_url, sizeof(g_checksums_api_url), "%s", checksums_url);

    EbUpdateProgress p = {0};
    p.status = EB_UPDATE_AVAILABLE;
    strncpy(p.latest_version, tag->valuestring, sizeof(p.latest_version) - 1);
    set_progress(&p);

    cJSON_Delete(root);
    return 0;
}

/* ------------------------------------------------------------------------
 * Download + verify + install thread
 * ---------------------------------------------------------------------- */

/* Parses a standard sha256sum-format line set ("<hex>  <filename>" per
 * line, an optional leading "*" before the filename for binary mode) and
 * returns the hex digest matching `want_name`, or NULL if not found.
 * `text` is modified in place (used as scratch for line-splitting). */
static const char *find_checksum(char *text, const char *want_name) {
    char *line = strtok(text, "\r\n");
    while (line) {
        char hex[65] = {0};
        char name[256] = {0};
        if (sscanf(line, "%64s %255s", hex, name) == 2) {
            const char *n = name;
            if (*n == '*') n++; /* binary-mode marker */
            if (strcmp(n, want_name) == 0) {
                static char result[65];
                snprintf(result, sizeof(result), "%s", hex);
                return result;
            }
        }
        line = strtok(NULL, "\r\n");
    }
    return NULL;
}

static bool sha256_hex_of_file(const char *path, char out_hex[65]) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    SHA256_CTX ctx;
    sha256_init(&ctx);
    unsigned char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        sha256_update(&ctx, buf, n);
    fclose(f);
    unsigned char hash[SHA256_BLOCK_SIZE];
    sha256_final(&ctx, hash);
    for (int i = 0; i < SHA256_BLOCK_SIZE; i++)
        snprintf(out_hex + i * 2, 3, "%02x", hash[i]);
    return true;
}

/* Downloads `asset_url` to `dest_path.part`, then renames to `dest_path`
 * only after both the transfer and the checksum verify succeed -- the
 * live file at `dest_path` (if any) is never touched on any failure path. */
static bool download_and_verify(const char *asset_url, const char *dest_path,
                                 const char *expected_hex, char *err, size_t err_size) {
    char tmp_path[4160];
    snprintf(tmp_path, sizeof(tmp_path), "%s.part", dest_path);

    FILE *f = fopen(tmp_path, "wb");
    if (!f) {
        snprintf(err, err_size, "Couldn't write to disk");
        return false;
    }
    int dummy_progress; /* only the binary asset reports progress; checksums.txt is tiny */
    FileWriteCtx ctx = { .f = f, .progress_percent_out = &dummy_progress };
    bool ok = fetch_release_asset(asset_url, NULL, &ctx, err, err_size);
    fclose(f);

    if (!ok) {
        remove(tmp_path);
        return false;
    }

    char actual_hex[65];
    if (!sha256_hex_of_file(tmp_path, actual_hex)) {
        snprintf(err, err_size, "Couldn't verify download");
        remove(tmp_path);
        return false;
    }
    if (strcmp(actual_hex, expected_hex) != 0) {
        snprintf(err, err_size, "Checksum mismatch");
        remove(tmp_path);
        return false;
    }

    remove(dest_path); /* best-effort; rename() below overwrites anyway on POSIX */
    if (rename(tmp_path, dest_path) != 0) {
        snprintf(err, err_size, "Couldn't finalize download: %s", strerror(errno));
        remove(tmp_path);
        return false;
    }
    return true;
}

static int download_thread_fn(void *unused) {
    (void)unused;
    char err[64] = {0};

    /* 1. checksums.txt -- small, fetched into memory. */
    MemBuf checksums = {0};
    if (!fetch_release_asset(g_checksums_api_url, &checksums, NULL, err, sizeof(err))) {
        set_error(err[0] ? err : "Couldn't fetch checksums");
        free(checksums.data);
        return 0;
    }

    char *checksums_copy = malloc(checksums.len + 1);
    if (!checksums_copy) { set_error("Out of memory"); free(checksums.data); return 0; }
    memcpy(checksums_copy, checksums.data, checksums.len);
    checksums_copy[checksums.len] = '\0';
    free(checksums.data);

    char expected_hex[65];
    const char *found = find_checksum(checksums_copy, EB_UPDATER_ASSET_NAME);
    if (found) snprintf(expected_hex, sizeof(expected_hex), "%s", found);
#ifdef __APPLE__
    char expected_dylib_hex[65];
    /* Re-tokenize: find_checksum()'s strtok already consumed checksums_copy. */
    char *checksums_copy2 = strdup(checksums_copy);
    const char *found_dylib = checksums_copy2 ? find_checksum(checksums_copy2, EB_UPDATER_DYLIB_NAME) : NULL;
    if (found_dylib) snprintf(expected_dylib_hex, sizeof(expected_dylib_hex), "%s", found_dylib);
#endif
    free(checksums_copy);
#ifdef __APPLE__
    free(checksums_copy2);
#endif

    if (!found) { set_error("Checksum manifest incomplete"); return 0; }
#ifdef __APPLE__
    if (!found_dylib) { set_error("Checksum manifest incomplete"); return 0; }
#endif

    /* 2. The binary itself, staged at "earthbound.new" beside the running
     * executable (CWD is already the executable's own directory -- see
     * chdir_to_executable_dir() in main.c, called before anything else). */
    if (!download_and_verify(g_asset_api_url, "earthbound.new", expected_hex, err, sizeof(err))) {
        set_error(err[0] ? err : "Download failed");
        return 0;
    }

#ifdef __APPLE__
    /* 3. macOS also ships its own dylib -- fetched the same way, by
     * re-resolving its asset url from a fresh check (the AVAILABLE check
     * only stored the platform binary + checksums URLs; the dylib isn't
     * looked up unless we're actually installing on macOS). Simpler in
     * practice: the dylib rarely changes version-to-version, but we still
     * verify+replace it every update for correctness. */
    char dylib_asset_url[512] = {0};
    {
        char url[256];
        snprintf(url, sizeof(url), "https://api.github.com/repos/%s/releases/latest", EB_UPDATER_REPO_STRING);
        MemBuf body = {0};
        long code = http_get(url, true, "application/vnd.github+json", &body, NULL, err, sizeof(err));
        if (code >= 200 && code < 300) {
            cJSON *root = cJSON_ParseWithLength(body.data, body.len);
            if (root) {
                cJSON *assets = cJSON_GetObjectItemCaseSensitive(root, "assets");
                cJSON *asset;
                cJSON_ArrayForEach(asset, assets) {
                    cJSON *name = cJSON_GetObjectItemCaseSensitive(asset, "name");
                    cJSON *api_url = cJSON_GetObjectItemCaseSensitive(asset, "url");
                    if (cJSON_IsString(name) && cJSON_IsString(api_url) &&
                        strcmp(name->valuestring, EB_UPDATER_DYLIB_NAME) == 0) {
                        snprintf(dylib_asset_url, sizeof(dylib_asset_url), "%s", api_url->valuestring);
                    }
                }
                cJSON_Delete(root);
            }
        }
        free(body.data);
    }
    if (!dylib_asset_url[0]) { set_error("No dylib in release"); remove("earthbound.new"); return 0; }
    if (!download_and_verify(dylib_asset_url, EB_UPDATER_DYLIB_NAME ".new", expected_dylib_hex, err, sizeof(err))) {
        set_error(err[0] ? err : "Dylib download failed");
        remove("earthbound.new");
        return 0;
    }
#endif

    /* 4. Install: swap the new file(s) into place. POSIX allows replacing
     * a file that's currently executing -- the running process keeps its
     * old inode open until it exits, so this is safe to do while running. */
    chmod("earthbound.new", 0755);
    if (rename("earthbound.new", "earthbound") != 0) {
        char msg[64];
        snprintf(msg, sizeof(msg), "Couldn't install: %s", strerror(errno));
        set_error(msg);
        return 0;
    }
#ifdef __APPLE__
    if (rename(EB_UPDATER_DYLIB_NAME ".new", EB_UPDATER_DYLIB_NAME) != 0) {
        /* The main binary already swapped -- this is a genuinely bad state
         * (mismatched binary/dylib pair), but there's no safe rollback of
         * step 4's first rename(). Surface it clearly; the player's next
         * launch will fail to load the dylib and needs a fresh download. */
        set_error("Update partially installed -- please redownload");
        return 0;
    }
#endif

    /* 5. Stage the relaunch (main.c's atexit-ordered execv, see its own
     * comment for why this can't just be an inline execv() here) and
     * request a clean quit. */
    char exe_path[4096];
    if (!getcwd(exe_path, sizeof(exe_path))) {
        set_error("Update installed -- please restart manually");
        return 0;
    }
    strncat(exe_path, "/earthbound", sizeof(exe_path) - strlen(exe_path) - 1);
    platform_update_stage_relaunch(exe_path);

    EbUpdateProgress p = {0};
    p.status = EB_UPDATE_DONE;
    set_progress(&p);
    platform_request_quit();
    return 0;
}

/* ------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */
bool platform_update_supported(void) {
    return true;
}

void platform_update_check_start(void) {
    EbUpdateProgress p = {0};
    p.status = EB_UPDATE_CHECKING;
    set_progress(&p);
    SDL_Thread *t = SDL_CreateThread(check_thread_fn, "eb_update_check", NULL);
    if (t) SDL_DetachThread(t);
    else set_error("Couldn't start update check");
}

void platform_update_download_start(void) {
    EbUpdateProgress p = {0};
    p.status = EB_UPDATE_DOWNLOADING;
    set_progress(&p);
    SDL_Thread *t = SDL_CreateThread(download_thread_fn, "eb_update_dl", NULL);
    if (t) SDL_DetachThread(t);
    else set_error("Couldn't start download");
}

void platform_update_poll(EbUpdateProgress *out) {
    SDL_LockMutex(updater_mutex());
    *out = g_progress;
    SDL_UnlockMutex(updater_mutex());
}

#else /* !EB_UPDATER_ENABLED */

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

#endif /* EB_UPDATER_ENABLED */
