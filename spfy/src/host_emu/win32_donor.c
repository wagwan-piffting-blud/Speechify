// win32_vst.c - Win32 import shims + guest heap + import dispatch, for hosting
// statically-linked MSVC VST plugins headless. Derived from the AcuVoice win32.c;
// VFS/INI removed, the modern MSVC/UCRT startup surface added.
#include "emu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <ctype.h>

// ---------------- import table ----------------
typedef struct { char name[64]; shim_fn fn; int argbytes; } imp_t;
static imp_t g_imp[MAX_IMPORTS];
static int   g_nimp = 0;
int g_shim_cleanup = 0;

uint32_t arg32(int i){ return rd32(CPU.r[ESP] + 4 + 4*i); }
void ret_set(uint32_t v){ CPU.r[EAX] = v; }

int win32_is_import_va(uint32_t va){ return va >= IMP_BASE && va < IMP_BASE + (uint32_t)MAX_IMPORTS*IMP_STRIDE; }

int g_cur_imp = -1;   // index of the import currently being dispatched (for s_unimpl logging)
void win32_dispatch(uint32_t va){
    int idx = (va - IMP_BASE) / IMP_STRIDE;
    if (idx < 0 || idx >= g_nimp || !g_imp[idx].fn){ fprintf(stderr,"** bad import dispatch idx=%d va=%08x\n",idx,va); CPU.halted=1; CPU.faulted=1; return; }
    g_shim_cleanup = g_imp[idx].argbytes;
    g_cur_imp = idx;
    static int implog=-1; if(implog<0) implog = getenv("EMU_IMPLOG")?1:0;
    /* spfy addition: SPFY_ESPLOG=1 prints ESP delta around each shim
     * dispatch, useful for tracking down stack-balance bugs introduced
     * by wrong argbytes or shim-internal stack manipulation. */
    static int esplog=-1; if(esplog<0) esplog = getenv("SPFY_ESPLOG")?1:0;
    uint32_t esp_in = CPU.r[ESP];
    if(implog) fprintf(stderr,"[imp] %-26s ret=%08x esp=%08x clean=%d\n", g_imp[idx].name, rd32(CPU.r[ESP]), CPU.r[ESP], g_imp[idx].argbytes);
    int wasFaulted = CPU.faulted;
    /* spfy fix for nested-dispatch reentrance:
     * the donor uses the file-global g_shim_cleanup to ferry argbytes
     * into the exit-side ESP cleanup. _initterm's s_initterm shim drives
     * cpu_run via call_nested, which dispatches MORE imports during the
     * ctor (e.g. QueryPerformanceCounter, clean=4). Those nested
     * dispatches OVERWRITE g_shim_cleanup, so by the time the outer
     * _initterm dispatch exits, the saved value is stale -> wrong ESP
     * advance -> caller stack misalignment.
     * Snapshot the cleanup locally before calling the shim and restore
     * after, so nested dispatches don't corrupt the outer value. */
    int saved_cleanup = g_shim_cleanup;
    int saved_cur_imp = g_cur_imp;
    g_imp[idx].fn();
    g_shim_cleanup = saved_cleanup;
    g_cur_imp = saved_cur_imp;
    if(CPU.faulted && !wasFaulted) fprintf(stderr,"[imp-FAULT] in shim '%s' (idx %d)\n", g_imp[idx].name, idx);
    uint32_t retaddr = rd32(CPU.r[ESP]);
    CPU.r[ESP] += 4 + g_shim_cleanup;
    CPU.eip = retaddr;
    if(esplog){ int delta = (int)(CPU.r[ESP] - esp_in); fprintf(stderr,"[esp] %-26s in=%08x out=%08x delta=%+d (clean=%d)\n", g_imp[idx].name, esp_in, CPU.r[ESP], delta, g_imp[idx].argbytes); }
}
const char* win32_imp_name(int idx){ return (idx>=0 && idx<g_nimp) ? g_imp[idx].name : "?"; }

// register a host callback (e.g. the VST audioMaster) as a guest-callable pseudo-import.
uint32_t host_register_callback(shim_fn fn, int argbytes, const char* name){
    int idx = g_nimp++;
    if (idx >= MAX_IMPORTS){ fprintf(stderr,"too many imports\n"); exit(1); }
    snprintf(g_imp[idx].name, sizeof g_imp[idx].name, "%s", name?name:"cb");
    g_imp[idx].fn = fn; g_imp[idx].argbytes = argbytes;
    return IMP_BASE + idx*IMP_STRIDE;
}

// ---------------- guest heap (implicit free list) ----------------
static uint32_t HEAP_END;
// VirtualAlloc arena: 64KB-granular bump allocator. Win32 returns 64KB-aligned base
// addresses for MEM_RESERVE; Delphi's memory manager masks the low 16 bits of the
// returned pointer to find its pool header, so non-aligned pointers from the general
// heap make it write metadata over the heap's own block headers. Keep it separate.
static uint32_t g_valloc_next;
static void valloc_init(void){ mem_map(VALLOC_BASE, VALLOC_SIZE, "valloc"); g_valloc_next = VALLOC_BASE; }
static uint32_t valloc_reserve(uint32_t n){
    n = (n + 0xFFFFu) & ~0xFFFFu; if(n==0) n=0x10000;          // round up to 64KB
    if(g_valloc_next + n > VALLOC_BASE + VALLOC_SIZE){ fprintf(stderr,"** valloc OOM requesting %u\n", n); return 0; }
    uint32_t p = g_valloc_next; g_valloc_next += n; return p;   // already 64KB aligned
}
static void heap_init(void){
    mem_map(HEAP_BASE, HEAP_SIZE, "heap");
    HEAP_END = HEAP_BASE + HEAP_SIZE;
    wr32(HEAP_BASE, HEAP_SIZE - 8);
    wr32(HEAP_BASE + 4, 1); // free
    valloc_init();
}
uint32_t guest_alloc(uint32_t n, int zero){
    n = (n + 7) & ~7u; if(n==0) n=8;
    uint32_t b = HEAP_BASE;
    while (b < HEAP_END){
        uint32_t sz = rd32(b), fr = rd32(b+4);
        if (fr && sz >= n){
            if (sz >= n + 16){ uint32_t nb = b + 8 + n; wr32(nb, sz - n - 8); wr32(nb+4, 1); wr32(b, n); }
            wr32(b+4, 0);
            if (zero){ for(uint32_t i=0;i<rd32(b);i+=4) wr32(b+8+i,0); }
            return b + 8;
        }
        b += 8 + sz;
    }
    fprintf(stderr,"** heap OOM requesting %u\n", n);
    return 0;
}
void guest_free(uint32_t p){
    if(!p) return; uint32_t b=p-8; wr32(b+4,1);
    for(;;){ uint32_t sz=rd32(b); uint32_t nb=b+8+sz; if(nb>=HEAP_END) break; if(rd32(nb+4)){ wr32(b, sz + 8 + rd32(nb)); } else break; }
}
static uint32_t heap_realloc(uint32_t p, uint32_t n){
    if(!p) return guest_alloc(n,0);
    uint32_t oldsz = rd32(p-8);
    uint32_t np = guest_alloc(n,0); if(!np) return 0;
    uint32_t c = oldsz<n?oldsz:n; for(uint32_t i=0;i<c;i++) wr8(np+i, rd8(p+i));
    guest_free(p);
    return np;
}

// ---------------- helpers ----------------
static void guest_strcpy_out(uint32_t dst, const char* s, int max){ int i=0; for(; s[i] && i<max-1; i++) wr8(dst+i, s[i]); wr8(dst+i,0); }
static void guest_strn(uint32_t src, char* out, int max){ int i=0; for(; i<max-1; i++){ char c=(char)rd8(src+i); out[i]=c; if(!c)break; } out[i<max?i:max-1]=0; }

static uint32_t g_lasterr = 0;
static uint32_t g_tls[256]; static int g_tlsnext=1;
static uint32_t g_cmdline_a=0, g_cmdline_w=0, g_env_a=0, g_env_w=0;
static uint32_t g_tick=1;

// ---------------- critical sections / tls ----------------
static void s_nop0(void){}
static void s_ret0(void){ ret_set(0); }
static void s_ret1(void){ ret_set(1); }
static void s_EnterCriticalSection(void){}
static void s_LeaveCriticalSection(void){}
static void s_InitializeCriticalSection(void){}
static void s_DeleteCriticalSection(void){}
static void s_InitializeCriticalSectionAndSpinCount(void){ ret_set(1); }
static void s_InitializeCriticalSectionEx(void){ ret_set(1); }

static void s_TlsAlloc(void){ ret_set(g_tlsnext++); }
static void s_TlsFree(void){ ret_set(1); }
static void s_TlsSetValue(void){ uint32_t i=arg32(0); if(i<256)g_tls[i]=arg32(1); ret_set(1); }
static void s_TlsGetValue(void){ uint32_t i=arg32(0); ret_set(i<256?g_tls[i]:0); }
// Fls* (fiber-local) map onto the same table.
static void s_FlsAlloc(void){ ret_set(g_tlsnext++); }
static void s_FlsFree(void){ ret_set(1); }
static void s_FlsSetValue(void){ uint32_t i=arg32(0); if(i<256)g_tls[i]=arg32(1); ret_set(1); }
static void s_FlsGetValue(void){ uint32_t i=arg32(0); ret_set(i<256?g_tls[i]:0); }

static void s_InterlockedIncrement(void){ uint32_t p=arg32(0); uint32_t v=rd32(p)+1; wr32(p,v); ret_set(v); }
static void s_InterlockedDecrement(void){ uint32_t p=arg32(0); uint32_t v=rd32(p)-1; wr32(p,v); ret_set(v); }
static void s_InterlockedExchange(void){ uint32_t p=arg32(0),nv=arg32(1),ov=rd32(p); wr32(p,nv); ret_set(ov); }
static void s_InterlockedCompareExchange(void){ uint32_t p=arg32(0),ex=arg32(1),cmp=arg32(2),cur=rd32(p); if(cur==cmp) wr32(p,ex); ret_set(cur); }
static void s_InitializeSListHead(void){ uint32_t p=arg32(0); if(p){wr32(p,0);wr32(p+4,0);} }
static void s_InterlockedFlushSList(void){ ret_set(0); }

