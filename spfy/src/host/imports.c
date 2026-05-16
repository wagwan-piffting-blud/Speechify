/*
 * host/imports.c — import stubs for SWIttsFe-en-US.dll's dependency surface.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * 80 entries total (matches the static dump on 2026-05-10):
 *
 *   KERNEL32.dll  : 11 funcs (critical sections, timers, process info,
 *                              SearchPathA, DisableThreadLibraryCalls)
 *   USER32.dll    :  2 funcs (SetTimer, KillTimer — stubbed no-op)
 *   WINMM.dll     :  6 funcs (time*  — derived from clock_gettime)
 *   MSVCR71.dll   : 61 funcs (libc + C++ ABI helpers)
 *
 * Calling conventions:
 *   KERNEL32 / USER32 / WINMM are __stdcall on Win32.
 *   MSVCR71 is __cdecl (libc).
 *   On non-i386 builds the calling-convention attributes are no-ops.
 *
 * Strategy: provide minimal implementations against libc/<windows.h>.
 * On Windows we mostly forward to the real KERNEL32/USER32/WINMM exports
 * (which always exist on a Windows host); MSVCR71 functions map to
 * universal CRT or local stubs (because MSVCR71.dll itself is rarely
 * installed on modern Windows).
 *
 * IMPORTANT: this is *not* a generic Windows DLL host. The stub
 * semantics are tuned for SWIttsFe specifically — for example, USER32
 * SetTimer/KillTimer are no-ops because we never deliver WM_TIMER
 * messages back into the DLL. The DLL appears not to consume them in
 * the offline synth path (they're leftovers from the streaming variant
 * per static analysis).
 */

#include "imports.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <setjmp.h>
#include <time.h>
#include <errno.h>
#include <sys/stat.h>

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
  #include <mmsystem.h>
  #define HSTD __stdcall
  #define HCDECL __cdecl
  /* On Win32 with mingw/MSVC, _stat/_fstat exist as-is. */
  #include <io.h>
  #include <fcntl.h>
#else
  #include <unistd.h>
  #include <sys/time.h>
  #include <pthread.h>
  #if defined(__i386__)
    #define HSTD __attribute__((stdcall))
  #else
    #define HSTD
  #endif
  #define HCDECL
  /* POSIX has stat/fstat without underscore */
  #define _stat  stat
  #define _fstat fstat
  #define _fileno fileno
#endif

/* ============================================================
 * KERNEL32.dll
 * ============================================================ */

#ifdef _WIN32
/* The Win32 path forwards to the real kernel32. We pin handles on first
 * use to avoid GetProcAddress on the hot path. */
static HMODULE k32_h = NULL;
static FARPROC k32_get(const char *name) {
    if (!k32_h) k32_h = LoadLibraryA("kernel32.dll");
    return k32_h ? GetProcAddress(k32_h, name) : NULL;
}
#endif

/* The DLL imports 11 KERNEL32 funcs by name. We provide explicit
 * stubs (or forwarders) for each. */

#ifdef _WIN32
typedef void (WINAPI *k32_critsec_fn)(LPCRITICAL_SECTION);
typedef DWORD (WINAPI *k32_dword_void_fn)(VOID);
typedef VOID (WINAPI *k32_exit_fn)(UINT);
typedef BOOL (WINAPI *k32_qpc_fn)(LARGE_INTEGER *);
typedef VOID (WINAPI *k32_systime_fn)(FILETIME *);
typedef DWORD (WINAPI *k32_searchpath_fn)(LPCSTR, LPCSTR, LPCSTR, DWORD, LPSTR, LPSTR *);
typedef BOOL (WINAPI *k32_dtlc_fn)(HMODULE);

