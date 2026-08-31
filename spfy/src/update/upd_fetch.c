/* One HTTPS GET, and nothing that outlives it.
 *
 * Windows: WinHTTP, loaded with LoadLibrary rather than linked.
 *
 *   ⚠ THE DYNAMIC LOAD IS NOT OPTIONAL. spfy_synth.exe is an i686 binary
 *   whose import table deliberately reaches no further than msvcrt and
 *   min-OS 4.0 -- that is what lets the x86 installer target Windows a
 *   generation older than the x64 one. A static -lwinhttp puts winhttp.dll
 *   in the import table, and then the WHOLE CLI refuses to start on a
 *   machine that lacks it, over a feature that is meant to be invisible.
 *   Loaded this way, a missing winhttp.dll costs exactly one skipped check.
 *
 * Elsewhere: curl, then wget, fork+exec'd with an argv -- never a shell, so
 * a URL out of the environment cannot smuggle a command into it.
 *
 * A `file://` URL (or a bare existing path) is read straight off disk. That
 * is how the whole pipeline is tested without a server.
 */

#include "spfy_update.h"
#include "upd_internal.h"
#include "env.h"
#include "spfy/version.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN 1
#  endif
#  include <windows.h>
#  include <winhttp.h>
#else
#  include <errno.h>
#  include <fcntl.h>
#  include <sys/types.h>
#  include <sys/wait.h>
#  include <unistd.h>
#endif

/* A manifest is a few KB. Anything past this is not ours and is not worth
 * the memory. */
#define UPD_MAX_BODY (4u * 1024u * 1024u)

const char *spfy_upd_url(void)
{
    const char *ev = spfy_env("SPFY_UPDATE_URL");
    if (ev && *ev) return ev;
    return SPFY_UPDATE_URL;
}

static int read_local_file(const char *path, char **out, size_t *out_n)
{
    FILE *fp = fopen(path, "rb");
    long sz;
    char *buf;

    if (!fp) return -1;
    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return -1; }
    sz = ftell(fp);
    if (sz < 0 || (unsigned long)sz > UPD_MAX_BODY) { fclose(fp); return -1; }
    if (fseek(fp, 0, SEEK_SET) != 0) { fclose(fp); return -1; }
    buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(fp); return -1; }
    if (fread(buf, 1, (size_t)sz, fp) != (size_t)sz) {
        free(buf); fclose(fp); return -1;
    }
    fclose(fp);
    buf[sz] = '\0';
    *out = buf;
    if (out_n) *out_n = (size_t)sz;
    return 0;
}

/* file:///C:/x/y.json -> C:/x/y.json ; file:///tmp/y.json -> /tmp/y.json */
static int try_local(const char *url, char **out, size_t *out_n)
{
    if (strncmp(url, "file://", 7) == 0) {
        const char *p = url + 7;
        if (*p == '/' && p[1] && p[2] == ':') p++;   /* file:///C:/... */
        return read_local_file(p, out, out_n);
    }
    if (strstr(url, "://") == NULL)
        return read_local_file(url, out, out_n);
    return -1;
}

#ifdef _WIN32

typedef HINTERNET (WINAPI *pfn_open)(LPCWSTR, DWORD, LPCWSTR, LPCWSTR, DWORD);
typedef HINTERNET (WINAPI *pfn_connect)(HINTERNET, LPCWSTR, INTERNET_PORT, DWORD);
typedef HINTERNET (WINAPI *pfn_openreq)(HINTERNET, LPCWSTR, LPCWSTR, LPCWSTR,
                                        LPCWSTR, LPCWSTR *, DWORD);
typedef BOOL (WINAPI *pfn_send)(HINTERNET, LPCWSTR, DWORD, LPVOID, DWORD,
                                DWORD, DWORD_PTR);
typedef BOOL (WINAPI *pfn_recv)(HINTERNET, LPVOID);
typedef BOOL (WINAPI *pfn_avail)(HINTERNET, LPDWORD);
typedef BOOL (WINAPI *pfn_read)(HINTERNET, LPVOID, DWORD, LPDWORD);
typedef BOOL (WINAPI *pfn_close)(HINTERNET);
typedef BOOL (WINAPI *pfn_crack)(LPCWSTR, DWORD, DWORD, LPURL_COMPONENTS);
typedef BOOL (WINAPI *pfn_timeouts)(HINTERNET, int, int, int, int);
typedef BOOL (WINAPI *pfn_qhdr)(HINTERNET, DWORD, LPCWSTR, LPVOID, LPDWORD,
                                LPDWORD);