// ---------------- heap / memory ----------------
static void s_HeapAlloc(void){ uint32_t flags=arg32(1),n=arg32(2); ret_set(guest_alloc(n,(flags&8)!=0)); }
static void s_HeapFree(void){ guest_free(arg32(2)); ret_set(1); }
static void s_HeapReAlloc(void){ uint32_t p=arg32(2),n=arg32(3); ret_set(heap_realloc(p,n)); }
static void s_HeapSize(void){ uint32_t p=arg32(2); ret_set(p?rd32(p-8):0); }
static void s_HeapCreate(void){ ret_set(0xE0000001u); }
static void s_HeapDestroy(void){ ret_set(1); }
static void s_GetProcessHeap(void){ ret_set(0xE0000001u); }
static void s_HeapValidate(void){ ret_set(1); }
static void s_LocalAlloc(void){ uint32_t flags=arg32(0),n=arg32(1); ret_set(guest_alloc(n,(flags&0x40)!=0)); }
static void s_LocalFree(void){ guest_free(arg32(0)); ret_set(0); }
static void s_GlobalAlloc(void){ uint32_t flags=arg32(0),n=arg32(1); ret_set(guest_alloc(n,(flags&0x40)!=0)); }
static void s_GlobalFree(void){ guest_free(arg32(0)); ret_set(0); }
static void s_GlobalLock(void){ ret_set(arg32(0)); }
static void s_GlobalUnlock(void){ ret_set(1); }
// VirtualAlloc(addr, size, type, protect). Delphi's memory manager RESERVEs a big region
// then COMMITs sub-pages within it, relying on the commit returning the requested address.
// Our whole heap is pre-mapped, so a commit at a non-zero addr is a no-op that just echoes
// addr back. Only a fresh (addr==0) reservation/commit consumes heap. Ignoring addr here
// caused double-allocation -> 128MB heap OOM during init.
static void s_VirtualAlloc(void){ uint32_t addr=arg32(0),n=arg32(1),typ=arg32(2);
    if(EMU_VERBOSE) fprintf(stderr,"[VirtualAlloc] addr=%08x size=%u type=%08x\n",addr,n,typ);
    if(addr){ ret_set(addr); return; }                 // commit within an already-mapped reserve
    ret_set(valloc_reserve(n)); }                       // fresh 64KB-aligned reservation
static void s_VirtualFree(void){ ret_set(1); }
static void s_VirtualProtect(void){ uint32_t old=arg32(3); if(old) wr32(old,0x40); ret_set(1); }
static void s_VirtualQuery(void){ ret_set(0); }

// ---------------- strings / charset ----------------
static void s_lstrcpyA(void){ uint32_t d=arg32(0),s=arg32(1); int i=0; for(;;){ uint8_t c=rd8(s+i); wr8(d+i,c); if(!c)break; i++; } ret_set(d); }
static void s_lstrlenA(void){ uint32_t s=arg32(0); int i=0; while(rd8(s+i))i++; ret_set(i); }
static void s_MultiByteToWideChar(void){ uint32_t src=arg32(2); int srclen=(int)arg32(3); uint32_t dst=arg32(4); int dstlen=(int)arg32(5);
    int n=0; for(int i=0; (srclen<0)||(i<srclen); i++){ uint8_t c=rd8(src+i); if(dst&&n<dstlen) wr16(dst+2*n,c); n++; if(srclen<0&&!c)break; } ret_set(n); }
static void s_WideCharToMultiByte(void){ uint32_t src=arg32(2); int srclen=(int)arg32(3); uint32_t dst=arg32(4); int dstlen=(int)arg32(5);
    int n=0; for(int i=0; (srclen<0)||(i<srclen); i++){ uint16_t c=rd16(src+2*i); if(dst&&n<dstlen) wr8(dst+n,(uint8_t)c); n++; if(srclen<0&&!c)break; } ret_set(n); }
static void s_GetStringTypeW(void){ ret_set(1); }
static void s_GetStringTypeA(void){ ret_set(1); }
static void s_LCMapStringW(void){ ret_set(0); }
// LCMapStringA: handle the case-map flags the CRT uses (LCMAP_LOWERCASE/UPPERCASE).
static void s_LCMapStringA(void){ uint32_t flags=arg32(1),src=arg32(2); int n=(int)arg32(3); uint32_t dst=arg32(4); int dn=(int)arg32(5);
    int i=0; for(; (n<0||i<n); i++){ uint8_t c=rd8(src+i); if(flags&0x100){ if(c>='a'&&c<='z')c-=32; } if(flags&0x200){ if(c>='A'&&c<='Z')c+=32; } if(dst&&i<dn)wr8(dst+i,c); if(n<0&&!c)break; } ret_set(n<0?i+1:i); }
static void s_LCMapStringEx(void){ ret_set(0); }
static void s_CompareStringW(void){ ret_set(2); } // CSTR_EQUAL
static void s_GetACP(void){ ret_set(1252); }
static void s_GetOEMCP(void){ ret_set(437); }
static void s_GetCPInfo(void){ uint32_t out=arg32(1); if(out){ wr32(out,1); wr8(out+4,0); wr8(out+5,0);} ret_set(1); }
static void s_IsValidCodePage(void){ ret_set(1); }
static void s_GetLocaleInfoA(void){ uint32_t out=arg32(2),sz=arg32(3); if(out&&sz) guest_strcpy_out(out,"",sz); ret_set(1); }
static void s_GetLocaleInfoW(void){ ret_set(0); }
static void s_GetUserDefaultLCID(void){ ret_set(0x0409); }
static void s_GetThreadLocale(void){ ret_set(0x0409); }
static void s_SetThreadLocale(void){ ret_set(0x0409); }

