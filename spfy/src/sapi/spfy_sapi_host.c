/* spfy_sapi_host.exe — 32-bit out-of-process COM server for the Speechify
 * SAPI engine. */

#define INITGUID 1
#include "sapiddk_min.h"

#include <stdio.h>

extern IClassFactory *spfy_sapi_get_factory(void);

/* Symbol forwarding hooks defined in spfy_sapi.c. */
extern HMODULE g_hModule;

static volatile LONG g_keep_alive = 1;

/* Watchdog: periodically check whether any clients still hold refs. */
extern LONG g_dll_refs;

static DWORD WINAPI watchdog_thread(LPVOID p)
{
    (void)p;
    while (g_keep_alive) {
        Sleep(2000);
        if (g_dll_refs == 0) {
            PostThreadMessage(GetCurrentThreadId(), WM_QUIT, 0, 0);
            return 0;
        }
    }
    return 0;
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR cmd, int show)
{
    (void)hPrev; (void)cmd; (void)show;
    /* DllMain isn't called for an EXE, so manually wire the globals the
     * shared spfy_sapi.c code expects. */
    g_hModule = hInst;

    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr)) return 1;

    IClassFactory *factory = spfy_sapi_get_factory();
    DWORD dwRegister = 0;
    hr = CoRegisterClassObject(&CLSID_SpfyTTSEngine,
                               (IUnknown *)factory,
                               CLSCTX_LOCAL_SERVER,
                               REGCLS_MULTIPLEUSE | REGCLS_SUSPENDED,
                               &dwRegister);
    if (FAILED(hr)) { CoUninitialize(); return 1; }

    hr = CoResumeClassObjects();
    if (FAILED(hr)) {
        CoRevokeClassObject(dwRegister);
        CoUninitialize();
        return 1;
    }

    /* Reference-counting message loop. */
    DWORD tid = GetCurrentThreadId();
    HANDLE wd = CreateThread(NULL, 0, watchdog_thread, NULL, 0, NULL);
    (void)tid; (void)wd;

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    g_keep_alive = 0;

    CoSuspendClassObjects();
    CoRevokeClassObject(dwRegister);
    CoUninitialize();
    return 0;
}
