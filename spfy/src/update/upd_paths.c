/* Where the checker keeps its state, and where it lives on disk.
 *
 * Part of spfy_update_trigger: no network, no parser, nothing the SAPI DLL
 * cannot afford to call while a screen reader is speaking.
 */

#include "spfy_update.h"
#include "upd_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN 1
#  endif
#  include <windows.h>
#  include <direct.h>
#  define SPFY_UPD_SEP '\\'
#else
#  include <sys/stat.h>
#  include <sys/types.h>
#  include <unistd.h>
#  define SPFY_UPD_SEP '/'
#endif

int spfy_upd_mkdir(const char *path)
{
#ifdef _WIN32
    if (CreateDirectoryA(path, NULL)) return 0;
    return (GetLastError() == ERROR_ALREADY_EXISTS) ? 0 : -1;
#else
    if (mkdir(path, 0700) == 0) return 0;
    {
        struct stat st;
        if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) return 0;
    }
    return -1;
#endif
}

int spfy_upd_file_stat(const char *path, long long *bytes, long long *mtime)
{
#ifdef _WIN32
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &fad)) return -1;
    if (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) return -1;
    if (bytes)
        *bytes = ((long long)fad.nFileSizeHigh << 32) |
                 (long long)fad.nFileSizeLow;
    if (mtime) {
        /* FILETIME is 100 ns ticks since 1601; unix seconds are all the
         * stamp cache needs and they survive a rebuild of this struct. */
        unsigned long long t =
            ((unsigned long long)fad.ftLastWriteTime.dwHighDateTime << 32) |
             (unsigned long long)fad.ftLastWriteTime.dwLowDateTime;
        *mtime = (long long)(t / 10000000ull) - 11644473600ll;
    }
    return 0;
#else
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    if (!S_ISREG(st.st_mode)) return -1;
    if (bytes) *bytes = (long long)st.st_size;
    if (mtime) *mtime = (long long)st.st_mtime;
    return 0;
#endif
}

int spfy_upd_state_dir(char *buf, size_t buf_n)
{
#ifdef _WIN32
    const char *base = getenv("LOCALAPPDATA");
    char fallback[MAX_PATH];

    if (!base || !*base) {
        const char *up = getenv("USERPROFILE");
        if (up && *up) {
            _snprintf(fallback, sizeof fallback - 1,
                      "%s\\AppData\\Local", up);
            fallback[sizeof fallback - 1] = '\0';
            base = fallback;
        } else {
            base = getenv("TEMP");
        }
    }
    if (!base || !*base) return -1;
    if (_snprintf(buf, buf_n - 1, "%s\\Speechify", base) < 0) return -1;
    buf[buf_n - 1] = '\0';
#else
    const char *base = getenv("XDG_STATE_HOME");
    char fallback[1024];

    if (!base || !*base) {
        const char *home = getenv("HOME");
        if (!home || !*home) return -1;
        snprintf(fallback, sizeof fallback, "%s/.local/state", home);
        base = fallback;
        /* ~/.local and ~/.local/state may not exist yet. */
        {
            char up[1024];
            snprintf(up, sizeof up, "%s/.local", home);
            (void)spfy_upd_mkdir(up);
            (void)spfy_upd_mkdir(base);
        }
    }
    if (snprintf(buf, buf_n, "%s/spfy", base) >= (int)buf_n) return -1;
#endif
    return spfy_upd_mkdir(buf);
}

int spfy_upd_state_path(char *buf, size_t buf_n, const char *leaf)
{
    char dir[1024];
    if (spfy_upd_state_dir(dir, sizeof dir) != 0) return -1;
    if (strlen(dir) + strlen(leaf) + 2 > buf_n) return -1;
    sprintf(buf, "%s%c%s", dir, SPFY_UPD_SEP, leaf);
    return 0;
}

#ifdef _WIN32
/* An address inside THIS module for GetModuleHandleEx to resolve.
 * A static datum, not the function itself: casting a function pointer to
 * LPCSTR is a constraint violation ISO C does not allow (-Wpedantic says so),
 * and FROM_ADDRESS is satisfied by any address in the module's image. */
static const char upd_module_anchor = 0;
#endif

int spfy_upd_self_dir(char *buf, size_t buf_n)
{
#ifdef _WIN32
    HMODULE self = NULL;
    char path[MAX_PATH];
    DWORD n;
    char *slash;

    /* FROM_ADDRESS so this resolves the SAPI DLL when linked into it and the
     * .exe when linked into that -- GetModuleHandle(NULL) would name the
     * HOST process (Narrator, Balabolka) and send us looking for the helper
     * in System32. UNCHANGED_REFCOUNT: we must not hold a reference on our
     * own module. */
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)&upd_module_anchor, &self))
        return -1;
    n = GetModuleFileNameA(self, path, (DWORD)sizeof path);
    if (n == 0 || n >= sizeof path) return -1;
    slash = strrchr(path, '\\');
    if (!slash) return -1;
    *slash = '\0';
    if (strlen(path) + 1 > buf_n) return -1;
    strcpy(buf, path);
    return 0;
#else
    ssize_t n = readlink("/proc/self/exe", buf, buf_n - 1);
    if (n <= 0) {
        /* macOS and the BSDs have no /proc; the helper is only ever looked
         * for beside us on Windows, so failing here is harmless. */
        return -1;
    }
    buf[n] = '\0';
    {
        char *slash = strrchr(buf, '/');
        if (!slash) return -1;
        *slash = '\0';
    }
    return 0;
#endif
}