// ---------------- process / module / system ----------------
static void s_GetVersion(void){ ret_set(0x0A280105u); } // build 10240, ver 5.1 low word (NT)
static void s_GetVersionExA(void){ uint32_t p=arg32(0); if(p){ wr32(p+4,6); wr32(p+8,2); wr32(p+12,9200); wr32(p+16,2);} ret_set(1); }
static void s_GetVersionExW(void){ uint32_t p=arg32(0); if(p){ wr32(p+4,6); wr32(p+8,2); wr32(p+12,9200); wr32(p+16,2);} ret_set(1); }
static void s_GetCommandLineA(void){ ret_set(g_cmdline_a); }
static void s_GetCommandLineW(void){ ret_set(g_cmdline_w); }
static void s_GetModuleHandleA(void){ ret_set(PE.image_base); }
static void s_GetModuleHandleW(void){ ret_set(PE.image_base); }
static void s_GetModuleHandleExW(void){ uint32_t ph=arg32(2); if(ph) wr32(ph,PE.image_base); ret_set(1); }
static void s_GetModuleFileNameA(void){ uint32_t out=arg32(1),sz=arg32(2); guest_strcpy_out(out,"C:\\plugin.dll",sz); ret_set(13); }
static void s_GetModuleFileNameW(void){ uint32_t out=arg32(1),sz=arg32(2); const char* s="C:\\plugin.dll"; int i=0; for(;s[i]&&(uint32_t)i<sz-1;i++) wr16(out+2*i,s[i]); wr16(out+2*i,0); ret_set(i); }
static void s_GetProcAddress(void){ char n[64]; guest_strn(arg32(1),n,sizeof n); emu_log("[GetProcAddress] %s -> 0\n",n); ret_set(0); }
static void s_LoadLibraryA(void){ ret_set(0); }
static void s_LoadLibraryW(void){ ret_set(0); }
static void s_LoadLibraryExW(void){ ret_set(0); }
static void s_FreeLibrary(void){ ret_set(1); }
static void s_GetStartupInfoA(void){ uint32_t p=arg32(0); for(int i=0;i<68;i+=4) wr32(p+i,0); wr32(p,68); }
static void s_GetStartupInfoW(void){ uint32_t p=arg32(0); for(int i=0;i<68;i+=4) wr32(p+i,0); wr32(p,68); }
static void s_GetStdHandle(void){ ret_set(0x10 + arg32(0)); }
static void s_SetStdHandle(void){ ret_set(1); }
static void s_SetHandleCount(void){ ret_set(arg32(0)); }
static void s_GetFileType(void){ ret_set(1); }
static void s_GetCurrentThreadId(void){ ret_set(0x2000); }
static void s_GetCurrentProcessId(void){ ret_set(0x1000); }
static void s_GetCurrentProcess(void){ ret_set(0xFFFFFFFFu); }
static void s_GetCurrentThread(void){ ret_set(0xFFFFFFFEu); }
static void s_GetLastError(void){ ret_set(g_lasterr); }
static void s_SetLastError(void){ g_lasterr=arg32(0); }
static void s_SetUnhandledExceptionFilter(void){ ret_set(0); }
static void s_UnhandledExceptionFilter(void){ ret_set(1); } // EXCEPTION_EXECUTE_HANDLER
static void s_SetErrorMode(void){ ret_set(0); }
static void s_RtlUnwind(void){ /* no-op: SEH unwind not modeled */ }
static void s_RaiseException(void){ emu_log("[RaiseException] code=%08x\n",arg32(0)); CPU.halted=1; CPU.faulted=1; }
static void s_IsDebuggerPresent(void){ ret_set(0); }
// Report SSE(6) and SSE2(10) present (we emulate them); /arch:SSE2 binaries fast-fail otherwise.
static void s_IsProcessorFeaturePresent(void){ uint32_t f=arg32(0); ret_set((f==6||f==10)?1:0); }
static void s_QueryPerformanceCounter(void){ uint32_t p=arg32(0); if(p){ wr32(p,g_tick); wr32(p+4,0); } g_tick+=100; ret_set(1); }
static void s_QueryPerformanceFrequency(void){ uint32_t p=arg32(0); if(p){ wr32(p,1000000); wr32(p+4,0);} ret_set(1); }
// Fill a real FILETIME (100-ns ticks since 1601-01-01). A zero FILETIME maps to
// 1601 -> a NEGATIVE time_t, which _localtime64_s rejects with EINVAL (and the CRT
// then _invoke_watson-terminates with STATUS_STACK_BUFFER_OVERRUN). Plugins that
// timestamp during static-init (e.g. Bitunz) depend on a valid post-1970 value.
// Fixed wall-clock epoch (2024-01-01 00:00:00 UTC). Deterministic on purpose: the splicer
// must render identically every run, and a per-load-varying clock made dblue_Crusher's Delphi
// init take a different path on the 2nd load in a chain (null-deref). Valid post-1970 value
// keeps _localtime64_s / Delphi date validation happy (see Bitunz fix).
#define EMU_FIXED_UNIX 1704067200ULL
static uint64_t emu_filetime_now(void){
    return (EMU_FIXED_UNIX + 11644473600ULL) * 10000000ULL;
}
static void s_GetSystemTimeAsFileTime(void){ uint32_t p=arg32(0); if(p){ uint64_t ft=emu_filetime_now(); wr32(p,(uint32_t)ft); wr32(p+4,(uint32_t)(ft>>32)); } }
// CompareStringA(Locale,flags,s1,n1,s2,n2) -> 1/2/3 (less/equal/greater). Delphi's
// AnsiCompareText/Str uses the result, so return a real comparison (case-insensitive if
// NORM_IGNORECASE=1). n<0 means NUL-terminated. argbytes=24.
static void s_CompareStringA(void){
    uint32_t flags=arg32(1), s1=arg32(2); int n1=(int)arg32(3);
    uint32_t s2=arg32(4); int n2=(int)arg32(5);
    char a[256],b[256]; int i;
    for(i=0;i<255 && (n1<0? rd8(s1+i)!=0 : i<n1); i++) a[i]=(char)rd8(s1+i); a[i]=0;
    for(i=0;i<255 && (n2<0? rd8(s2+i)!=0 : i<n2); i++) b[i]=(char)rd8(s2+i); b[i]=0;
    int c;
    if(flags&1){ for(i=0;a[i]&&b[i];i++){ int ca=tolower((unsigned char)a[i]),cb=tolower((unsigned char)b[i]); if(ca!=cb)break; } c=tolower((unsigned char)a[i])-tolower((unsigned char)b[i]); }
    else c=strcmp(a,b);
    ret_set(c<0?1:(c>0?3:2));
}
// Fill a SYSTEMTIME (8x WORD) from the host clock. A zeroed SYSTEMTIME (wYear=0) trips
// Delphi/VCL date validation the same way a zero FILETIME does. argbytes=4.
static void s_GetLocalTime(void){
    uint32_t p=arg32(0); if(!p) return;
    time_t t=(time_t)EMU_FIXED_UNIX;
    struct tm* lt=gmtime(&t); struct tm fb={0}; if(!lt){ fb.tm_year=124; fb.tm_mon=0; fb.tm_mday=1; lt=&fb; }
    wr16(p+0,(uint16_t)(lt->tm_year+1900)); wr16(p+2,(uint16_t)(lt->tm_mon+1));
    wr16(p+4,(uint16_t)lt->tm_wday);        wr16(p+6,(uint16_t)lt->tm_mday);
    wr16(p+8,(uint16_t)lt->tm_hour);        wr16(p+10,(uint16_t)lt->tm_min);
    wr16(p+12,(uint16_t)lt->tm_sec);        wr16(p+14,0);
}
// MulDiv(nNumber, nNumerator, nDenominator) -> round((a*b)/c), half away from zero; -1 on /0 or overflow.
// Critical: stdcall (12 argbytes). Unshimmed it defaulted to argbytes=0, leaving 12 bytes of args on the
// stack -> the caller's saved regs/return address desync -> ret into a Delphi data table (dblue_Crusher).
static void s_MulDiv(void){
    int32_t a=(int32_t)arg32(0), b=(int32_t)arg32(1), c=(int32_t)arg32(2);
    if(c==0){ ret_set(0xFFFFFFFFu); return; }
    int64_t v=(int64_t)a*(int64_t)b;
    if(((v<0)?-1:1) == ((c<0)?-1:1)) v += c/2; else v -= c/2;
    int64_t r=v/c;
    if(r>2147483647LL || r<-2147483648LL){ ret_set(0xFFFFFFFFu); return; }
    ret_set((uint32_t)(int32_t)r);
}
static void s_GetTickCount(void){ ret_set(g_tick++); }
static void s_GetTickCount64(void){ ret_set(g_tick++); }
static void s_Sleep(void){}
static void s_ExitProcess(void){ emu_log("[ExitProcess] %u\n",arg32(0)); CPU.halted=1; }
static void s_TerminateProcess(void){ CPU.halted=1; }
static void s_EncodePointer(void){ ret_set(arg32(0)); } // identity
static void s_DecodePointer(void){ ret_set(arg32(0)); }
static void s_GetSystemInfo(void){ uint32_t p=arg32(0); if(p){ for(int i=0;i<48;i+=4) wr32(p+i,0); wr32(p+0,0); wr32(p+4,0x1000); wr32(p+0x14,1);/*numproc*/ wr32(p+0x24,1);} }
static void s_GetSystemTimeAsFileTimeW(void){ }

// ---------------- bad-ptr checks (treat all guest memory as valid) ----------------
static void s_IsBadReadPtr(void){ ret_set(0); }
static void s_IsBadWritePtr(void){ ret_set(0); }
static void s_IsBadCodePtr(void){ ret_set(0); }
static void s_IsBadStringPtrA(void){ ret_set(0); }
static void s_IsBadStringPtrW(void){ ret_set(0); }

// ---------------- file I/O (no real FS; succeed-noop or fail) ----------------
static void s_WriteFile(void){ uint32_t n=arg32(2),pg=arg32(3); if(pg)wr32(pg,n); ret_set(1); }
static void s_ReadFile(void){ uint32_t pg=arg32(3); if(pg)wr32(pg,0); ret_set(0); }
static void s_CreateFileA(void){ g_lasterr=2; ret_set(0xFFFFFFFFu); }
static void s_CreateFileW(void){ g_lasterr=2; ret_set(0xFFFFFFFFu); }
static void s_CloseHandle(void){ ret_set(1); }
static void s_FlushFileBuffers(void){ ret_set(1); }
static void s_SetFilePointer(void){ ret_set(0); }
static void s_SetFilePointerEx(void){ ret_set(1); }
static void s_GetFileAttributesA(void){ ret_set(0xFFFFFFFFu); }
static void s_GetFileAttributesW(void){ ret_set(0xFFFFFFFFu); }
static void s_FindClose(void){ ret_set(1); }
static void s_FindFirstFileExA(void){ ret_set(0xFFFFFFFFu); }
static void s_FindFirstFileA(void){ ret_set(0xFFFFFFFFu); }
static void s_FindNextFileA(void){ ret_set(0); }
static void s_GetConsoleCP(void){ ret_set(437); }
static void s_GetConsoleMode(void){ ret_set(0); }
static void s_GetConsoleOutputCP(void){ ret_set(437); }
static void s_WriteConsoleW(void){ ret_set(1); }
static void s_WriteConsoleA(void){ ret_set(1); }
static void s_GetOSFHandle(void){ ret_set(0xFFFFFFFFu); }

// ---------------- env ----------------
static void s_GetEnvironmentStrings(void){ ret_set(g_env_a); }
static void s_GetEnvironmentStringsW(void){ ret_set(g_env_w); }
static void s_FreeEnvironmentStringsA(void){ ret_set(1); }
static void s_FreeEnvironmentStringsW(void){ ret_set(1); }
static void s_GetEnvironmentVariableA(void){ ret_set(0); }
static void s_GetEnvironmentVariableW(void){ ret_set(0); }
static void s_SetEnvironmentVariableA(void){ ret_set(1); }

// ---------------- sync objects ----------------
static void s_WaitForSingleObject(void){ ret_set(0); }
static void s_CreateSemaphoreA(void){ ret_set(0xC0000001u); }
static void s_CreateSemaphoreW(void){ ret_set(0xC0000001u); }
static void s_ReleaseSemaphore(void){ ret_set(1); }
static void s_CreateEventA(void){ ret_set(0xC0000002u); }
static void s_SetEvent(void){ ret_set(1); }
static void s_ResetEvent(void){ ret_set(1); }
static void s_CreateMutexA(void){ ret_set(0xC0000003u); }
static void s_ReleaseMutex(void){ ret_set(1); }

// ---------------- message box (GUI plugins) ----------------
static void s_MessageBoxA(void){ char t[256],c[256]; guest_strn(arg32(1),c,sizeof c); guest_strn(arg32(2),t,sizeof t); emu_log("[MessageBox] %s | %s\n",t,c); ret_set(1); }
static void s_OutputDebugStringA(void){ char s[256]; guest_strn(arg32(0),s,sizeof s); emu_log("[dbg] %s",s); }

