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
 * raw platform binary (and its paired shared library, where the platform
 * has one) as individually named GitHub release assets, alongside the
 * existing user-facing zips (which are for a *fresh* download, not what
 * this updater fetches):
 *   - "earthbound-macos" + "libSDL2-2.0.0.dylib" (macOS)
 *   - "earthbound-linux" (Linux)
 *   - "earthbound-windows.exe" + "SDL2.dll" (Windows)
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
 * already grants access. http_get() below never auto-follows redirects on
 * either backend (libcurl: CURLOPT_FOLLOWLOCATION off; WinHTTP:
 * WINHTTP_OPTION_DISABLE_FEATURE/WINHTTP_DISABLE_REDIRECTS) -- the one call
 * site that can redirect (fetch_release_asset()) reads the Location itself
 * and reissues a fresh, unauthenticated request.
 *
 * WINDOWS SELF-REPLACE: Windows locks its own running .exe for its entire
 * lifetime -- unlike POSIX, there is no way to rename() or exec() a
 * replacement for the file you're currently executing from. Instead, the
 * install step here writes a small generated .bat helper and launches it
 * detached (CREATE_NO_WINDOW, survives this process exiting); the helper
 * waits for this process's PID to disappear from `tasklist`, THEN moves
 * the already-downloaded-and-verified "earthbound.new.exe" over
 * "earthbound.exe" (and the .dll, if present), relaunches it, and deletes
 * itself. This process's own role ends at "stage the download, spawn the
 * helper, quit cleanly" -- it never touches main.c's POSIX relaunch path
 * (platform_update_stage_relaunch()/execv()), which stays macOS/Linux-only.
 */
#include "platform/platform.h"

#ifdef EB_UPDATER_ENABLED

#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#else
#include <curl/curl.h>
#include <unistd.h>
#include <sys/stat.h>
#endif

#include "cJSON.h"
#ifdef _WIN32
/* sha256.h's own WORD typedef (unsigned int) collides with windows.h's
 * (unsigned short) once both are visible in the same translation unit --
 * rename via macro substitution during preprocessing so the vendored file
 * itself never needs touching; BYTE doesn't need the same treatment since
 * both headers agree it's unsigned char (identical redefinitions are legal). */
#define WORD SHA256_WORD
#include "sha256.h"
#undef WORD
#else
#include "sha256.h"
#endif
#include "version.h"          /* EB_VERSION_STRING -- generated, see CMakeLists.txt */
#include "updater_secret.h"   /* EB_UPDATER_REPO_STRING / EB_UPDATER_TOKEN_STRING -- generated, gitignored */
#include "data/pak_export.h"       /* pak_export_write() -- real body only in an EB_RUNTIME_ASSETS=OFF build */
#include "data/runtime_assets.h"   /* eb_runtime_assets_default_path() -- always compiled, see its own comment */

#ifndef _WIN32
/* Unix-specific: defined in main.c, called once the new binary is verified
 * and swapped into place. Mirrors platform_save_set_path()'s existing
 * pattern of a port/unix-local entry point forward-declared where it's
 * used, not added to the shared platform.h contract. Windows never calls
 * this -- see the file header's "WINDOWS SELF-REPLACE" note. */
void platform_update_stage_relaunch(const char *new_exe_path);
#endif

#if defined(__APPLE__)
#define EB_UPDATER_ASSET_NAME "earthbound-macos"
#define EB_UPDATER_DYLIB_NAME "libSDL2-2.0.0.dylib"
#elif defined(__linux__)
#define EB_UPDATER_ASSET_NAME "earthbound-linux"
#elif defined(_WIN32)
#define EB_UPDATER_ASSET_NAME "earthbound-windows.exe"
#define EB_UPDATER_DLL_NAME   "SDL2.dll"
#else
#error "sdl2_updater.c: unhandled platform"
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
 * HTTP helpers -- shared types; two backends (WinHTTP / libcurl) below
 * implement the same two entry points: http_get() and fetch_release_asset().
 * ---------------------------------------------------------------------- */
typedef struct {
    char *data;
    size_t len;
} MemBuf;

typedef struct {
    FILE *f;
    int *progress_percent_out; /* NULL = don't report */
} FileWriteCtx;

#ifdef _WIN32