struct winhttp_api {
    HMODULE       dll;
    pfn_open      Open;
    pfn_connect   Connect;
    pfn_openreq   OpenRequest;
    pfn_send      SendRequest;
    pfn_recv      ReceiveResponse;
    pfn_avail     QueryDataAvailable;
    pfn_read      ReadData;
    pfn_close     CloseHandle_;
    pfn_crack     CrackUrl;
    pfn_timeouts  SetTimeouts;
    pfn_qhdr      QueryHeaders;
};

static int winhttp_load(struct winhttp_api *w)
{
    memset(w, 0, sizeof *w);
    w->dll = LoadLibraryA("winhttp.dll");
    if (!w->dll) return -1;

#define GETP(field, name) \
    do { \
        FARPROC _p = GetProcAddress(w->dll, name); \
        if (!_p) { FreeLibrary(w->dll); w->dll = NULL; return -1; } \
        *(FARPROC *)&w->field = _p; \
    } while (0)

    GETP(Open,               "WinHttpOpen");
    GETP(Connect,            "WinHttpConnect");
    GETP(OpenRequest,        "WinHttpOpenRequest");
    GETP(SendRequest,        "WinHttpSendRequest");
    GETP(ReceiveResponse,    "WinHttpReceiveResponse");
    GETP(QueryDataAvailable, "WinHttpQueryDataAvailable");
    GETP(ReadData,           "WinHttpReadData");
    GETP(CloseHandle_,       "WinHttpCloseHandle");
    GETP(CrackUrl,           "WinHttpCrackUrl");
    GETP(SetTimeouts,        "WinHttpSetTimeouts");
    GETP(QueryHeaders,       "WinHttpQueryHeaders");
#undef GETP
    return 0;
}

/* `sink`, when non-NULL, sends the body straight to a file instead of a
 * malloc'd buffer, and `out`/`out_n` are then unused.
 *
 * ⚠ THE BUFFERED PATH CANNOT DOWNLOAD A VOICE. UPD_MAX_BODY is 4 MB, which is
 * right for a manifest and three orders of magnitude short of a 92 MB voice
 * zip. Rather than duplicate sixty lines of WinHTTP bring-up for the file
 * case, the same request grows one branch in its read loop. */