// ============================================================================
//   Universal CRT (api-ms-win-crt-*) + vcruntime host layer. Plugins built with
//   modern MSVC dynamically import the CRT; its startup runs the global ctors
//   (_initterm) that construct the plugin object. All cdecl (argbytes 0).
// ============================================================================
// Re-entrant guest call that PRESERVES the in-progress call's stack (unlike call_guest,
// which resets ESP). Used to run global constructors from inside _initterm.
static void call_nested(uint32_t fn){
    /* spfy debug: log ESP at ctor entry/exit when SPFY_ESPLOG=1. */
    static int esplog=-1; if(esplog<0) esplog = getenv("SPFY_ESPLOG")?1:0;
    uint32_t esp_in = CPU.r[ESP];
    cpu_t save = CPU;
    cpu_push32(RET_SENTINEL);
    CPU.eip = fn; CPU.halted = 0; CPU.faulted = 0;
    cpu_run(2000000000ULL);
    int faulted = CPU.faulted;
    uint32_t esp_post_ctor = CPU.r[ESP];   /* ESP from inside the ctor at the cpu_run exit */
    uint32_t eip_post_ctor = CPU.eip;
    CPU = save;                       // restore regs/eip/esp; guest memory effects persist
    if (faulted) emu_log("[initterm] ctor %08x faulted\n", fn);
    if (esplog) fprintf(stderr, "[esp] call_nested fn=%08x in_esp=%08x post_ctor_esp=%08x post_ctor_eip=%08x restored_esp=%08x faulted=%d\n",
                        fn, esp_in, esp_post_ctor, eip_post_ctor, CPU.r[ESP], faulted);
}
static void s_initterm(void){   // void _initterm(PVFV* first, PVFV* last)
    uint32_t first=arg32(0), last=arg32(1);
    for(uint32_t p=first; p+4<=last; p+=4){ uint32_t fn=rd32(p); if(fn) call_nested(fn); }
}
static void s_initterm_e(void){ // int _initterm_e(...) - returns 0 on success
    uint32_t first=arg32(0), last=arg32(1);
    for(uint32_t p=first; p+4<=last; p+=4){ uint32_t fn=rd32(p); if(fn) call_nested(fn); }
    ret_set(0);
}

// CRT heap (cdecl) -> guest heap
static void s_malloc(void){ ret_set(guest_alloc(arg32(0),0)); }
static void s_calloc(void){ ret_set(guest_alloc(arg32(0)*arg32(1),1)); }
static void s_realloc(void){ ret_set(heap_realloc(arg32(0),arg32(1))); }
static void s_free(void){ guest_free(arg32(0)); }
static void s_msize(void){ uint32_t p=arg32(0); ret_set(p?rd32(p-8):0); }
static void s_callnewh(void){ ret_set(0); }
static void s_set_new_mode(void){ ret_set(0); }

// CRT mem/string (cdecl)
static void s_memcpy(void){ uint32_t d=arg32(0),s=arg32(1),n=arg32(2); uint8_t*hd=mem_host(d),*hs=mem_host(s);
    if(hd&&hs) memcpy(hd,hs,n); ret_set(d); }   // skip if either side unmapped (don't hard-fault the run)
static void s_memmove(void){ uint32_t d=arg32(0),s=arg32(1),n=arg32(2); uint8_t*hd=mem_host(d),*hs=mem_host(s);
    if(hd&&hs) memmove(hd,hs,n); ret_set(d); }
static void s_memset(void){ uint32_t d=arg32(0),c=arg32(1),n=arg32(2); uint8_t*hd=mem_host(d);
    if(hd) memset(hd,(int)c,n); ret_set(d); }
static void s_strlen(void){ uint32_t s=arg32(0); int i=0; while(rd8(s+i))i++; ret_set(i); }
static void s_strcmp(void){ uint32_t a=arg32(0),b=arg32(1); for(int i=0;;i++){ uint8_t x=rd8(a+i),y=rd8(b+i); if(x!=y){ret_set((int)x-(int)y);return;} if(!x)break; } ret_set(0); }
static void s_strncmp(void){ uint32_t a=arg32(0),b=arg32(1); int n=(int)arg32(2); for(int i=0;i<n;i++){ uint8_t x=rd8(a+i),y=rd8(b+i); if(x!=y){ret_set((int)x-(int)y);return;} if(!x)break; } ret_set(0); }
static void s_strcpy(void){ uint32_t d=arg32(0),s=arg32(1); int i=0; for(;;){ uint8_t c=rd8(s+i); wr8(d+i,c); if(!c)break; i++; } ret_set(d); }
static void s_strncpy(void){ uint32_t d=arg32(0),s=arg32(1); int n=(int)arg32(2); int i=0; for(;i<n;i++){ uint8_t c=rd8(s+i); wr8(d+i,c); if(!c){ for(i++;i<n;i++) wr8(d+i,0); break; } } ret_set(d); }
static void s_strstr(void){ uint32_t h=arg32(0),n=arg32(1); if(!rd8(n)){ret_set(h);return;} for(uint32_t i=0;rd8(h+i);i++){ uint32_t j=0; while(rd8(n+j)&&rd8(h+i+j)==rd8(n+j))j++; if(!rd8(n+j)){ret_set(h+i);return;} } ret_set(0); }
static void s_memchr(void){ uint32_t s=arg32(0),c=arg32(1)&0xff; int n=(int)arg32(2); for(int i=0;i<n;i++) if(rd8(s+i)==c){ret_set(s+i);return;} ret_set(0); }

// CRT startup / onexit / environment (cdecl stubs)
static void s_ret0c(void){ ret_set(0); }
static void s_nop0c(void){}
static uint32_t g_errno_va=0, g_iob_va=0, g_env_strs=0;
static void s_errno(void){ ret_set(g_errno_va); }            // int* _errno()
static void s_acrt_iob_func(void){ ret_set(g_iob_va + arg32(0)*0x20); }   // FILE* __acrt_iob_func(i)
static void s_get_narrow_env(void){ ret_set(g_env_strs); }   // char** _get_initial_narrow_environment()
static void s_invalid_parameter(void){ emu_log("[crt] invalid_parameter (ignored)\n"); } // do NOT abort
static void s_purecall(void){ emu_log("[crt] purecall\n"); }
static void s_terminate(void){ emu_log("[crt] terminate()\n"); CPU.halted=1; }
static void s_CxxThrowException(void){ emu_log("[crt] C++ exception thrown\n"); CPU.halted=1; CPU.faulted=1; }
static void s_CxxFrameHandler(void){ ret_set(1); }           // ExceptionContinueSearch
static void s_stdio_vsprintf(void){ ret_set(0); }
static void s_stdio_vfprintf(void){ ret_set(0); }

// MSVC _libm_sse2_*_precise: arg(s) in XMM0 (and XMM1), result in XMM0 (double). No stack args.
static void s_libm_log(void){ CPU.xmm[0].d[0]=log(CPU.xmm[0].d[0]); }
static void s_libm_log10(void){ CPU.xmm[0].d[0]=log10(CPU.xmm[0].d[0]); }
static void s_libm_exp(void){ CPU.xmm[0].d[0]=exp(CPU.xmm[0].d[0]); }
static void s_libm_pow(void){ CPU.xmm[0].d[0]=pow(CPU.xmm[0].d[0],CPU.xmm[1].d[0]); }
static void s_libm_sin(void){ CPU.xmm[0].d[0]=sin(CPU.xmm[0].d[0]); }
static void s_libm_cos(void){ CPU.xmm[0].d[0]=cos(CPU.xmm[0].d[0]); }
static void s_libm_tan(void){ CPU.xmm[0].d[0]=tan(CPU.xmm[0].d[0]); }
static void s_libm_sqrt(void){ CPU.xmm[0].d[0]=sqrt(CPU.xmm[0].d[0]); }
static void s_libm_sinh(void){ CPU.xmm[0].d[0]=sinh(CPU.xmm[0].d[0]); }
static void s_libm_atan2(void){ CPU.xmm[0].d[0]=atan2(CPU.xmm[0].d[0],CPU.xmm[1].d[0]); }
static void s_libm_hypot(void){ CPU.xmm[0].d[0]=sqrt(CPU.xmm[0].d[0]*CPU.xmm[0].d[0]+CPU.xmm[1].d[0]*CPU.xmm[1].d[0]); }
// classic cdecl libm (arg on stack as double, result in ST0)
static void s_c_floor(void){ uint64_t b=rd32(CPU.r[ESP]+4)|((uint64_t)rd32(CPU.r[ESP]+8)<<32); double x; memcpy(&x,&b,8); CPU.fpu_top=(CPU.fpu_top-1)&7; CPU.st[CPU.fpu_top]=floor(x); }
static void s_c_ceil(void){ uint64_t b=rd32(CPU.r[ESP]+4)|((uint64_t)rd32(CPU.r[ESP]+8)<<32); double x; memcpy(&x,&b,8); CPU.fpu_top=(CPU.fpu_top-1)&7; CPU.st[CPU.fpu_top]=ceil(x); }

// ---- minimal GUI / OLE / winmm so init that creates a (hidden) window survives ----
static uint32_t g_hwndSeq=0x00CC0001;
static void s_OleInitialize(void){ ret_set(0); }       // S_OK
static void s_CoInitializeEx(void){ ret_set(0); }
static void s_RegisterClass(void){ ret_set(0xC001); }  // fake ATOM
static void s_CreateWindowEx(void){ ret_set(g_hwndSeq++); }  // fake HWND
static void s_DefWindowProc(void){ ret_set(0); }
static void s_GetClientRect(void){ uint32_t r=arg32(1); if(r){wr32(r,0);wr32(r+4,0);wr32(r+8,100);wr32(r+12,100);} ret_set(1); }
static void s_timeGetTime(void){ ret_set(g_tick++); }
// Some plugins treat sync handles as pointers to their own structs -> hand out real guest buffers.
static void s_CreateEventW(void){ ret_set(guest_alloc(256,1)); }

// ---- Delphi/Borland RTL init: user32/advapi32/oleaut32 (correct argbytes is what matters) ----
static void s_GetKeyboardType(void){ ret_set(0); }
static void s_CharNextA(void){ uint32_t p=arg32(0); ret_set(rd8(p)?p+1:p); }
static void s_CharPrevA(void){ uint32_t s=arg32(0),p=arg32(1); ret_set(p>s?p-1:s); }
static void s_RegOpenKey(void){ ret_set(2); }       // ERROR_FILE_NOT_FOUND
static void s_RegQueryValue(void){ ret_set(2); }
static void s_RegCloseKey(void){ ret_set(0); }
static void s_RegCreateKey(void){ ret_set(5); }     // ERROR_ACCESS_DENIED
static void s_lstrcpynA(void){ uint32_t d=arg32(0),s=arg32(1); int n=(int)arg32(2); int i=0; for(;i<n-1;i++){ uint8_t c=rd8(s+i); wr8(d+i,c); if(!c)break; } if(n>0)wr8(d+i,0); ret_set(d); }
static void s_SysAllocStringLen(void){ uint32_t len=arg32(1); ret_set(guest_alloc(len*2+8,1)); } // BSTR-ish
static void s_SysFreeString(void){ guest_free(arg32(0)); }

