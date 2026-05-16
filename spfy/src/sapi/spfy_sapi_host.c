/* spfy_sapi_host.exe — 32-bit out-of-process COM server for the Speechify
 * SAPI engine. Exposes the same CLSID_SpfyTTSEngine class factory as
 * spfy_sapi.dll, but as a LocalServer32 so 64-bit SAPI clients can talk
 * to it via cross-bitness COM marshaling.
 *
 * Lifecycle: COM launches us when a client calls CoCreateInstance with
 * CLSCTX_LOCAL_SERVER. We register the factory via CoRegisterClassObject
 * (REGCLS_MULTIPLEUSE | REGCLS_SUSPENDED), pump messages until all
 * outstanding refcounts drop, then exit.
 *
 * The class-factory implementation, COM object, and SpfyEngineImpl live
 * in spfy_sapi.c — we link against the same .obj so there's exactly one
 * place where the synth logic is defined. */

#define INITGUID 1
#include "sapiddk_min.h"

#include <stdio.h>

extern IClassFactory *spfy_sapi_get_factory(void);

/* Symbol forwarding hooks defined in spfy_sapi.c. They're identical to
 * the file-internal ones — we just need an extern accessor so the EXE
 * can grab the same singleton without re-declaring it. */
extern HMODULE g_hModule;       /* spfy_sapi.c globals reused */

static volatile LONG g_keep_alive = 1;

/* Watchdog: periodically check whether any clients still hold refs. If
 * the factory's lock count has dropped to zero AND no engine instances
 * remain, post WM_QUIT so the message loop exits. COM normally drives
 * this via CoSuspendClassObjects + revoke when DllCanUnloadNow is true,
 * but for a local server we do it ourselves. */
extern LONG g_dll_refs;         /* live count of factory+engine refs */

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

    /* Reference-counting message loop. Cross-bitness clients keep the
     * factory locked via LockServer + AddRef on the engine; when both
     * hit zero we exit. The watchdog posts WM_QUIT. */
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