/* ---- WinHTTP backend ---- */

/* Splits "https://host[:port]/path" (the only form ever used here -- the
 * GitHub API and its S3 redirect targets) into WinHTTP's wide-string
 * pieces. ASCII-only conversion is fine: both hosts are always plain ASCII. */
static bool win_parse_url(const char *url, wchar_t *host, size_t host_cap,
                           wchar_t *path, size_t path_cap, INTERNET_PORT *port, bool *https) {
    const char *p = url;
    if (strncmp(p, "https://", 8) == 0) { *https = true; p += 8; }
    else if (strncmp(p, "http://", 7) == 0) { *https = false; p += 7; }
    else return false;

    const char *slash = strchr(p, '/');
    const char *host_end = slash ? slash : p + strlen(p);
    size_t host_len = (size_t)(host_end - p);
    if (host_len == 0 || host_len >= host_cap) return false;
    for (size_t i = 0; i < host_len; i++) host[i] = (wchar_t)(unsigned char)p[i];
    host[host_len] = L'\0';
    *port = *https ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT;

    const char *path_str = slash ? slash : "/";
    size_t path_len = strlen(path_str);
    if (path_len >= path_cap) return false;
    for (size_t i = 0; i <= path_len; i++) path[i] = (wchar_t)(unsigned char)path_str[i];
    return true;
}

/* One request, no auto-follow. If `redirect_out` is non-NULL and the
 * response is a 3xx, the Location header is copied there (UTF-8) and the
 * body is never read. Otherwise the body streams into `mem` or `file_ctx`
 * (whichever is non-NULL; both NULL just drains and discards it, for a
 * pure status/redirect probe). Returns the HTTP status code, or -1 on a
 * transport-level failure. */
