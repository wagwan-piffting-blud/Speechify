/* How the result reaches the user.
 *
 * Console: five lines, once, on a stream the caller picks.
 *
 * Windows GUI: a TRAY BALLOON, never a MessageBox. The check can fire while
 * a screen reader is mid-sentence -- the SAPI path is the whole reason this
 * feature has a GUI at all -- and a modal dialog would steal focus from the
 * application the user is reading. A balloon (a toast on 10/11) takes no
 * focus, is announced by Narrator and NVDA on its own, and disappears by
 * itself. If it cannot be shown at all, nothing is shown: the pending result
 * still sits in update_state.json and spfy_synth prints it next run.
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
#  include <shellapi.h>
#  ifndef NIN_BALLOONUSERCLICK
#    define NIN_BALLOONUSERCLICK (WM_USER + 5)
#  endif
#  ifndef NIN_BALLOONTIMEOUT
#    define NIN_BALLOONTIMEOUT   (WM_USER + 4)
#  endif
#  ifndef NIIF_INFO
#    define NIIF_INFO 0x00000001
#  endif
#  define UPD_TRAY_MSG (WM_APP + 71)
#else
#  include <sys/types.h>
#  include <unistd.h>
#endif

static const char *reason_text(int reason)
{
    switch (reason) {
    case SPFY_UPD_R_SIZE:
    case SPFY_UPD_R_HASH:    return "rebuilt";
    case SPFY_UPD_R_MISSING: return "incomplete";
    case SPFY_UPD_R_VERSION: return "newer";
    default:                 return "";
    }
}

static void human_size(long long bytes, char *out, size_t out_n)
{
    if (bytes <= 0) { spfy_upd_strlcpy(out, "", out_n); return; }
    if (bytes >= 1024LL * 1024LL)
        snprintf(out, out_n, "%.0f MB", (double)bytes / (1024.0 * 1024.0));
    else
        snprintf(out, out_n, "%.0f KB", (double)bytes / 1024.0);
}

void spfy_upd_notify_console(const spfy_upd_report *rep, FILE *fp)
{
    int i;

    if (!fp) return;
    if (!rep->engine_update && rep->n_voices == 0) return;

    fprintf(fp, "\n-- Speechify update available ------------------------------\n");
    if (rep->engine_update)
        fprintf(fp, "   engine   %s  ->  %s\n",
                rep->local_version[0] ? rep->local_version : "?",
                rep->remote_version);
    for (i = 0; i < rep->n_voices; i++) {
        char sz[32];
        human_size(rep->voices[i].zip_bytes, sz, sizeof sz);
        fprintf(fp, "   voice    %-12s %-6s %s %s\n",
                rep->voices[i].display,
                rep->voices[i].lang,
                reason_text(rep->voices[i].reason),
                rep->voices[i].remote_version);
        if (rep->voices[i].url[0]) {
            fprintf(fp, "            %s", rep->voices[i].url);
            if (sz[0]) fprintf(fp, "  (%s)", sz);
            fputc('\n', fp);
        }
    }
    if (rep->message[0])
        fprintf(fp, "   %s\n", rep->message);
    if (rep->engine_update && rep->engine_url[0])
        fprintf(fp, "   %s\n", rep->engine_url);
    fprintf(fp, "   (silence with SPFY_NO_UPDATE_CHECK=1 or --no-update-check)\n");
    fprintf(fp, "------------------------------------------------------------\n");
    fflush(fp);
}

int spfy_upd_console_visible(void)
{
#ifdef _WIN32
    /* No console window means nobody is going to read a printed line. That is
     * exactly the case when spfy_sapi64.dll spawns spfy_synth.exe to render
     * for a 64-bit SAPI client (Narrator on x64): the process has stdio, but
     * it goes to a pipe the shim drains for audio. */
    return GetConsoleWindow() != NULL;
#else
    /* Nothing else has a tray to fall back to, so printing is all there is. */
    return 1;
#endif
}