// ---- GDI / screen DC (VCL/GUI frameworks query the display at init) ----
static void s_GetDC(void){ ret_set(0xDC000001u); }                 // fake screen DC
static void s_GetDeviceCaps(void){ uint32_t i=arg32(1); uint32_t v=0;
    switch(i){ case 8:v=1920;break; case 10:v=1080;break; case 12:v=32;break; case 14:v=1;break;   // HORZRES/VERTRES/BITSPIXEL/PLANES
        case 88:case 90:v=96;break; case 24:v=0xFFFFFFFFu;break; case 38:v=0x6F1D;break; default:v=0; } ret_set(v); }
static void s_GetSystemMetrics(void){ uint32_t i=arg32(0); ret_set(i==0?1920:i==1?1080:0); }
static uint32_t g_gdiSeq=0x60000001;
static void s_gdi_handle(void){ ret_set(g_gdiSeq++); }              // generic GDI object handle

// ---------------- spfy extra shims (defined in spfy_extra_shims.c) ----------------
extern void s_DisableThreadLibraryCalls(void);
extern void s_SearchPathA(void);
extern void s_winmm_zero(void);
extern void s_strdup(void);
extern void s_stricmp(void);
extern void s_strchr(void);
extern void s_strtol(void);
extern void s_strtod(void);
extern void s_atof(void);
extern void s_atoi(void);
extern void s_atol(void);
extern void s_ldiv(void);
extern void s_toupper(void);
extern void s_isdigit(void);
extern void s_isspace(void);
extern void s_fopen(void);
extern void s_fclose(void);
extern void s_fread(void);
extern void s_fwrite(void);
extern void s_fseek(void);
extern void s_ftell(void);
extern void s_rewind(void);
extern void s_fflush(void);
extern void s_fputs(void);
extern void s_fgets(void);
extern void s_getchar(void);
extern void s_fileno(void);
extern void s_stat(void);
extern void s_fstat(void);
extern void s_iob_var(void);
extern void s_sprintf(void);
extern void s_vsprintf(void);
extern void s_sscanf(void);
extern void s_vfprintf(void);
extern void s_fprintf(void);
extern void s_pow_cdecl(void);
extern void s_log_cdecl(void);
extern void s_exp_cdecl(void);
/* s_CxxFrameHandler is defined statically below — used for both
 * __CxxFrameHandler3/4 (donor entries) and __CxxFrameHandler (spfy
 * entry). No extern needed; just reference the static directly in REG[]. */
extern void s_except_handler3(void);
extern void s_CppXcptFilter(void);
extern void s_security_error_handler(void);
extern void s_terminate_msvc(void);
extern void s_adjust_fdiv(void);
extern void s_dllonexit(void);
extern void s_onexit(void);
extern void s_setjmp3(void);
extern void s_longjmp(void);
extern void s_exit_cdecl(void);
extern void s_clock(void);