static long win_http_request(const char *url, bool send_auth, const char *accept,
                              MemBuf *mem, FileWriteCtx *file_ctx,
                              char *redirect_out, size_t redirect_out_size,
                              char *err, size_t err_size) {
    wchar_t host[256], path[2048];
    INTERNET_PORT port;
    bool https;
    if (!win_parse_url(url, host, 256, path, 2048, &port, &https)) {
        snprintf(err, err_size, "Bad URL");
        return -1;
    }

    HINTERNET hSession = WinHttpOpen(L"EarthBound-Updater/1.0",
                                      WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                      WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) { snprintf(err, err_size, "WinHttpOpen failed (%lu)", GetLastError()); return -1; }
    WinHttpSetTimeouts(hSession, 15000, 15000, 60000, 60000);

    HINTERNET hConnect = WinHttpConnect(hSession, host, port, 0);
    if (!hConnect) {
        snprintf(err, err_size, "WinHttpConnect failed (%lu)", GetLastError());
        WinHttpCloseHandle(hSession);
        return -1;
    }

    DWORD flags = https ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", path, NULL,
                                             WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hRequest) {
        snprintf(err, err_size, "WinHttpOpenRequest failed (%lu)", GetLastError());
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return -1;
    }

    /* Never auto-follow -- see file header's GitHub redirect-quirk note. */
    DWORD disable_redirects = WINHTTP_DISABLE_REDIRECTS;
    WinHttpSetOption(hRequest, WINHTTP_OPTION_DISABLE_FEATURE, &disable_redirects, sizeof(disable_redirects));

    wchar_t headers[1024] = L"";
    if (send_auth) {
        wchar_t token_w[400] = L"";
        MultiByteToWideChar(CP_UTF8, 0, EB_UPDATER_TOKEN_STRING, -1, token_w, 400);
        wchar_t auth_line[512];
        _snwprintf(auth_line, 512, L"Authorization: token %s\r\n", token_w);
        auth_line[511] = L'\0';
        wcsncat(headers, auth_line, (sizeof(headers) / sizeof(wchar_t)) - wcslen(headers) - 1);
    }
    if (accept) {
        wchar_t accept_w[128] = L"";
        MultiByteToWideChar(CP_UTF8, 0, accept, -1, accept_w, 128);
        wchar_t accept_line[160];
        _snwprintf(accept_line, 160, L"Accept: %s\r\n", accept_w);
        accept_line[159] = L'\0';
        wcsncat(headers, accept_line, (sizeof(headers) / sizeof(wchar_t)) - wcslen(headers) - 1);
    }
    wcsncat(headers, L"User-Agent: EarthBound-Updater\r\n",
            (sizeof(headers) / sizeof(wchar_t)) - wcslen(headers) - 1);

    BOOL sent = WinHttpSendRequest(hRequest, headers, (DWORD)-1, WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    if (!sent || !WinHttpReceiveResponse(hRequest, NULL)) {
        snprintf(err, err_size, "Request failed (%lu)", GetLastError());
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return -1;
    }

    DWORD status = 0, status_size = sizeof(status);
    WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size, WINHTTP_NO_HEADER_INDEX);

    if (status >= 300 && status < 400 && redirect_out) {
        wchar_t location[2048];
        DWORD loc_size = sizeof(location);
        if (WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX,
                                 location, &loc_size, WINHTTP_NO_HEADER_INDEX)) {
            WideCharToMultiByte(CP_UTF8, 0, location, -1, redirect_out, (int)redirect_out_size, NULL, NULL);
        } else {
            redirect_out[0] = '\0';
        }
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return (long)status;
    }

    /* Stream the body (if the caller wants it -- a pure status/redirect
     * probe passes mem=file_ctx=NULL and we just discard it below). */
    DWORD total_size = 0;
    {
        DWORD cl = 0, cl_size = sizeof(cl);
        if (WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                                 WINHTTP_HEADER_NAME_BY_INDEX, &cl, &cl_size, WINHTTP_NO_HEADER_INDEX))
            total_size = cl;
    }
    unsigned long long downloaded = 0;
    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(hRequest, &avail) || avail == 0) break;
        char *buf = malloc(avail);
        if (!buf) break;
        DWORD read_bytes = 0;
        if (!WinHttpReadData(hRequest, buf, avail, &read_bytes)) { free(buf); break; }
        if (mem) {
            char *grown = realloc(mem->data, mem->len + read_bytes + 1);
            if (grown) {
                mem->data = grown;
                memcpy(mem->data + mem->len, buf, read_bytes);
                mem->len += read_bytes;
                mem->data[mem->len] = '\0';
            }
        } else if (file_ctx) {
            fwrite(buf, 1, read_bytes, file_ctx->f);
        }
        free(buf);
        downloaded += read_bytes;
        if (file_ctx && file_ctx->progress_percent_out && total_size > 0) {
            int pct = (int)((downloaded * 100) / total_size);
            EbUpdateProgress p;
            SDL_LockMutex(updater_mutex());
            p = g_progress;
            SDL_UnlockMutex(updater_mutex());
            p.progress_percent = pct;
            set_progress(&p);
        }
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return (long)status;
}

static long http_get(const char *url, bool send_auth, const char *accept,
                      MemBuf *mem, FileWriteCtx *file_ctx, char *err, size_t err_size) {
    return win_http_request(url, send_auth, accept, mem, file_ctx, NULL, 0, err, err_size);
}

static bool fetch_release_asset(const char *asset_api_url, MemBuf *mem, FileWriteCtx *file_ctx,
                                 char *err, size_t err_size) {
    char redirect_url[2048] = {0};
    long code = win_http_request(asset_api_url, true, "application/octet-stream",
                                  NULL, NULL, redirect_url, sizeof(redirect_url), err, err_size);

    if (code >= 300 && code < 400 && redirect_url[0]) {
        long final_code = http_get(redirect_url, false, NULL, mem, file_ctx, err, err_size);
        bool ok = (final_code >= 200 && final_code < 300);
        if (!ok && err[0] == '\0')
            snprintf(err, err_size, "download failed (HTTP %ld)", final_code);
        return ok;
    }
    if (code >= 200 && code < 300) {
        /* Unexpected (no redirect) -- re-fetch with the body this time. */
        long final_code = http_get(asset_api_url, true, "application/octet-stream", mem, file_ctx, err, err_size);
        return final_code >= 200 && final_code < 300;
    }
    if (err[0] == '\0')
        snprintf(err, err_size, "unexpected response (HTTP %ld)", code);
    return false;
}

#else /* !_WIN32 */

/* ---- libcurl backend (macOS/Linux) ---- */

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

#endif /* _WIN32 */

/* ------------------------------------------------------------------------
 * Check thread
 * ---------------------------------------------------------------------- */