static VOID HSTD imp_EnterCriticalSection(LPCRITICAL_SECTION cs) {
    static k32_critsec_fn p; if (!p) p = (k32_critsec_fn)k32_get("EnterCriticalSection");
    p(cs);
}
static VOID HSTD imp_LeaveCriticalSection(LPCRITICAL_SECTION cs) {
    static k32_critsec_fn p; if (!p) p = (k32_critsec_fn)k32_get("LeaveCriticalSection");
    p(cs);
}
static VOID HSTD imp_InitializeCriticalSection(LPCRITICAL_SECTION cs) {
    static k32_critsec_fn p; if (!p) p = (k32_critsec_fn)k32_get("InitializeCriticalSection");
    p(cs);
}
static DWORD HSTD imp_GetTickCount(void) {
    static k32_dword_void_fn p; if (!p) p = (k32_dword_void_fn)k32_get("GetTickCount");
    return p();
}
static BOOL HSTD imp_QueryPerformanceCounter(LARGE_INTEGER *li) {
    static k32_qpc_fn p; if (!p) p = (k32_qpc_fn)k32_get("QueryPerformanceCounter");
    return p(li);
}
static VOID HSTD imp_ExitProcess(UINT code) {
    static k32_exit_fn p; if (!p) p = (k32_exit_fn)k32_get("ExitProcess");
    p(code);
}
static DWORD HSTD imp_GetCurrentThreadId(void) {
    static k32_dword_void_fn p; if (!p) p = (k32_dword_void_fn)k32_get("GetCurrentThreadId");
    return p();
}
static DWORD HSTD imp_GetCurrentProcessId(void) {
    static k32_dword_void_fn p; if (!p) p = (k32_dword_void_fn)k32_get("GetCurrentProcessId");
    return p();
}
static VOID HSTD imp_GetSystemTimeAsFileTime(FILETIME *ft) {
    static k32_systime_fn p; if (!p) p = (k32_systime_fn)k32_get("GetSystemTimeAsFileTime");
    p(ft);
}
static DWORD HSTD imp_SearchPathA(LPCSTR path, LPCSTR file, LPCSTR ext,
                                  DWORD bufLen, LPSTR buf, LPSTR *filePart) {
    static k32_searchpath_fn p;
    if (!p) p = (k32_searchpath_fn)k32_get("SearchPathA");
    return p(path, file, ext, bufLen, buf, filePart);
}
static BOOL HSTD imp_DisableThreadLibraryCalls(HMODULE m) {
    /* No-op in our hosting model — we never deliver DLL_THREAD_ATTACH
     * anyway. Return TRUE to indicate success. */
    (void)m;
    return TRUE;
}

#else /* !_WIN32: POSIX */

/* Linux/macOS implementations. Critical sections become pthread mutexes;
 * the CRITICAL_SECTION struct in Win32 is 24 bytes (sizeof on x86), so
 * we wrap a pthread_mutex_t (which fits) and assume the DLL only treats
 * it as opaque. NB: this works because the DLL never reads internal
 * fields of CRITICAL_SECTION. */

typedef struct critical_section {
    pthread_mutex_t m;
} critical_section_t;

static void HSTD imp_EnterCriticalSection(critical_section_t *cs) {
    pthread_mutex_lock(&cs->m);
}
static void HSTD imp_LeaveCriticalSection(critical_section_t *cs) {
    pthread_mutex_unlock(&cs->m);
}
static void HSTD imp_InitializeCriticalSection(critical_section_t *cs) {
    pthread_mutex_init(&cs->m, NULL);
}
static uint32_t HSTD imp_GetTickCount(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}
typedef struct { int32_t low; int32_t high; } large_int_t;
static int32_t HSTD imp_QueryPerformanceCounter(large_int_t *li) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    int64_t v = ts.tv_sec * 1000000000LL + ts.tv_nsec;
    li->low = (int32_t)v;
    li->high = (int32_t)(v >> 32);
    return 1;
}
static void HSTD imp_ExitProcess(uint32_t code) { exit((int)code); }
static uint32_t HSTD imp_GetCurrentThreadId(void) {
    return (uint32_t)(uintptr_t)pthread_self();
}
static uint32_t HSTD imp_GetCurrentProcessId(void) { return (uint32_t)getpid(); }
typedef struct { uint32_t low; uint32_t high; } filetime_t;
static void HSTD imp_GetSystemTimeAsFileTime(filetime_t *ft) {
    /* FILETIME = 100ns ticks since 1601-01-01. */
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    int64_t epoch_100ns = ((int64_t)ts.tv_sec + 11644473600LL) * 10000000LL
                        + ts.tv_nsec / 100;
    ft->low = (uint32_t)epoch_100ns;
    ft->high = (uint32_t)(epoch_100ns >> 32);
}
static uint32_t HSTD imp_SearchPathA(const char *path, const char *file,
                                     const char *ext, uint32_t bufLen,
                                     char *buf, char **filePart) {
    /* SWIttsFe doesn't seem to actually need this for offline synth
     * (statically-baked enu.ddl). Return 0 = not found. */
    (void)path; (void)file; (void)ext; (void)bufLen; (void)buf; (void)filePart;
    return 0;
}
static int32_t HSTD imp_DisableThreadLibraryCalls(void *m) { (void)m; return 1; }