void spfy_upd_open_url(const char *url)
{
    if (!url || !*url) return;
#ifdef _WIN32
    ShellExecuteA(NULL, "open", url, NULL, NULL, SW_SHOWNORMAL);
#else
    {
        pid_t pid = fork();
        if (pid == 0) {
            execlp("xdg-open", "xdg-open", url, (char *)NULL);
            execlp("open", "open", url, (char *)NULL);
            _exit(127);
        }
    }
#endif
}

#ifdef _WIN32

static char g_click_url[512];

static LRESULT CALLBACK tray_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == UPD_TRAY_MSG) {
        if (LOWORD(lp) == NIN_BALLOONUSERCLICK) {
            spfy_upd_open_url(g_click_url);
            PostQuitMessage(0);
        } else if (LOWORD(lp) == NIN_BALLOONTIMEOUT) {
            PostQuitMessage(0);
        }
        return 0;
    }
    if (msg == WM_TIMER) { PostQuitMessage(0); return 0; }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

int spfy_upd_notify_gui(const spfy_upd_report *rep)
{
    WNDCLASSA wc;
    HWND hwnd;
    NOTIFYICONDATAA nid;
    MSG msg;
    char body[256] = {0};
    size_t used = 0;
    int i;

    if (!rep->engine_update && rep->n_voices == 0) return -1;

    if (rep->engine_update) {
        int n = snprintf(body, sizeof body, "Engine %s is available.",
                         rep->remote_version);
        if (n > 0) used = (size_t)n;
    }
    for (i = 0; i < rep->n_voices && used + 1 < sizeof body; i++) {
        int n = snprintf(body + used, sizeof body - used, "%s%s %s updated.",
                         used ? "\n" : "",
                         rep->voices[i].display,
                         rep->voices[i].remote_version);
        if (n <= 0) break;
        used += (size_t)n;
    }
    spfy_upd_strlcpy(g_click_url,
                     rep->n_voices > 0 && rep->voices[0].url[0]
                         ? rep->voices[0].url : rep->engine_url,
                     sizeof g_click_url);

    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc   = tray_proc;
    wc.hInstance     = GetModuleHandleA(NULL);
    wc.lpszClassName = "SpfyUpdateTray";
    if (!RegisterClassA(&wc) &&
        GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return -1;

    /* HWND_MESSAGE: no taskbar entry, no z-order, nothing to activate. */
    hwnd = CreateWindowExA(0, "SpfyUpdateTray", "spfy", 0, 0, 0, 0, 0,
                           HWND_MESSAGE, NULL, wc.hInstance, NULL);
    if (!hwnd) return -1;

    memset(&nid, 0, sizeof nid);
    nid.cbSize           = sizeof nid;
    nid.hWnd             = hwnd;
    nid.uID              = 1;
    nid.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = UPD_TRAY_MSG;
    nid.hIcon            = LoadIconA(NULL, (LPCSTR)IDI_INFORMATION);
    spfy_upd_strlcpy(nid.szTip, "Speechify", sizeof nid.szTip);
    if (!Shell_NotifyIconA(NIM_ADD, &nid)) {
        DestroyWindow(hwnd);
        return -1;
    }

    nid.uFlags = NIF_INFO;
    spfy_upd_strlcpy(nid.szInfoTitle, "Speechify update available",
                     sizeof nid.szInfoTitle);
    spfy_upd_strlcpy(nid.szInfo, body, sizeof nid.szInfo);
    nid.dwInfoFlags = NIIF_INFO;
    nid.uTimeout    = 15000;      /* advisory; Windows clamps it */
    Shell_NotifyIconA(NIM_MODIFY, &nid);

    /* Hard stop, so a balloon nobody dismisses cannot leave a process (and
     * a tray icon) behind for the rest of the session. */
    SetTimer(hwnd, 1, 30000, NULL);
    while (GetMessageA(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    Shell_NotifyIconA(NIM_DELETE, &nid);
    DestroyWindow(hwnd);
    return 0;
}

#else

int spfy_upd_notify_gui(const spfy_upd_report *rep)
{
    (void)rep;
    return -1;
}

#endif