static int fetch_win(const char *url, int timeout_s, char **out, size_t *out_n,
                     FILE *sink, spfy_upd_progress_fn on_progress, void *pctx)
{
    struct winhttp_api w;
    WCHAR wurl[2048], host[256], upath[1536], extra[1024], obj[2560];
    WCHAR agent[64];
    URL_COMPONENTS uc;
    HINTERNET hs = NULL, hc = NULL, hr = NULL;
    char *buf = NULL;
    size_t cap = 0, len = 0;
    int rc = -1;
    DWORD status = 0, status_n = sizeof status;
    int ms = timeout_s > 0 ? timeout_s * 1000 : 20000;

    if (MultiByteToWideChar(CP_UTF8, 0, url, -1, wurl,
                            (int)(sizeof wurl / sizeof wurl[0])) == 0)
        return -1;
    if (winhttp_load(&w) != 0) return -1;

    memset(&uc, 0, sizeof uc);
    uc.dwStructSize      = sizeof uc;
    uc.lpszHostName      = host;   uc.dwHostNameLength   = 256;
    uc.lpszUrlPath       = upath;  uc.dwUrlPathLength    = 1536;
    uc.lpszExtraInfo     = extra;  uc.dwExtraInfoLength  = 1024;
    if (!w.CrackUrl(wurl, 0, 0, &uc)) goto done;

    _snwprintf(agent, sizeof agent / sizeof agent[0] - 1,
               L"spfy-update/%hs", SPFY_VERSION);
    _snwprintf(obj, sizeof obj / sizeof obj[0] - 1, L"%ls%ls", upath, extra);

    hs = w.Open(agent, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hs) goto done;
    w.SetTimeouts(hs, ms, ms, ms, ms);

    hc = w.Connect(hs, host, uc.nPort, 0);
    if (!hc) goto done;

    hr = w.OpenRequest(hc, L"GET", obj, NULL, WINHTTP_NO_REFERER,
                       WINHTTP_DEFAULT_ACCEPT_TYPES,
                       (uc.nScheme == INTERNET_SCHEME_HTTPS)
                           ? WINHTTP_FLAG_SECURE : 0);
    if (!hr) goto done;

    /* Redirects are followed by default (github.com -> the signed asset
     * host), which is the entire reason a release-download URL works here. */
    if (!w.SendRequest(hr, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                       WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) goto done;
    if (!w.ReceiveResponse(hr, NULL)) goto done;

    if (!w.QueryHeaders(hr, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_n,
                        WINHTTP_NO_HEADER_INDEX))
        goto done;
    if (status != 200) goto done;

    if (sink) {
        /* Content-Length is advisory here: it is only used to drive a
         * progress figure, and a server that omits it just means the caller
         * shows bytes rather than a percentage. */
        DWORD clen = 0, clen_n = sizeof clen;
        unsigned long long total = 0, done_n = 0;
        char sbuf[65536];
        if (w.QueryHeaders(hr, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                           WINHTTP_HEADER_NAME_BY_INDEX, &clen, &clen_n,
                           WINHTTP_NO_HEADER_INDEX))
            total = clen;
        for (;;) {
            DWORD avail = 0, got = 0;
            if (!w.QueryDataAvailable(hr, &avail)) goto done;
            if (avail == 0) break;
            if (avail > sizeof sbuf) avail = sizeof sbuf;
            if (!w.ReadData(hr, sbuf, avail, &got)) goto done;
            if (got == 0) break;
            if (fwrite(sbuf, 1, got, sink) != got) goto done;
            done_n += got;
            if (on_progress && on_progress(pctx, done_n, total) != 0) goto done;
        }
        rc = 0;
        goto done;
    }

    for (;;) {
        DWORD avail = 0, got = 0;
        if (!w.QueryDataAvailable(hr, &avail)) goto done;
        if (avail == 0) break;
        if (len + avail + 1 > cap) {
            size_t want = len + avail + 1;
            char *nb;
            if (want > UPD_MAX_BODY) goto done;
            cap = want * 2 > UPD_MAX_BODY ? UPD_MAX_BODY : want * 2;
            nb = (char *)realloc(buf, cap);
            if (!nb) goto done;
            buf = nb;
        }
        if (!w.ReadData(hr, buf + len, avail, &got)) goto done;
        if (got == 0) break;
        len += got;
    }
    if (!buf) goto done;
    buf[len] = '\0';
    *out = buf;
    if (out_n) *out_n = len;
    buf = NULL;
    rc = 0;

done:
    if (hr) w.CloseHandle_(hr);
    if (hc) w.CloseHandle_(hc);
    if (hs) w.CloseHandle_(hs);
    free(buf);
    if (w.dll) FreeLibrary(w.dll);
    return rc;
}

#else  /* !_WIN32 */

/* Run `argv` with stdout on a pipe and collect it. No shell anywhere. */
static int run_capture(char *const argv[], char **out, size_t *out_n)
{
    int fd[2];
    pid_t pid;
    char *buf = NULL;
    size_t cap = 0, len = 0;
    int status = 0;

    if (pipe(fd) != 0) return -1;
    pid = fork();
    if (pid < 0) { close(fd[0]); close(fd[1]); return -1; }
    if (pid == 0) {
        int devnull = open("/dev/null", O_RDWR);
        close(fd[0]);
        if (devnull >= 0) { dup2(devnull, 0); dup2(devnull, 2); }
        dup2(fd[1], 1);
        close(fd[1]);
        if (devnull > 2) close(devnull);
        execvp(argv[0], argv);
        _exit(127);
    }
    close(fd[1]);
    for (;;) {
        ssize_t got;
        if (len + 65536 + 1 > cap) {
            size_t want = len + 65536 + 1;
            char *nb;
            if (want > UPD_MAX_BODY) { free(buf); buf = NULL; break; }
            cap = want;
            nb = (char *)realloc(buf, cap);
            if (!nb) { free(buf); buf = NULL; break; }
            buf = nb;
        }
        got = read(fd[0], buf + len, 65536);
        if (got < 0) {
            if (errno == EINTR) continue;
            free(buf); buf = NULL; break;
        }
        if (got == 0) break;
        len += (size_t)got;
    }
    close(fd[0]);
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
        ;
    if (!buf) return -1;
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) { free(buf); return -1; }
    buf[len] = '\0';
    *out = buf;
    if (out_n) *out_n = len;
    return 0;
}