// ---------------- argbytes lookup ----------------
typedef struct { const char* name; shim_fn fn; int argb; } reg_t;
static const reg_t REG[] = {
    {"EnterCriticalSection",s_EnterCriticalSection,4},{"LeaveCriticalSection",s_LeaveCriticalSection,4},
    {"InitializeCriticalSection",s_InitializeCriticalSection,4},{"DeleteCriticalSection",s_DeleteCriticalSection,4},
    {"InitializeCriticalSectionAndSpinCount",s_InitializeCriticalSectionAndSpinCount,8},
    {"InitializeCriticalSectionEx",s_InitializeCriticalSectionEx,12},
    {"TlsAlloc",s_TlsAlloc,0},{"TlsFree",s_TlsFree,4},{"TlsSetValue",s_TlsSetValue,8},{"TlsGetValue",s_TlsGetValue,4},
    {"FlsAlloc",s_FlsAlloc,4},{"FlsFree",s_FlsFree,4},{"FlsSetValue",s_FlsSetValue,8},{"FlsGetValue",s_FlsGetValue,4},
    {"InterlockedIncrement",s_InterlockedIncrement,4},{"InterlockedDecrement",s_InterlockedDecrement,4},
    {"InterlockedExchange",s_InterlockedExchange,8},{"InterlockedCompareExchange",s_InterlockedCompareExchange,12},
    {"InitializeSListHead",s_InitializeSListHead,4},{"InterlockedFlushSList",s_InterlockedFlushSList,4},
    {"HeapAlloc",s_HeapAlloc,12},{"HeapFree",s_HeapFree,12},{"HeapReAlloc",s_HeapReAlloc,16},{"HeapSize",s_HeapSize,12},
    {"HeapCreate",s_HeapCreate,12},{"HeapDestroy",s_HeapDestroy,4},{"GetProcessHeap",s_GetProcessHeap,0},{"HeapValidate",s_HeapValidate,12},
    {"LocalAlloc",s_LocalAlloc,8},{"LocalFree",s_LocalFree,4},
    {"GlobalAlloc",s_GlobalAlloc,8},{"GlobalFree",s_GlobalFree,4},{"GlobalLock",s_GlobalLock,4},{"GlobalUnlock",s_GlobalUnlock,4},
    {"VirtualAlloc",s_VirtualAlloc,16},{"VirtualFree",s_VirtualFree,12},{"VirtualProtect",s_VirtualProtect,16},{"VirtualQuery",s_VirtualQuery,12},
    {"lstrcpyA",s_lstrcpyA,8},{"lstrlenA",s_lstrlenA,4},
    {"MultiByteToWideChar",s_MultiByteToWideChar,24},{"WideCharToMultiByte",s_WideCharToMultiByte,32},
    {"GetStringTypeW",s_GetStringTypeW,16},{"GetStringTypeA",s_GetStringTypeA,20},
    {"LCMapStringW",s_LCMapStringW,24},{"LCMapStringA",s_LCMapStringA,24},{"LCMapStringEx",s_LCMapStringEx,36},{"CompareStringW",s_CompareStringW,24},
    {"GetACP",s_GetACP,0},{"GetOEMCP",s_GetOEMCP,0},{"GetCPInfo",s_GetCPInfo,8},{"IsValidCodePage",s_IsValidCodePage,4},
    {"GetLocaleInfoA",s_GetLocaleInfoA,16},{"GetLocaleInfoW",s_GetLocaleInfoW,16},
    {"GetUserDefaultLCID",s_GetUserDefaultLCID,0},{"GetThreadLocale",s_GetThreadLocale,0},{"SetThreadLocale",s_SetThreadLocale,4},
    {"GetVersion",s_GetVersion,0},{"GetVersionExA",s_GetVersionExA,4},{"GetVersionExW",s_GetVersionExW,4},
    {"GetCommandLineA",s_GetCommandLineA,0},{"GetCommandLineW",s_GetCommandLineW,0},
    {"GetModuleHandleA",s_GetModuleHandleA,4},{"GetModuleHandleW",s_GetModuleHandleW,4},{"GetModuleHandleExW",s_GetModuleHandleExW,12},
    {"GetModuleFileNameA",s_GetModuleFileNameA,12},{"GetModuleFileNameW",s_GetModuleFileNameW,12},
    {"GetProcAddress",s_GetProcAddress,8},
    {"LoadLibraryA",s_LoadLibraryA,4},{"LoadLibraryW",s_LoadLibraryW,4},{"LoadLibraryExW",s_LoadLibraryExW,12},{"FreeLibrary",s_FreeLibrary,4},
    {"GetStartupInfoA",s_GetStartupInfoA,4},{"GetStartupInfoW",s_GetStartupInfoW,4},
    {"GetStdHandle",s_GetStdHandle,4},{"SetStdHandle",s_SetStdHandle,8},{"SetHandleCount",s_SetHandleCount,4},{"GetFileType",s_GetFileType,4},
    {"MulDiv",s_MulDiv,12},
    {"GetCurrentThreadId",s_GetCurrentThreadId,0},{"GetCurrentProcessId",s_GetCurrentProcessId,0},
    {"GetCurrentProcess",s_GetCurrentProcess,0},{"GetCurrentThread",s_GetCurrentThread,0},
    {"GetLastError",s_GetLastError,0},{"SetLastError",s_SetLastError,4},
    {"SetUnhandledExceptionFilter",s_SetUnhandledExceptionFilter,4},{"UnhandledExceptionFilter",s_UnhandledExceptionFilter,4},
    {"SetErrorMode",s_SetErrorMode,4},{"RtlUnwind",s_RtlUnwind,16},{"RaiseException",s_RaiseException,16},
    {"IsDebuggerPresent",s_IsDebuggerPresent,0},{"IsProcessorFeaturePresent",s_IsProcessorFeaturePresent,4},
    {"QueryPerformanceCounter",s_QueryPerformanceCounter,4},{"QueryPerformanceFrequency",s_QueryPerformanceFrequency,4},
    {"GetSystemTimeAsFileTime",s_GetSystemTimeAsFileTime,4},{"GetTickCount",s_GetTickCount,0},{"GetTickCount64",s_GetTickCount64,0},
    {"Sleep",s_Sleep,4},{"ExitProcess",s_ExitProcess,4},{"TerminateProcess",s_TerminateProcess,8},
    {"EncodePointer",s_EncodePointer,4},{"DecodePointer",s_DecodePointer,4},
    {"GetSystemInfo",s_GetSystemInfo,4},{"GetNativeSystemInfo",s_GetSystemInfo,4},
    {"IsBadReadPtr",s_IsBadReadPtr,8},{"IsBadWritePtr",s_IsBadWritePtr,8},{"IsBadCodePtr",s_IsBadCodePtr,4},
    {"IsBadStringPtrA",s_IsBadStringPtrA,8},{"IsBadStringPtrW",s_IsBadStringPtrW,8},
    {"WriteFile",s_WriteFile,20},{"ReadFile",s_ReadFile,20},{"CreateFileA",s_CreateFileA,28},{"CreateFileW",s_CreateFileW,28},
    {"CloseHandle",s_CloseHandle,4},{"FlushFileBuffers",s_FlushFileBuffers,4},
    {"SetFilePointer",s_SetFilePointer,16},{"SetFilePointerEx",s_SetFilePointerEx,20},
    {"GetFileAttributesA",s_GetFileAttributesA,4},{"GetFileAttributesW",s_GetFileAttributesW,4},
    {"FindClose",s_FindClose,4},{"FindFirstFileExA",s_FindFirstFileExA,24},{"FindFirstFileA",s_FindFirstFileA,8},{"FindNextFileA",s_FindNextFileA,8},
    {"GetConsoleCP",s_GetConsoleCP,0},{"GetConsoleMode",s_GetConsoleMode,8},{"GetConsoleOutputCP",s_GetConsoleOutputCP,0},
    {"WriteConsoleW",s_WriteConsoleW,20},{"WriteConsoleA",s_WriteConsoleA,20},
    {"GetEnvironmentStrings",s_GetEnvironmentStrings,0},{"GetEnvironmentStringsW",s_GetEnvironmentStringsW,0},
    {"FreeEnvironmentStringsA",s_FreeEnvironmentStringsA,4},{"FreeEnvironmentStringsW",s_FreeEnvironmentStringsW,4},
    {"GetEnvironmentVariableA",s_GetEnvironmentVariableA,12},{"GetEnvironmentVariableW",s_GetEnvironmentVariableW,12},
    {"SetEnvironmentVariableA",s_SetEnvironmentVariableA,8},
    {"WaitForSingleObject",s_WaitForSingleObject,8},
    {"CreateSemaphoreA",s_CreateSemaphoreA,16},{"CreateSemaphoreW",s_CreateSemaphoreW,16},{"ReleaseSemaphore",s_ReleaseSemaphore,12},
    {"CreateEventA",s_CreateEventA,16},{"SetEvent",s_SetEvent,4},{"ResetEvent",s_ResetEvent,4},
    {"CreateMutexA",s_CreateMutexA,12},{"ReleaseMutex",s_ReleaseMutex,4},
    {"MessageBoxA",s_MessageBoxA,16},{"OutputDebugStringA",s_OutputDebugStringA,4},
    // --- Universal CRT / vcruntime (cdecl, argbytes 0) ---
    {"_initterm",s_initterm,0},{"_initterm_e",s_initterm_e,0},
    {"malloc",s_malloc,0},{"calloc",s_calloc,0},{"realloc",s_realloc,0},{"free",s_free,0},
    {"_msize",s_msize,0},{"_callnewh",s_callnewh,0},{"_set_new_mode",s_set_new_mode,0},{"??2@YAPAXI@Z",s_malloc,0},{"??3@YAXPAX@Z",s_free,0},
    {"memcpy",s_memcpy,0},{"memmove",s_memmove,0},{"memset",s_memset,0},{"memchr",s_memchr,0},
    {"strlen",s_strlen,0},{"strcmp",s_strcmp,0},{"strncmp",s_strncmp,0},{"strcpy",s_strcpy,0},{"strncpy",s_strncpy,0},{"strstr",s_strstr,0},
    {"_initialize_onexit_table",s_ret0c,0},{"_register_onexit_function",s_ret0c,0},{"_execute_onexit_table",s_ret0c,0},{"_crt_atexit",s_ret0c,0},
    {"_configure_narrow_argv",s_ret0c,0},{"_initialize_narrow_environment",s_ret0c,0},{"_get_initial_narrow_environment",s_get_narrow_env,0},
    {"_set_app_type",s_nop0c,0},{"_seh_filter_dll",s_ret0c,0},{"_seh_filter_exe",s_ret0c,0},{"_cexit",s_nop0c,0},{"_c_exit",s_nop0c,0},
    {"_fpreset",s_nop0c,0},{"__setusermatherr",s_nop0c,0},{"_controlfp_s",s_ret0c,0},{"_configthreadlocale",s_ret0c,0},
    {"_invalid_parameter_noinfo_noreturn",s_invalid_parameter,0},{"_invalid_parameter_noinfo",s_invalid_parameter,0},
    {"_errno",s_errno,0},{"__acrt_iob_func",s_acrt_iob_func,0},{"_get_stream_buffer_pointers",s_ret0c,0},
    {"__stdio_common_vsprintf",s_stdio_vsprintf,0},{"__stdio_common_vfprintf",s_stdio_vfprintf,0},{"__stdio_common_vsscanf",s_ret0c,0},
    {"terminate",s_terminate,0},{"_purecall",s_purecall,0},
    {"_CxxThrowException",s_CxxThrowException,8},{"__CxxFrameHandler3",s_CxxFrameHandler,0},{"__CxxFrameHandler4",s_CxxFrameHandler,0},
    {"_except_handler4_common",s_CxxFrameHandler,0},{"__std_terminate",s_terminate,0},
    {"__vcrt_InitializeCriticalSectionEx",s_InitializeCriticalSectionAndSpinCount,12},
    {"_beginthreadex",s_ret0c,0},{"_endthreadex",s_nop0c,0},{"_register_thread_local_exe_atexit_callback",s_ret0c,0},
    // --- MSVC SSE2 libm (XMM ABI, no stack args) + classic libm ---
    {"_libm_sse2_log_precise",s_libm_log,0},{"_libm_sse2_log10_precise",s_libm_log10,0},{"_libm_sse2_exp_precise",s_libm_exp,0},
    {"_libm_sse2_pow_precise",s_libm_pow,0},{"_libm_sse2_sin_precise",s_libm_sin,0},{"_libm_sse2_cos_precise",s_libm_cos,0},
    {"_libm_sse2_tan_precise",s_libm_tan,0},{"_libm_sse2_sqrt_precise",s_libm_sqrt,0},{"_CIsinh",s_libm_sinh,0},{"_CIatan2",s_libm_atan2,0},
    {"_hypot",s_libm_hypot,0},{"_hypotf",s_libm_hypot,0},{"floor",s_c_floor,0},{"ceil",s_c_ceil,0},{"log10",s_ret0c,0},
    // --- GUI / OLE / winmm (return plausible fake handles so hidden-window init survives) ---
    {"OleInitialize",s_OleInitialize,4},{"OleUninitialize",s_nop0c,0},{"CoInitialize",s_OleInitialize,4},{"CoInitializeEx",s_CoInitializeEx,8},{"CoUninitialize",s_nop0c,0},
    {"RegisterClassExW",s_RegisterClass,4},{"RegisterClassExA",s_RegisterClass,4},{"RegisterClassW",s_RegisterClass,4},{"RegisterClassA",s_RegisterClass,4},
    {"UnregisterClassW",s_ret1,8},{"UnregisterClassA",s_ret1,8},
    {"CreateWindowExW",s_CreateWindowEx,48},{"CreateWindowExA",s_CreateWindowEx,48},
    {"DefWindowProcW",s_DefWindowProc,16},{"DefWindowProcA",s_DefWindowProc,16},
    {"DestroyWindow",s_ret1,4},{"ShowWindow",s_ret1,8},{"UpdateWindow",s_ret1,4},{"MoveWindow",s_ret1,24},{"SetWindowPos",s_ret1,28},
    {"GetClientRect",s_GetClientRect,8},{"GetWindowRect",s_GetClientRect,8},
    {"SetWindowLongW",s_ret0,12},{"SetWindowLongA",s_ret0,12},{"GetWindowLongW",s_ret0,8},{"GetWindowLongA",s_ret0,8},
    {"SetTimer",s_ret1,16},{"KillTimer",s_ret1,8},{"SetPropW",s_ret1,12},{"GetPropW",s_ret0,8},
    {"CreateEventW",s_CreateEventW,16},{"CreateMutexW",s_CreateMutexA,12},
    {"timeBeginPeriod",s_ret0,4},{"timeEndPeriod",s_ret0,4},{"timeGetTime",s_timeGetTime,0},
    // --- Delphi/Borland RTL init (user32/advapi32/oleaut32) ---
    {"GetKeyboardType",s_GetKeyboardType,4},{"LoadStringA",s_ret0,16},{"LoadStringW",s_ret0,16},
    {"CharNextA",s_CharNextA,4},{"CharNextW",s_CharNextA,4},{"CharPrevA",s_CharPrevA,8},
    {"CharToOemA",s_ret1,8},{"OemToCharA",s_ret1,8},{"CharUpperA",s_ret0,4},{"CharLowerA",s_ret0,4},{"CharUpperBuffA",s_ret0,8},
    {"GetKeyboardLayout",s_ret0,4},{"ActivateKeyboardLayout",s_ret0,8},{"LoadKeyboardLayoutA",s_ret0,8},
    {"RegOpenKeyExA",s_RegOpenKey,20},{"RegOpenKeyExW",s_RegOpenKey,20},{"RegOpenKeyA",s_RegOpenKey,12},
    {"RegQueryValueExA",s_RegQueryValue,24},{"RegQueryValueExW",s_RegQueryValue,24},{"RegQueryValueA",s_RegQueryValue,16},
    {"RegCloseKey",s_RegCloseKey,4},{"RegCreateKeyExA",s_RegCreateKey,36},{"RegCreateKeyExW",s_RegCreateKey,36},
    {"RegSetValueExA",s_ret0,24},{"RegEnumKeyExA",s_ret0,32},{"RegEnumValueA",s_ret0,32},{"RegDeleteKeyA",s_ret0,8},{"RegQueryInfoKeyA",s_ret0,48},
    {"LoadLibraryExA",s_LoadLibraryA,12},{"lstrcpynA",s_lstrcpynA,12},{"lstrcmpiA",s_ret0,8},
    {"SysAllocStringLen",s_SysAllocStringLen,8},{"SysAllocString",s_SysAllocStringLen,4},{"SysFreeString",s_SysFreeString,4},
    {"SysReAllocStringLen",s_ret1,12},{"SysStringLen",s_ret0,4},{"VariantInit",s_nop0,4},{"VariantClear",s_ret0,4},
    // --- GDI / screen DC (VCL queries the display at init) ---
    {"GetDC",s_GetDC,4},{"GetWindowDC",s_GetDC,4},{"GetDCEx",s_GetDC,12},{"ReleaseDC",s_ret1,8},
    {"GetDeviceCaps",s_GetDeviceCaps,8},{"GetSystemMetrics",s_GetSystemMetrics,4},
    {"CreatePalette",s_gdi_handle,4},{"CreateCompatibleDC",s_gdi_handle,4},{"CreateCompatibleBitmap",s_gdi_handle,12},
    {"CreateSolidBrush",s_gdi_handle,4},{"CreatePen",s_gdi_handle,12},{"CreateFontIndirectA",s_gdi_handle,4},{"CreateFontIndirectW",s_gdi_handle,4},
    {"CreateBitmap",s_gdi_handle,20},{"CreateDIBSection",s_gdi_handle,24},{"GetStockObject",s_gdi_handle,4},
    {"SelectObject",s_gdi_handle,8},{"SelectPalette",s_gdi_handle,12},{"RealizePalette",s_ret0,4},
    {"DeleteObject",s_ret1,4},{"DeleteDC",s_ret1,4},{"GetObjectA",s_ret0,12},{"GetObjectW",s_ret0,12},
    {"GetSystemPaletteEntries",s_ret0,16},{"GetPaletteEntries",s_ret0,16},{"SetTextColor",s_ret0,8},{"SetBkColor",s_ret0,8},{"SetBkMode",s_ret0,8},
    {"GetTextMetricsA",s_ret1,8},{"GetTextMetricsW",s_ret1,8},{"GetSysColor",s_ret0,4},{"GetSysColorBrush",s_gdi_handle,4},
    // ---- Bulk stdcall argbytes for GUI/util imports (Delphi VCL touches these during init).
    //      Correct argbytes is what matters: returning 0 is fine, but wrong cleanup desyncs the
    //      caller's stack -> corrupted return address. argbytes auto-extracted from SysWOW64 ret-N
    //      (148) + hand-filled ApiSet forwarders (60). GetLocalTime gets a real fill above. ----
    {"GetLocalTime",s_GetLocalTime,4},
    // comctl32 ImageList_*
    {"ImageList_Add",s_ret0,12},{"ImageList_BeginDrag",s_ret0,16},{"ImageList_Create",s_ret0,20},
    {"ImageList_Destroy",s_ret0,4},{"ImageList_DragEnter",s_ret0,12},{"ImageList_DragLeave",s_ret0,4},
    {"ImageList_DragMove",s_ret0,8},{"ImageList_DragShowNolock",s_ret0,4},{"ImageList_Draw",s_ret0,24},
    {"ImageList_DrawEx",s_ret0,40},{"ImageList_EndDrag",s_ret0,0},{"ImageList_GetBkColor",s_ret0,4},
    {"ImageList_GetDragImage",s_ret0,8},{"ImageList_GetIconSize",s_ret0,12},
    {"ImageList_GetImageCount",s_ret0,4},{"ImageList_Read",s_ret0,4},{"ImageList_Remove",s_ret0,8},
    {"ImageList_ReplaceIcon",s_ret0,12},{"ImageList_SetBkColor",s_ret0,8},
    {"ImageList_SetDragCursorImage",s_ret0,16},{"ImageList_SetIconSize",s_ret0,12},{"ImageList_Write",s_ret0,8},
    // gdi32
    {"BitBlt",s_ret0,36},{"CreateBrushIndirect",s_gdi_handle,4},{"CreateDIBitmap",s_gdi_handle,24},
    {"CreatePenIndirect",s_gdi_handle,4},{"GetBitmapBits",s_ret0,12},{"GetClipBox",s_ret0,8},
    {"GetCurrentPositionEx",s_ret0,8},{"GetDIBits",s_ret0,28},{"GetPixel",s_ret0,12},
    {"GetTextExtentPoint32A",s_ret1,16},{"IntersectClipRect",s_ret0,20},{"LineTo",s_ret1,12},
    {"MoveToEx",s_ret1,16},{"PatBlt",s_ret1,24},{"RectVisible",s_ret0,8},{"RestoreDC",s_ret1,8},
    {"SaveDC",s_ret1,4},{"SetPixel",s_ret0,16},{"SetROP2",s_ret0,8},{"SetStretchBltMode",s_ret0,8},
    {"SetViewportOrgEx",s_ret1,16},{"SetWindowOrgEx",s_ret1,16},{"StretchBlt",s_ret0,44},
    {"UnrealizeObject",s_ret1,4},{"SetDIBColorTable",s_ret0,16},{"SetBrushOrgEx",s_ret1,16},
    {"MaskBlt",s_ret0,48},{"GetWindowOrgEx",s_ret1,8},{"GetDIBColorTable",s_ret0,16},
    {"GetDCOrgEx",s_ret1,8},{"GetBrushOrgEx",s_ret1,8},{"ExcludeClipRect",s_ret0,20},
    {"CreateHalftonePalette",s_gdi_handle,4},
    // kernel32
    {"CreateThread",s_ret0,24},{"EnumCalendarInfoA",s_ret0,16},{"FindResourceA",s_ret0,12},
    {"GlobalAddAtomA",s_ret0,4},{"GlobalDeleteAtom",s_ret0,4},{"GlobalFindAtomA",s_ret0,4},
    {"SizeofResource",s_ret0,8},{"SetEndOfFile",s_ret1,4},{"LockResource",s_ret0,4},
    {"LoadResource",s_ret0,8},{"GlobalReAlloc",s_ret0,12},{"GlobalHandle",s_ret0,4},
    {"GetStringTypeExA",s_ret1,20},{"GetFullPathNameA",s_ret0,16},{"GetDiskFreeSpaceA",s_ret1,20},
    {"GetDateFormatA",s_ret0,24},{"FreeResource",s_ret0,4},{"FormatMessageA",s_ret0,28},
    {"CompareStringA",s_CompareStringA,24},
    // version
    {"VerQueryValueA",s_ret0,16},{"GetFileVersionInfoSizeA",s_ret0,8},{"GetFileVersionInfoA",s_ret0,16},
    // oleaut32
    {"SafeArrayGetLBound",s_ret0,12},{"SafeArrayGetUBound",s_ret0,12},{"SafeArrayPtrOfIndex",s_ret0,12},
    {"VariantChangeType",s_ret0,16},{"VariantCopy",s_ret0,8},{"SafeArrayCreate",s_ret0,12},
    // user32
    {"AdjustWindowRectEx",s_ret1,16},{"CallNextHookEx",s_ret0,16},{"CallWindowProcA",s_ret0,20},
    {"CheckMenuItem",s_ret0,12},{"ClientToScreen",s_ret1,8},{"CreateIcon",s_gdi_handle,28},
    {"DefFrameProcA",s_ret0,20},{"DefMDIChildProcA",s_ret0,16},{"DestroyCursor",s_ret1,4},
    {"DestroyIcon",s_ret1,4},{"DispatchMessageA",s_ret0,4},{"DrawFrameControl",s_ret1,16},
    {"DrawIcon",s_ret1,16},{"DrawIconEx",s_ret1,36},{"DrawTextA",s_ret0,20},{"EnableMenuItem",s_ret0,12},
    {"EnableScrollBar",s_ret1,12},{"EnumThreadWindows",s_ret1,12},{"EnumWindows",s_ret1,8},
    {"EqualRect",s_ret0,8},{"FillRect",s_ret1,12},{"FindWindowA",s_ret0,8},{"FrameRect",s_ret1,12},
    {"GetActiveWindow",s_ret0,0},{"GetCapture",s_ret0,0},{"GetClassInfoA",s_ret0,12},
    {"GetClassNameA",s_ret0,12},{"GetCursorPos",s_ret1,4},{"GetDesktopWindow",s_ret0,0},{"GetFocus",s_ret0,0},
    {"GetForegroundWindow",s_ret0,0},{"GetIconInfo",s_ret0,8},{"GetKeyNameTextA",s_ret0,12},
    {"GetKeyState",s_ret0,4},{"GetLastActivePopup",s_ret0,4},{"GetMenu",s_ret0,4},
    {"GetMenuItemCount",s_ret0,4},{"GetMenuItemID",s_ret0,8},{"GetMenuItemInfoA",s_ret0,16},
    {"GetMenuState",s_ret0,12},{"GetMenuStringA",s_ret0,20},{"GetParent",s_ret0,4},{"GetPropA",s_ret0,8},
    {"GetScrollInfo",s_ret0,12},{"GetScrollPos",s_ret0,8},{"GetScrollRange",s_ret1,16},
    {"GetSubMenu",s_ret0,8},{"GetTopWindow",s_ret0,4},{"GetWindow",s_ret0,8},{"GetWindowTextA",s_ret0,12},
    {"GetWindowThreadProcessId",s_ret0,8},{"InflateRect",s_ret1,12},{"InsertMenuA",s_ret1,20},
    {"InsertMenuItemA",s_ret1,16},{"IntersectRect",s_ret0,12},{"IsChild",s_ret0,8},
    {"IsDialogMessageA",s_ret0,8},{"IsIconic",s_ret0,4},{"IsRectEmpty",s_ret0,4},{"IsWindow",s_ret0,4},
    {"IsWindowEnabled",s_ret1,4},{"IsWindowVisible",s_ret0,4},{"IsZoomed",s_ret0,4},{"LoadBitmapA",s_gdi_handle,8},
    {"LoadCursorA",s_gdi_handle,8},{"LoadIconA",s_gdi_handle,8},{"MapVirtualKeyA",s_ret0,8},{"MapWindowPoints",s_ret0,16},
    {"OffsetRect",s_ret1,12},{"PostMessageA",s_ret1,16},{"PtInRect",s_ret0,12},
    {"RegisterClipboardFormatA",s_ret0,4},{"RegisterWindowMessageA",s_ret0,4},{"RemovePropA",s_ret0,8},
    {"ScreenToClient",s_ret1,8},{"ScrollWindow",s_ret1,20},{"SendMessageA",s_ret0,16},
    {"SetClassLongA",s_ret0,12},{"SetMenu",s_ret1,8},{"SetMenuItemInfoA",s_ret1,16},{"SetParent",s_ret0,8},
    {"SetPropA",s_ret1,12},{"SetRect",s_ret1,20},{"SetScrollInfo",s_ret0,16},{"SetScrollPos",s_ret0,16},
    {"SetScrollRange",s_ret1,20},{"SetWindowsHookExA",s_ret0,16},{"ShowScrollBar",s_ret1,12},
    {"SystemParametersInfoA",s_ret1,16},{"TrackPopupMenu",s_ret0,28},{"TranslateMDISysAccel",s_ret0,8},
    {"WindowFromPoint",s_ret0,8},{"WinHelpA",s_ret0,16},{"WaitMessage",s_ret1,0},
    {"UnhookWindowsHookEx",s_ret1,4},{"TranslateMessage",s_ret0,4},{"ShowOwnedPopups",s_ret1,8},
    {"ShowCursor",s_ret0,4},{"SetWindowPlacement",s_ret1,8},{"SetForegroundWindow",s_ret1,4},
    {"SetFocus",s_ret0,4},{"SetCursor",s_ret0,4},{"SetCapture",s_ret0,4},{"SetActiveWindow",s_ret0,4},
    {"RemoveMenu",s_ret1,12},{"ReleaseCapture",s_ret1,0},{"RedrawWindow",s_ret1,16},
    {"PostQuitMessage",s_nop0,4},{"PeekMessageA",s_ret0,20},{"InvalidateRect",s_ret1,12},
    {"GetWindowPlacement",s_ret1,8},{"GetSystemMenu",s_ret0,8},{"GetKeyboardState",s_ret1,4},
    {"GetKeyboardLayoutList",s_ret0,8},{"GetCursor",s_ret0,0},{"EndPaint",s_ret1,8},
    {"EnableWindow",s_ret0,8},{"DrawMenuBar",s_ret1,4},{"DrawEdge",s_ret1,16},{"DestroyMenu",s_ret1,4},
    {"DeleteMenu",s_ret1,12},{"CreatePopupMenu",s_gdi_handle,0},{"CreateMenu",s_gdi_handle,0},
    {"BeginPaint",s_GetDC,8},
    /* ============================================================
     * spfy additions: SWIttsFe-en-US.dll imports not in the donor's
     * VST-oriented base shim list. Implementations live in
     * spfy_extra_shims.c. Argbytes follow Win32 stdcall (KERNEL32/
     * USER32/WINMM) or cdecl=0 (MSVCR71 — caller cleans).
     * ============================================================ */
    {"DisableThreadLibraryCalls", s_DisableThreadLibraryCalls, 4},
    {"SearchPathA",               s_SearchPathA,               24},
    {"timeGetDevCaps",            s_winmm_zero,                8},
    {"timeKillEvent",             s_winmm_zero,                4},
    {"timeSetEvent",              s_winmm_zero,                20},
    /* MSVCR71 (cdecl, argbytes=0) */
    {"_strdup",                   s_strdup,                    0},
    {"_stricmp",                  s_stricmp,                   0},
    {"strchr",                    s_strchr,                    0},
    {"strtol",                    s_strtol,                    0},
    {"strtod",                    s_strtod,                    0},
    {"atof",                      s_atof,                      0},
    {"atoi",                      s_atoi,                      0},
    {"atol",                      s_atol,                      0},
    {"ldiv",                      s_ldiv,                      0},
    {"toupper",                   s_toupper,                   0},
    {"isdigit",                   s_isdigit,                   0},
    {"isspace",                   s_isspace,                   0},
    {"fopen",                     s_fopen,                     0},
    {"fclose",                    s_fclose,                    0},
    {"fread",                     s_fread,                     0},
    {"fwrite",                    s_fwrite,                    0},
    {"fseek",                     s_fseek,                     0},
    {"ftell",                     s_ftell,                     0},
    {"rewind",                    s_rewind,                    0},
    {"fflush",                    s_fflush,                    0},
    {"fputs",                     s_fputs,                     0},
    {"fgets",                     s_fgets,                     0},
    {"getchar",                   s_getchar,                   0},
    {"_fileno",                   s_fileno,                    0},
    {"_stat",                     s_stat,                      0},
    {"_fstat",                    s_fstat,                     0},
    {"_iob",                      s_iob_var,                   0},   /* TODO: variable, not function */
    {"sprintf",                   s_sprintf,                   0},
    {"vsprintf",                  s_vsprintf,                  0},
    {"sscanf",                    s_sscanf,                    0},
    {"vfprintf",                  s_vfprintf,                  0},
    {"fprintf",                   s_fprintf,                   0},
    {"pow",                       s_pow_cdecl,                 0},
    {"log",                       s_log_cdecl,                 0},
    {"exp",                       s_exp_cdecl,                 0},
    {"__CxxFrameHandler",         s_CxxFrameHandler,           0},
    {"_except_handler3",          s_except_handler3,           0},
    {"__CppXcptFilter",           s_CppXcptFilter,             0},
    {"__security_error_handler",  s_security_error_handler,    0},
    {"?terminate@@YAXXZ",         s_terminate_msvc,            0},
    {"_adjust_fdiv",              s_adjust_fdiv,               0},
    {"__dllonexit",               s_dllonexit,                 0},
    {"_onexit",                   s_onexit,                    0},
    {"_setjmp3",                  s_setjmp3,                   0},
    {"longjmp",                   s_longjmp,                   0},
    {"exit",                      s_exit_cdecl,                0},
    {"clock",                     s_clock,                     0},
    /* ============== end spfy additions ============== */
    {0,0,0}
};