#endif /* _WIN32 */

/* ============================================================
 * USER32.dll — stubbed no-ops
 * ============================================================ */

#ifdef _WIN32
static UINT_PTR HSTD imp_SetTimer(HWND hWnd, UINT_PTR id, UINT elapse, void *proc) {
    (void)hWnd; (void)elapse; (void)proc;
    return id ? id : 1;
}
static BOOL HSTD imp_KillTimer(HWND hWnd, UINT_PTR id) {
    (void)hWnd; (void)id;
    return TRUE;
}
#else
static uintptr_t HSTD imp_SetTimer(void *hWnd, uintptr_t id, uint32_t elapse, void *proc) {
    (void)hWnd; (void)elapse; (void)proc;
    return id ? id : 1;
}
static int32_t HSTD imp_KillTimer(void *hWnd, uintptr_t id) {
    (void)hWnd; (void)id;
    return 1;
}
#endif

/* ============================================================
 * WINMM.dll — timer functions
 * ============================================================ */

typedef struct { uint32_t wPeriodMin; uint32_t wPeriodMax; } host_timecaps_t;

#ifdef _WIN32
typedef MMRESULT (WINAPI *winmm_caps_fn)(LPTIMECAPS, UINT);
typedef MMRESULT (WINAPI *winmm_setevent_fn)(UINT, UINT, LPTIMECALLBACK, DWORD_PTR, UINT);
typedef MMRESULT (WINAPI *winmm_killevent_fn)(UINT);
typedef MMRESULT (WINAPI *winmm_period_fn)(UINT);
typedef DWORD (WINAPI *winmm_gettime_fn)(VOID);

static HMODULE winmm_h = NULL;
static FARPROC winmm_get(const char *name) {
    if (!winmm_h) winmm_h = LoadLibraryA("winmm.dll");
    return winmm_h ? GetProcAddress(winmm_h, name) : NULL;
}

static MMRESULT HSTD imp_timeGetDevCaps(LPTIMECAPS p, UINT sz) {
    static winmm_caps_fn f; if (!f) f = (winmm_caps_fn)winmm_get("timeGetDevCaps");
    if (f) return f(p, sz);
    if (p && sz >= sizeof(TIMECAPS)) { p->wPeriodMin = 1; p->wPeriodMax = 1000000; }
    return 0;
}
static DWORD HSTD imp_timeGetTime(void) {
    static winmm_gettime_fn f; if (!f) f = (winmm_gettime_fn)winmm_get("timeGetTime");
    return f ? f() : GetTickCount();
}
static MMRESULT HSTD imp_timeKillEvent(UINT id) {
    static winmm_killevent_fn f; if (!f) f = (winmm_killevent_fn)winmm_get("timeKillEvent");
    return f ? f(id) : 0;
}
static MMRESULT HSTD imp_timeSetEvent(UINT delay, UINT res, LPTIMECALLBACK cb,
                                      DWORD_PTR user, UINT type) {
    static winmm_setevent_fn f; if (!f) f = (winmm_setevent_fn)winmm_get("timeSetEvent");
    return f ? f(delay, res, cb, user, type) : 0;
}
static MMRESULT HSTD imp_timeBeginPeriod(UINT ms) {
    static winmm_period_fn f; if (!f) f = (winmm_period_fn)winmm_get("timeBeginPeriod");
    return f ? f(ms) : 0;
}
static MMRESULT HSTD imp_timeEndPeriod(UINT ms) {
    static winmm_period_fn f; if (!f) f = (winmm_period_fn)winmm_get("timeEndPeriod");
    return f ? f(ms) : 0;
}
#else
static uint32_t HSTD imp_timeGetDevCaps(host_timecaps_t *p, uint32_t sz) {
    if (p && sz >= sizeof(*p)) { p->wPeriodMin = 1; p->wPeriodMax = 1000000; }
    return 0;
}
static uint32_t HSTD imp_timeGetTime(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}
static uint32_t HSTD imp_timeKillEvent(uint32_t id) { (void)id; return 0; }
static uint32_t HSTD imp_timeSetEvent(uint32_t delay, uint32_t res, void *cb,
                                      uintptr_t user, uint32_t type) {
    (void)delay; (void)res; (void)cb; (void)user; (void)type; return 0;
}
static uint32_t HSTD imp_timeBeginPeriod(uint32_t ms) { (void)ms; return 0; }
static uint32_t HSTD imp_timeEndPeriod(uint32_t ms) { (void)ms; return 0; }
#endif