/* Maintainer-only test hook: EB_UPDATER_TEST_TAG, if set, points every
 * "what's the latest release" lookup at that specific tag instead of the
 * real /releases/latest -- lets a controlled test (e.g. an unpublished
 * pre-release, which /releases/latest itself always skips) exercise the
 * real download/verify/install path without a real tester's app ever
 * seeing it. Unset in normal play; not documented in --help. */
static void build_releases_url(char *out, size_t out_size) {
    const char *test_tag = getenv("EB_UPDATER_TEST_TAG");
    if (test_tag && test_tag[0]) {
        snprintf(out, out_size, "https://api.github.com/repos/%s/releases/tags/%s", EB_UPDATER_REPO_STRING, test_tag);
    } else {
        snprintf(out, out_size, "https://api.github.com/repos/%s/releases/latest", EB_UPDATER_REPO_STRING);
    }
}

static int check_thread_fn(void *unused) {
    (void)unused;
    char url[256];
    build_releases_url(url, sizeof(url));

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
        /* Reuse latest_version to carry the running version too -- by
         * definition they're equal here, and this lets the UI show it
         * without needing a separate platform_update_* accessor. */
        strncpy(p.latest_version, EB_VERSION_STRING, sizeof(p.latest_version) - 1);
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
 * live file at `dest_path` (if any) is never touched on any failure path.
 * On Windows `dest_path` is always a "*.new"/"*.new.exe" staging name (see
 * install step below), so this rename never collides with the running,
 * locked executable. */
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

#if defined(__APPLE__) || defined(_WIN32)
/* Fetches the current release's asset API url for `asset_name` (used for
 * the paired shared library, whose url isn't cached from the AVAILABLE
 * check the way the main binary's is -- see file header). Linux has no
 * paired-library asset at all, so this whole helper is unused there. */
static bool fetch_asset_url_by_name(const char *asset_name, char *out_url, size_t out_url_size) {
    char url[256];
    build_releases_url(url, sizeof(url));
    MemBuf body = {0};
    char err[128] = {0};
    long code = http_get(url, true, "application/vnd.github+json", &body, NULL, err, sizeof(err));
    bool found = false;
    if (code >= 200 && code < 300) {
        cJSON *root = cJSON_ParseWithLength(body.data, body.len);
        if (root) {
            cJSON *assets = cJSON_GetObjectItemCaseSensitive(root, "assets");
            cJSON *asset;
            cJSON_ArrayForEach(asset, assets) {
                cJSON *name = cJSON_GetObjectItemCaseSensitive(asset, "name");
                cJSON *api_url = cJSON_GetObjectItemCaseSensitive(asset, "url");
                if (cJSON_IsString(name) && cJSON_IsString(api_url) &&
                    strcmp(name->valuestring, asset_name) == 0) {
                    snprintf(out_url, out_url_size, "%s", api_url->valuestring);
                    found = true;
                }
            }
            cJSON_Delete(root);
        }
    }
    free(body.data);
    return found;
}
#endif /* __APPLE__ || _WIN32 */

#ifdef _WIN32
/* Writes and launches the detached helper that finishes the install after
 * this process exits -- see file header's "WINDOWS SELF-REPLACE" note.
 * new_exe/new_dll are the already-downloaded-and-verified staging file
 * names ("earthbound.new.exe" / "SDL2.dll.new"); final_exe/final_dll are
 * what they get moved to. Returns false (and sets an error) only if the
 * helper itself couldn't be written or launched -- at that point nothing
 * has touched the live files yet, so it's safe to just report the error. */
static bool win32_launch_relaunch_helper(char *err, size_t err_size) {
    const char *bat_path = "eb_update_helper.bat";
    FILE *f = fopen(bat_path, "w");
    if (!f) {
        snprintf(err, err_size, "Couldn't write update helper");
        return false;
    }
    /* `ping -n 2 127.0.0.1` is a ~1s delay that works even when this batch
     * file's own console isn't interactive (unlike `timeout`, which can
     * misbehave with redirected/absent stdin) -- a common, well-known
     * batch-scripting substitute for `sleep`. The move is itself retried
     * in case the OS hasn't fully released the file handle the instant
     * tasklist stops reporting our PID. */
    fprintf(f,
        "@echo off\r\n"
        "set PID=%%1\r\n"
        ":waitloop\r\n"
        "tasklist /FI \"PID eq %%PID%%\" 2>NUL | find /I \"%%PID%%\" >NUL\r\n"
        "if \"%%ERRORLEVEL%%\"==\"0\" (\r\n"
        "    ping -n 2 127.0.0.1 >NUL\r\n"
        "    goto waitloop\r\n"
        ")\r\n"
        ":moveloop\r\n"
        "move /Y \"earthbound.new.exe\" \"earthbound.exe\" >NUL 2>&1\r\n"
        "if exist \"earthbound.new.exe\" (\r\n"
        "    ping -n 2 127.0.0.1 >NUL\r\n"
        "    goto moveloop\r\n"
        ")\r\n"
        "if exist \"%s.new\" move /Y \"%s.new\" \"%s\" >NUL 2>&1\r\n"
        "start \"\" \"earthbound.exe\"\r\n"
        "del \"%%~f0\"\r\n",
        EB_UPDATER_DLL_NAME, EB_UPDATER_DLL_NAME, EB_UPDATER_DLL_NAME);
    fclose(f);

    char cmdline[256];
    snprintf(cmdline, sizeof(cmdline), "cmd.exe /c \"%s\" %lu", bat_path, (unsigned long)GetCurrentProcessId());

    STARTUPINFOA si = { .cb = sizeof(si) };
    PROCESS_INFORMATION pi;
    BOOL ok = CreateProcessA(NULL, cmdline, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    if (!ok) {
        snprintf(err, err_size, "Couldn't launch update helper (%lu)", GetLastError());
        remove(bat_path);
        return false;
    }
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return true;
}
#endif

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

    /* checksums_pristine is NEVER passed directly to find_checksum() --
     * only fresh strdup()'d scratch copies are, one per search. Bug found
     * live (confirmed failing identically on both Windows and macOS
     * before this fix): find_checksum() uses strtok(), which inserts a
     * NUL at the *first* delimiter it scans past on its very first call,
     * regardless of which line the eventual match is on -- so by the time
     * the first search returns, the buffer it was given is already
     * truncated to just its first line. The old code passed
     * checksums_copy to the first search directly, THEN strdup()'d the
     * now-truncated result for the second search, silently losing every
     * line but the first from that point on. */
    char *checksums_pristine = malloc(checksums.len + 1);
    if (!checksums_pristine) { set_error("Out of memory"); free(checksums.data); return 0; }
    memcpy(checksums_pristine, checksums.data, checksums.len);
    checksums_pristine[checksums.len] = '\0';
    free(checksums.data);

    char expected_hex[65];
    char *scratch1 = strdup(checksums_pristine);
    const char *found = scratch1 ? find_checksum(scratch1, EB_UPDATER_ASSET_NAME) : NULL;
    if (found) snprintf(expected_hex, sizeof(expected_hex), "%s", found);
    free(scratch1);
#if defined(__APPLE__) || defined(_WIN32)
    char expected_lib_hex[65];
#if defined(__APPLE__)
#define EB_UPDATER_LIB_NAME EB_UPDATER_DYLIB_NAME
#else
#define EB_UPDATER_LIB_NAME EB_UPDATER_DLL_NAME
#endif
    char *scratch2 = strdup(checksums_pristine);
    const char *found_lib = scratch2 ? find_checksum(scratch2, EB_UPDATER_LIB_NAME) : NULL;
    if (found_lib) snprintf(expected_lib_hex, sizeof(expected_lib_hex), "%s", found_lib);
    free(scratch2);
#endif
    free(checksums_pristine);

    if (!found) { set_error("Checksum manifest incomplete"); return 0; }
#if defined(__APPLE__) || defined(_WIN32)
    if (!found_lib) { set_error("Checksum manifest incomplete"); return 0; }
#endif

    /* 2. The binary itself, staged beside the running executable (CWD is
     * already the executable's own directory -- see
     * chdir_to_executable_dir() in main.c, called before anything else).
     * Windows stages under a name distinct from the locked live .exe;
     * POSIX stages "earthbound.new" and rename()s over the live file
     * directly (see download_and_verify()'s own header comment). */
#ifdef _WIN32
    const char *new_exe_name = "earthbound.new.exe";
#else
    const char *new_exe_name = "earthbound.new";
#endif
    if (!download_and_verify(g_asset_api_url, new_exe_name, expected_hex, err, sizeof(err))) {
        set_error(err[0] ? err : "Download failed");
        return 0;
    }

#if defined(__APPLE__) || defined(_WIN32)
    /* 3. macOS/Windows also ship a paired shared library -- fetched the
     * same way, by re-resolving its asset url (the AVAILABLE check only
     * cached the platform binary + checksums URLs). Rarely changes
     * version-to-version, but verified+replaced every update regardless,
     * for correctness. */
    char lib_asset_url[512] = {0};
    fetch_asset_url_by_name(EB_UPDATER_LIB_NAME, lib_asset_url, sizeof(lib_asset_url));
    if (!lib_asset_url[0]) { set_error("No library asset in release"); remove(new_exe_name); return 0; }
    char new_lib_name[128];
    snprintf(new_lib_name, sizeof(new_lib_name), "%s.new", EB_UPDATER_LIB_NAME);
    if (!download_and_verify(lib_asset_url, new_lib_name, expected_lib_hex, err, sizeof(err))) {
        set_error(err[0] ? err : "Library download failed");
        remove(new_exe_name);
        return 0;
    }
#endif

    /* 3.5. This binary's own compiled-in data (if it has any -- see
     * pak_export.h's comment) is the one thing the *new* binary being
     * installed below can't get any other way. Exporting it now, before
     * the swap, means an existing tester updating from an old
     * compile-time-embedded build to this project's current
     * EB_RUNTIME_ASSETS build needs no ROM and no bundled setup helper --
     * the new binary just finds a ready-made, already-correct pak waiting
     * for it on first launch. A no-op (empty function body, returns true)
     * in an EB_RUNTIME_ASSETS=ON build, since that binary never had real
     * data to export in the first place, so this branch only ever runs in
     * a real compile-time-embedded build. This is NOT best-effort: unlike
     * a ROM-only install, a tester on an embedded build has no ROM sitting
     * next to it by definition (they never needed one), so main.c's
     * ROM-scan fallback doesn't apply to them -- if this export fails,
     * there is no other path to correct assets, and swapping the binary
     * anyway would strand them on a build with no data at all. Abort the
     * update instead, leaving the working old binary in place. */
#ifndef EB_RUNTIME_ASSETS
    {
        char pak_path[1024];
        if (!eb_runtime_assets_default_path(pak_path, sizeof(pak_path)) || !pak_export_write(pak_path)) {
            set_error("Couldn't prepare game data for the update -- try again, or check disk space");
            remove(new_exe_name);
#if defined(__APPLE__) || defined(_WIN32)
            remove(new_lib_name);
#endif
            return 0;
        }
    }
#endif

#ifdef _WIN32
    /* 4. Install (Windows): can't touch the live, locked .exe from this
     * process at all -- hand off to a detached helper that waits for us
     * to exit, then does the actual swap. See file header. */
    if (!win32_launch_relaunch_helper(err, sizeof(err))) {
        set_error(err[0] ? err : "Couldn't stage install");
        remove(new_exe_name);
        remove(new_lib_name);
        return 0;
    }
    EbUpdateProgress p = {0};
    p.status = EB_UPDATE_DONE;
    set_progress(&p);
    platform_request_quit();
    return 0;
#else
    /* 4. Install (macOS/Linux): swap the new file(s) into place. POSIX
     * allows replacing a file that's currently executing -- the running
     * process keeps its old inode open until it exits, so this is safe to
     * do while running. */
    chmod(new_exe_name, 0755);
    if (rename(new_exe_name, "earthbound") != 0) {
        char msg[64];
        snprintf(msg, sizeof(msg), "Couldn't install: %s", strerror(errno));
        set_error(msg);
        return 0;
    }
#ifdef __APPLE__
    if (rename(new_lib_name, EB_UPDATER_LIB_NAME) != 0) {
        /* The main binary already swapped -- this is a genuinely bad state
         * (mismatched binary/dylib pair), but there's no safe rollback of
         * the rename() above. Surface it clearly; the player's next launch
         * will fail to load the dylib and needs a fresh download. */
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
#endif
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