// Unregistered imports: return 0, do not halt (cdecl cleanup assumption).
extern int g_cur_imp;
static int g_unimpl_calls = 0;
static void s_unimpl(void){
    if(EMU_VERBOSE && g_unimpl_calls++ < 120) fprintf(stderr,"[unimpl-called] %s\n", g_imp[g_cur_imp>=0?g_cur_imp:0].name);
    ret_set(0);
}

int win32_register_import(const char* dll, const char* name){
    shim_fn fn=s_unimpl; int argb=0; int found=0;
    for(const reg_t* r=REG; r->name; r++) if(!strcmp(r->name,name)){ fn=r->fn; argb=r->argb; found=1; break; }
    if(!found) emu_log("** no shim for import %s!%s (returns 0 if called)\n",dll,name);
    /* Local addition for spfy host_emu: EMU_IATDUMP=1 prints every import slot
     * during pe_load_mem so phase-1 harness can enumerate SWIttsFe's imports
     * without re-walking the PE header. Silent unless explicitly enabled. */
    static int iatdump = -1; if (iatdump < 0) iatdump = getenv("EMU_IATDUMP") ? 1 : 0;
    if (iatdump) fprintf(stderr, "[iat] %-12s ! %-30s %s\n", dll, name, found ? "" : "(unimpl)");
    int idx=g_nimp++;
    if(idx>=MAX_IMPORTS){ fprintf(stderr,"too many imports\n"); exit(1); }
    snprintf(g_imp[idx].name,sizeof g_imp[idx].name,"%s",name);
    g_imp[idx].fn=fn; g_imp[idx].argbytes=argb;
    return IMP_BASE + idx*IMP_STRIDE;
}