/* ============================================================
 * MSVCR71.dll — libc + C++ ABI helpers (61 entries)
 * ============================================================ */

/* Most map directly to libc / libm. A handful need shims. */

static char *HCDECL imp__strdup(const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char *r = (char *)malloc(n);
    if (r) memcpy(r, s, n);
    return r;
}

static int HCDECL imp__stricmp(const char *a, const char *b) {
#ifdef _WIN32
    return _stricmp(a, b);
#else
    while (*a && *b) {
        int da = tolower((unsigned char)*a), db = tolower((unsigned char)*b);
        if (da != db) return da - db;
        a++; b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
#endif
}

/* MSVCR71's _setjmp3 takes (jmp_buf, ignored_count, ...). We translate to
 * setjmp() and discard the extra args. */
static int HCDECL imp__setjmp3(jmp_buf buf, int ignored, ...) {
    (void)ignored;
    return setjmp(buf);
}

/* operator new (mangled `??2@YAPAXI@Z`): size_t -> void*. Throws on null
 * in C++ but we abort instead — no SEH path back to the DLL.
 * Zero-init for the same reasons as imp_malloc above. */
static void *HCDECL imp_op_new(size_t n) {
    void *p = calloc(1, n);
    if (!p) abort();
    return p;
}
static void HCDECL imp_op_delete(void *p) { free(p); }

static void HCDECL imp__purecall(void) { abort(); }

/* __CxxFrameHandler — the DLL uses setjmp/longjmp, not C++ exceptions,
 * per static analysis. Stub returning 1 (= continue search). */
static int HCDECL imp_CxxFrameHandler(void *rec, void *frame,
                                       void *ctx, void *dispatch) {
    (void)rec; (void)frame; (void)ctx; (void)dispatch;
    return 1;
}

/* Various MSVC-internal helpers — safe to no-op for offline synth. */
static void HCDECL imp__initterm(void (**start)(void), void (**end)(void)) {
    for (; start < end; start++) if (*start) (*start)();
}
static int HCDECL imp__dllonexit(void *fn, void *p, void *q) {
    (void)fn; (void)p; (void)q; return 0;
}
static void *HCDECL imp__onexit(void *fn) { (void)fn; return NULL; }
static int HCDECL imp__except_handler3(void *rec, void *frame,
                                       void *ctx, void *disp) {
    (void)rec; (void)frame; (void)ctx; (void)disp;
    return 0;
}
static int HCDECL imp__CppXcptFilter(unsigned long code, void *info) {
    (void)code; (void)info;
    return 0; /* EXCEPTION_CONTINUE_SEARCH */
}
static void HCDECL imp_terminate(void) { abort(); }
static int HCDECL imp__security_error_handler(int code, void *data) {
    (void)code; (void)data; abort(); return 0;
}

/* MSVCR71 exports `_adjust_fdiv` as a *variable* (DWORD), not a function:
 * it's the Pentium FDIV-bug workaround flag. We export a 0 value. */
static int g_adjust_fdiv_val = 0;

/* _errno: returns a pointer to thread-local errno. We just expose libc's
 * errno-pointer. */
static int *HCDECL imp__errno(void) { return &errno; }

/* _iob: pointer to stdio FILE * table — MSVCR71 has `_iob[]` of length
 * 20+; we synthesize a tiny one mapping 0/1/2 to stdin/stdout/stderr. */
typedef struct { void *p; } iob_entry_t;
static FILE *imp__iob_storage[3];
static void imp__iob_init(void) {
    imp__iob_storage[0] = stdin;
    imp__iob_storage[1] = stdout;
    imp__iob_storage[2] = stderr;
}

/* For the libc functions we have direct equivalents we just expose
 * a fixed-convention wrapper symbol. */
static void *HCDECL imp_memset(void *d, int c, size_t n)        { return memset(d, c, n); }
static void *HCDECL imp_memcpy(void *d, const void *s, size_t n){ return memcpy(d, s, n); }
static void *HCDECL imp_memmove(void *d, const void *s, size_t n){ return memmove(d, s, n); }
/* Zero-init the FE's malloc. Some FE init code byte-writes only the
 * low byte of struct fields; without zero-init the high bytes are
 * heap garbage and downstream comparisons break. */
static void *HCDECL imp_malloc(size_t n) { return calloc(1, n); }
static void  HCDECL imp_free(void *p)                           { free(p); }
static void *HCDECL imp_calloc(size_t n, size_t s)              { return calloc(n, s); }
static void *HCDECL imp_realloc(void *p, size_t n)              { return realloc(p, n); }
static FILE *HCDECL imp_fopen(const char *p, const char *m)     { return fopen(p, m); }
static int   HCDECL imp_fclose(FILE *f)                         { return fclose(f); }
static size_t HCDECL imp_fread(void *p, size_t s, size_t n, FILE *f){ return fread(p, s, n, f); }
static size_t HCDECL imp_fwrite(const void *p, size_t s, size_t n, FILE *f){ return fwrite(p, s, n, f); }
static int   HCDECL imp_fseek(FILE *f, long o, int w)           { return fseek(f, o, w); }
static long  HCDECL imp_ftell(FILE *f)                          { return ftell(f); }
static void  HCDECL imp_rewind(FILE *f)                         { rewind(f); }
static int   HCDECL imp_fflush(FILE *f)                         { return fflush(f); }
static int   HCDECL imp_fputs(const char *s, FILE *f)           { return fputs(s, f); }
static char *HCDECL imp_fgets(char *s, int n, FILE *f)          { return fgets(s, n, f); }
static int   HCDECL imp_getchar(void)                           { return getchar(); }
static char *HCDECL imp_strncpy(char *d, const char *s, size_t n){ return strncpy(d, s, n); }
static int   HCDECL imp_strncmp(const char *a, const char *b, size_t n){ return strncmp(a, b, n); }
static char *HCDECL imp_strchr(const char *s, int c)            { return (char *)strchr(s, c); }
static int   HCDECL imp_toupper(int c)                          { return toupper(c); }
static int   HCDECL imp_isdigit(int c)                          { return isdigit(c); }
static int   HCDECL imp_isspace(int c)                          { return isspace(c); }
static int   HCDECL imp_atoi(const char *s)                     { return atoi(s); }
static long  HCDECL imp_atol(const char *s)                     { return atol(s); }
static double HCDECL imp_atof(const char *s)                    { return atof(s); }
static long  HCDECL imp_strtol(const char *s, char **e, int b)  { return strtol(s, e, b); }
static double HCDECL imp_strtod(const char *s, char **e)        { return strtod(s, e); }
static ldiv_t HCDECL imp_ldiv(long num, long den)               { return ldiv(num, den); }
static double HCDECL imp_pow(double a, double b)                { return pow(a, b); }
static double HCDECL imp_log(double x)                          { return log(x); }
static double HCDECL imp_exp(double x)                          { return exp(x); }
static int    HCDECL imp_sprintf(char *buf, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); int r = vsprintf(buf, fmt, ap); va_end(ap); return r;
}
static int    HCDECL imp_vsprintf(char *buf, const char *fmt, va_list ap) {
    return vsprintf(buf, fmt, ap);
}
static int    HCDECL imp_fprintf(FILE *f, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); int r = vfprintf(f, fmt, ap); va_end(ap); return r;
}
static int    HCDECL imp_vfprintf(FILE *f, const char *fmt, va_list ap) {
    return vfprintf(f, fmt, ap);
}
static int    HCDECL imp_sscanf(const char *s, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); int r = vsscanf(s, fmt, ap); va_end(ap); return r;
}
static int    HCDECL imp_exit(int c)                           { exit(c); return 0; }
static clock_t HCDECL imp_clock(void)                          { return clock(); }
static void   HCDECL imp_longjmp(jmp_buf b, int v)             { longjmp(b, v); }