static int fetch_unix(const char *url, int timeout_s, char **out, size_t *out_n)
{
    char tmo[32];
    char agent[64];
    char *curl_argv[10];
    char *wget_argv[10];
    int i;

    snprintf(tmo, sizeof tmo, "%d", timeout_s > 0 ? timeout_s : 20);
    snprintf(agent, sizeof agent, "spfy-update/%s", SPFY_VERSION);

    i = 0;
    curl_argv[i++] = (char *)"curl";
    curl_argv[i++] = (char *)"-fsSL";
    curl_argv[i++] = (char *)"--max-time";
    curl_argv[i++] = tmo;
    curl_argv[i++] = (char *)"-A";
    curl_argv[i++] = agent;
    curl_argv[i++] = (char *)"--";
    curl_argv[i++] = (char *)url;
    curl_argv[i]   = NULL;
    if (run_capture(curl_argv, out, out_n) == 0) return 0;

    i = 0;
    wget_argv[i++] = (char *)"wget";
    wget_argv[i++] = (char *)"-qO-";
    wget_argv[i++] = (char *)"--timeout";
    wget_argv[i++] = tmo;
    wget_argv[i++] = (char *)"-U";
    wget_argv[i++] = agent;
    wget_argv[i++] = (char *)"--";
    wget_argv[i++] = (char *)url;
    wget_argv[i]   = NULL;
    return run_capture(wget_argv, out, out_n);
}

/* Straight to a file. curl and wget both write one themselves, so there is
 * no pipe to drain and no body cap to hit -- the whole reason the buffered
 * path cannot be reused for a 92 MB voice.
 *
 * No progress callback on this path: parsing curl's progress meter back out
 * of a pipe would be guesswork, and `-#` is not machine-readable. The caller
 * prints "downloading..." and the tool prints nothing. */
static int fetch_unix_file(const char *url, int timeout_s, const char *dest)
{
    char tmo[32];
    char agent[64];
    char *argv[12];
    char *out = NULL;
    size_t n = 0;
    int i, rc;

    snprintf(tmo, sizeof tmo, "%d", timeout_s > 0 ? timeout_s : 600);
    snprintf(agent, sizeof agent, "spfy-update/%s", SPFY_VERSION);

    i = 0;
    argv[i++] = (char *)"curl";
    argv[i++] = (char *)"-fsSL";
    argv[i++] = (char *)"--max-time";
    argv[i++] = tmo;
    argv[i++] = (char *)"-A";
    argv[i++] = agent;
    argv[i++] = (char *)"-o";
    argv[i++] = (char *)dest;
    argv[i++] = (char *)"--";
    argv[i++] = (char *)url;
    argv[i]   = NULL;
    rc = run_capture(argv, &out, &n);
    free(out); out = NULL;
    if (rc == 0) return 0;

    i = 0;
    argv[i++] = (char *)"wget";
    argv[i++] = (char *)"-q";
    argv[i++] = (char *)"--timeout";
    argv[i++] = tmo;
    argv[i++] = (char *)"-U";
    argv[i++] = agent;
    argv[i++] = (char *)"-O";
    argv[i++] = (char *)dest;
    argv[i++] = (char *)"--";
    argv[i++] = (char *)url;
    argv[i]   = NULL;
    rc = run_capture(argv, &out, &n);
    free(out);
    return rc;
}

#endif /* _WIN32 */

int spfy_upd_fetch_file(const char *url, int timeout_s, const char *dest,
                        spfy_upd_progress_fn on_progress, void *pctx)
{
    if (!url || !*url || !dest || !*dest) return -1;
    if (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0)
        return -1;
#ifdef _WIN32
    {
        /* Write to a .part and rename on success. A half-written file left
         * under the real name is one an interrupted run would happily hand
         * to the unzipper next time. */
        char part[1024];
        FILE *fp;
        int rc;
        snprintf(part, sizeof part, "%s.part", dest);
        fp = fopen(part, "wb");
        if (!fp) return -1;
        rc = fetch_win(url, timeout_s, NULL, NULL, fp, on_progress, pctx);
        if (fclose(fp) != 0) rc = -1;
        if (rc != 0) { remove(part); return -1; }
        remove(dest);
        if (rename(part, dest) != 0) { remove(part); return -1; }
        return 0;
    }
#else
    (void)on_progress; (void)pctx;
    {
        char part[1024];
        int rc;
        snprintf(part, sizeof part, "%s.part", dest);
        rc = fetch_unix_file(url, timeout_s, part);
        if (rc != 0) { remove(part); return -1; }
        remove(dest);
        if (rename(part, dest) != 0) { remove(part); return -1; }
        return 0;
    }
#endif
}

int spfy_upd_fetch(const char *url, int timeout_s, char **out, size_t *out_n)
{
    if (!url || !*url || !out) return -1;
    *out = NULL;
    if (out_n) *out_n = 0;

    if (try_local(url, out, out_n) == 0) return 0;
    if (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0)
        return -1;

#ifdef _WIN32
    return fetch_win(url, timeout_s, out, out_n, NULL, NULL, NULL);
#else
    return fetch_unix(url, timeout_s, out, out_n);
#endif
}