void win32_reset(void){   // call before pe_load so per-process state doesn't accumulate across a macro chain
    g_nimp = 0; g_unimpl_calls = 0;
    // TLS/FLS slots: if not reset, the 2nd plugin in a chain gets non-1 slot indices and
    // g_tls[] holds stale pointers into the previous (now freed) plugin's memory -> Delphi's
    // RTL derefs a null/stale thread-data slot and faults (dblue_Crusher load #2).
    g_tlsnext = 1; memset(g_tls, 0, sizeof g_tls); g_lasterr = 0;
    g_tick = 1;   // tick/perf counter must restart each load, else a chained plugin sees a drifting clock
}

void win32_init(void){
    heap_init();
    mem_map(AUX_BASE, 0x1000, "aux");
    // command lines (ANSI + wide)
    g_cmdline_a = AUX_BASE+0x000; const char* cl="plugin"; int i=0; for(;cl[i];i++) wr8(g_cmdline_a+i,cl[i]); wr8(g_cmdline_a+i,0);
    g_cmdline_w = AUX_BASE+0x040; for(i=0;cl[i];i++) wr16(g_cmdline_w+2*i,cl[i]); wr16(g_cmdline_w+2*i,0);
    // environment blocks (double-NUL terminated)
    g_env_a = AUX_BASE+0x100; const char* ev="OS=Windows_NT"; for(i=0;ev[i];i++) wr8(g_env_a+i,ev[i]); wr8(g_env_a+i,0); wr8(g_env_a+i+1,0);
    g_env_w = AUX_BASE+0x200; for(i=0;ev[i];i++) wr16(g_env_w+2*i,ev[i]); wr16(g_env_w+2*i,0); wr16(g_env_w+2*i+2,0);
    // UCRT host data: errno, a dummy FILE table (stdin/out/err), empty narrow environment (char**=NULL)
    g_errno_va = AUX_BASE+0x400; wr32(g_errno_va,0);
    g_iob_va   = AUX_BASE+0x500; for(int k=0;k<0xC0;k+=4) wr32(g_iob_va+k,0);
    g_env_strs = AUX_BASE+0x600; wr32(g_env_strs,0);
}