#ifdef _WIN32
static int HCDECL imp__stat_w(const char *p, struct _stat *st)  { return _stat(p, st); }
static int HCDECL imp__fstat_w(int fd, struct _stat *st)        { return _fstat(fd, st); }
static int HCDECL imp__fileno_w(FILE *f)                        { return _fileno(f); }
#else
static int HCDECL imp__stat_w(const char *p, struct stat *st)   { return stat(p, st); }
static int HCDECL imp__fstat_w(int fd, struct stat *st)         { return fstat(fd, st); }
static int HCDECL imp__fileno_w(FILE *f)                        { return fileno(f); }
#endif

/* ============================================================
 * Dispatch table
 * ============================================================ */

typedef struct {
    const char *dll;
    const char *name;
    void *fn;
} host_import_entry_t;

static const host_import_entry_t g_imports[] = {
    /* KERNEL32 (11) */
    { "kernel32.dll", "LeaveCriticalSection",       (void *)&imp_LeaveCriticalSection },
    { "kernel32.dll", "EnterCriticalSection",       (void *)&imp_EnterCriticalSection },
    { "kernel32.dll", "InitializeCriticalSection",  (void *)&imp_InitializeCriticalSection },
    { "kernel32.dll", "GetTickCount",               (void *)&imp_GetTickCount },
    { "kernel32.dll", "QueryPerformanceCounter",    (void *)&imp_QueryPerformanceCounter },
    { "kernel32.dll", "ExitProcess",                (void *)&imp_ExitProcess },
    { "kernel32.dll", "GetCurrentThreadId",         (void *)&imp_GetCurrentThreadId },
    { "kernel32.dll", "GetCurrentProcessId",        (void *)&imp_GetCurrentProcessId },
    { "kernel32.dll", "GetSystemTimeAsFileTime",    (void *)&imp_GetSystemTimeAsFileTime },
    { "kernel32.dll", "SearchPathA",                (void *)&imp_SearchPathA },
    { "kernel32.dll", "DisableThreadLibraryCalls",  (void *)&imp_DisableThreadLibraryCalls },
    /* USER32 (2) */
    { "user32.dll",   "SetTimer",                   (void *)&imp_SetTimer },
    { "user32.dll",   "KillTimer",                  (void *)&imp_KillTimer },
    /* WINMM (6) */
    { "winmm.dll",    "timeGetDevCaps",             (void *)&imp_timeGetDevCaps },
    { "winmm.dll",    "timeKillEvent",              (void *)&imp_timeKillEvent },
    { "winmm.dll",    "timeSetEvent",               (void *)&imp_timeSetEvent },
    { "winmm.dll",    "timeGetTime",                (void *)&imp_timeGetTime },
    { "winmm.dll",    "timeEndPeriod",              (void *)&imp_timeEndPeriod },
    { "winmm.dll",    "timeBeginPeriod",            (void *)&imp_timeBeginPeriod },
    /* MSVCR71 (61) */
    { "msvcr71.dll",  "_strdup",                    (void *)&imp__strdup },
    { "msvcr71.dll",  "_stricmp",                   (void *)&imp__stricmp },
    { "msvcr71.dll",  "memset",                     (void *)&imp_memset },
    { "msvcr71.dll",  "_setjmp3",                   (void *)&imp__setjmp3 },
    { "msvcr71.dll",  "memcpy",                     (void *)&imp_memcpy },
    { "msvcr71.dll",  "pow",                        (void *)&imp_pow },
    { "msvcr71.dll",  "log",                        (void *)&imp_log },
    { "msvcr71.dll",  "exp",                        (void *)&imp_exp },
    { "msvcr71.dll",  "free",                       (void *)&imp_free },
    { "msvcr71.dll",  "malloc",                     (void *)&imp_malloc },
    { "msvcr71.dll",  "fclose",                     (void *)&imp_fclose },
    { "msvcr71.dll",  "fopen",                      (void *)&imp_fopen },
    { "msvcr71.dll",  "exit",                       (void *)&imp_exit },
    { "msvcr71.dll",  "strncpy",                    (void *)&imp_strncpy },
    { "msvcr71.dll",  "??2@YAPAXI@Z",               (void *)&imp_op_new },
    { "msvcr71.dll",  "??3@YAXPAX@Z",               (void *)&imp_op_delete },
    { "msvcr71.dll",  "_purecall",                  (void *)&imp__purecall },
    { "msvcr71.dll",  "__CxxFrameHandler",          (void *)&imp_CxxFrameHandler },
    { "msvcr71.dll",  "sprintf",                    (void *)&imp_sprintf },
    { "msvcr71.dll",  "clock",                      (void *)&imp_clock },
    { "msvcr71.dll",  "fread",                      (void *)&imp_fread },
    { "msvcr71.dll",  "rewind",                     (void *)&imp_rewind },
    { "msvcr71.dll",  "ftell",                      (void *)&imp_ftell },
    { "msvcr71.dll",  "fseek",                      (void *)&imp_fseek },
    { "msvcr71.dll",  "toupper",                    (void *)&imp_toupper },
    { "msvcr71.dll",  "calloc",                     (void *)&imp_calloc },
    { "msvcr71.dll",  "memmove",                    (void *)&imp_memmove },
    { "msvcr71.dll",  "realloc",                    (void *)&imp_realloc },
    { "msvcr71.dll",  "longjmp",                    (void *)&imp_longjmp },
    { "msvcr71.dll",  "atof",                       (void *)&imp_atof },
    { "msvcr71.dll",  "isdigit",                    (void *)&imp_isdigit },
    { "msvcr71.dll",  "strtol",                     (void *)&imp_strtol },
    { "msvcr71.dll",  "_errno",                     (void *)&imp__errno },
    { "msvcr71.dll",  "strtod",                     (void *)&imp_strtod },
    { "msvcr71.dll",  "fwrite",                     (void *)&imp_fwrite },
    { "msvcr71.dll",  "atoi",                       (void *)&imp_atoi },
    { "msvcr71.dll",  "atol",                       (void *)&imp_atol },
    { "msvcr71.dll",  "sscanf",                     (void *)&imp_sscanf },
    { "msvcr71.dll",  "ldiv",                       (void *)&imp_ldiv },
    { "msvcr71.dll",  "vfprintf",                   (void *)&imp_vfprintf },
    { "msvcr71.dll",  "_iob",                       (void *)&imp__iob_storage[0] },
    { "msvcr71.dll",  "fprintf",                    (void *)&imp_fprintf },
    { "msvcr71.dll",  "isspace",                    (void *)&imp_isspace },
    { "msvcr71.dll",  "vsprintf",                   (void *)&imp_vsprintf },
    { "msvcr71.dll",  "fflush",                     (void *)&imp_fflush },
    { "msvcr71.dll",  "fputs",                      (void *)&imp_fputs },
    { "msvcr71.dll",  "fgets",                      (void *)&imp_fgets },
    { "msvcr71.dll",  "getchar",                    (void *)&imp_getchar },
    { "msvcr71.dll",  "__security_error_handler",   (void *)&imp__security_error_handler },
    { "msvcr71.dll",  "_except_handler3",           (void *)&imp__except_handler3 },
    { "msvcr71.dll",  "_initterm",                  (void *)&imp__initterm },
    { "msvcr71.dll",  "_adjust_fdiv",               (void *)&g_adjust_fdiv_val },
    { "msvcr71.dll",  "__CppXcptFilter",            (void *)&imp__CppXcptFilter },
    { "msvcr71.dll",  "?terminate@@YAXXZ",          (void *)&imp_terminate },
    { "msvcr71.dll",  "__dllonexit",                (void *)&imp__dllonexit },
    { "msvcr71.dll",  "_onexit",                    (void *)&imp__onexit },
    { "msvcr71.dll",  "strchr",                     (void *)&imp_strchr },
    { "msvcr71.dll",  "_stat",                      (void *)&imp__stat_w },
    { "msvcr71.dll",  "_fstat",                     (void *)&imp__fstat_w },
    { "msvcr71.dll",  "strncmp",                    (void *)&imp_strncmp },
    { "msvcr71.dll",  "_fileno",                    (void *)&imp__fileno_w },
};

static const size_t g_imports_n = sizeof(g_imports) / sizeof(g_imports[0]);

void *host_default_resolver(const char *dll, const char *name,
                            uint16_t ordinal, void *user) {
    (void)ordinal; (void)user;
    if (!dll || !name) return NULL;

    /* Lazy-init _iob on first lookup of any function. */
    static int iob_ready = 0;
    if (!iob_ready) { imp__iob_init(); iob_ready = 1; }

    for (size_t i = 0; i < g_imports_n; i++) {
        if (strcmp(g_imports[i].dll, dll) == 0 &&
            strcmp(g_imports[i].name, name) == 0) {
            return g_imports[i].fn;
        }
    }
    /* Unknown import — surface for diagnostics. */
    fprintf(stderr, "[host] unresolved import: %s!%s\n", dll, name);
    return NULL;
}
