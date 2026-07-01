/*
 * spfy/src/host_emu/spfy_extra_shims.c — Win32/MSVCR71 import shims that
 * SWIttsFe-en-US.dll needs but the donor's win32_donor.c doesn't carry
 * (because the donor targets VST plugins, not a 2003-era TTS engine
 * statically linked against MSVCR71).
 *
 * Phase 1 sourcing: the IAT enumeration via EMU_IATDUMP=1 against
 * SWIttsFe-en-US.dll surfaced ~50 unshimmed names. Each gets the
 * minimum-viable implementation here. The shims are registered by
 * appending to REG[] in win32_donor.c (one localized block, see the
 * "spfy additions" marker there).
 *
 * Calling convention reminder:
 *   - cdecl   -> argbytes = 0 (caller cleans). Used by MSVCR71 entries.
 *   - stdcall -> argbytes = sizeof(args). Used by KERNEL32/USER32/WINMM.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "emu.h"

#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- helpers ---- */

static void guest_strn_in(uint32_t src, char *out, int maxlen) {
    int i = 0;
    for (; i < maxlen - 1; i++) {
        char c = (char)rd8(src + i);
        out[i] = c;
        if (!c) break;
    }
    out[i < maxlen ? i : maxlen - 1] = 0;
}

static void guest_str_out(uint32_t dst, const char *s) {
    int i = 0;
    for (; s[i]; i++) wr8(dst + i, (uint8_t)s[i]);
    wr8(dst + i, 0);
}

static int g_clock_tick = 0;

/* ---- KERNEL32 ---- */

void s_DisableThreadLibraryCalls(void) {
    /* HMODULE arg ignored. Return TRUE = thread attach/detach won't fire,
     * which matches our single-thread emulator anyway. */
    ret_set(1);
}

void s_SearchPathA(void) {
    /* DWORD SearchPathA(lpPath, lpFileName, lpExtension, nBufferLength, lpBuffer, lpFilePart)
     * Return 0 = file not found. The guest's SWIttsFe init code calls
     * this to look for resource files outside its own directory; failing
     * here just makes it skip the optional lookup and use a default. */
    ret_set(0);
}

/* ---- USER32 / WINMM stubs ---- */

void s_winmm_zero(void) { ret_set(0); }   /* timeGetDevCaps / timeKillEvent / timeSetEvent */

/* ---- MSVCR71 — string / mem ---- */

void s_strdup(void) {
    uint32_t src = arg32(0);
    char buf[1024];
    guest_strn_in(src, buf, (int)sizeof buf);
    uint32_t len = (uint32_t)strlen(buf) + 1;
    uint32_t p = guest_alloc(len, 0);
    if (!p) { ret_set(0); return; }
    for (uint32_t i = 0; i < len; i++) wr8(p + i, (uint8_t)buf[i]);
    ret_set(p);
}

void s_stricmp(void) {
    uint32_t a = arg32(0), b = arg32(1);
    int ca, cb, i = 0;
    for (;;) {
        ca = tolower(rd8(a + i));
        cb = tolower(rd8(b + i));
        if (ca != cb || !ca) break;
        i++;
    }
    ret_set((uint32_t)(ca - cb));
}

void s_strchr(void) {
    uint32_t s = arg32(0);
    int c = (int)(arg32(1) & 0xff);
    int i = 0;
    while (1) {
        int ch = rd8(s + i);
        if (ch == c) { ret_set(s + i); return; }
        if (!ch) break;
        i++;
    }
    ret_set(0);
}

void s_strtol(void) {
    /* long strtol(const char *str, char **endptr, int base) */
    uint32_t s = arg32(0), endp = arg32(1);
    int base = (int)arg32(2);
    char buf[64];
    guest_strn_in(s, buf, (int)sizeof buf);
    char *end_host = NULL;
    long v = strtol(buf, &end_host, base);
    if (endp) wr32(endp, s + (uint32_t)(end_host - buf));
    ret_set((uint32_t)v);
}

void s_strtod(void) {
    /* double strtod(const char *str, char **endptr) — returns in ST(0) */
    uint32_t s = arg32(0), endp = arg32(1);
    char buf[64];
    guest_strn_in(s, buf, (int)sizeof buf);
    char *end_host = NULL;
    double v = strtod(buf, &end_host);
    if (endp) wr32(endp, s + (uint32_t)(end_host - buf));
    /* x87 ABI: push result on ST(0). MSVC dwords are returned in EAX:EDX,
     * but doubles are returned in ST(0). */
    CPU.fpu_top = (CPU.fpu_top - 1) & 7;
    CPU.st[CPU.fpu_top] = v;
}

void s_atof(void) {
    uint32_t s = arg32(0);
    char buf[64];
    guest_strn_in(s, buf, (int)sizeof buf);
    double v = atof(buf);
    CPU.fpu_top = (CPU.fpu_top - 1) & 7;
    CPU.st[CPU.fpu_top] = v;
}

void s_atoi(void) {
    uint32_t s = arg32(0);
    char buf[64];
    guest_strn_in(s, buf, (int)sizeof buf);
    ret_set((uint32_t)atoi(buf));
}

void s_atol(void) { s_atoi(); }   /* same ABI: long == int on 32-bit Windows */

void s_ldiv(void) {
    /* ldiv_t ldiv(long num, long denom) — returns the 8-byte struct in EDX:EAX
     * (quotient in EAX, remainder in EDX). */
    long num = (long)(int32_t)arg32(0);
    long den = (long)(int32_t)arg32(1);
    if (den == 0) { CPU.r[EAX] = 0; CPU.r[EDX] = 0; return; }
    CPU.r[EAX] = (uint32_t)(num / den);
    CPU.r[EDX] = (uint32_t)(num % den);
}

void s_toupper(void) { ret_set((uint32_t)toupper((int)(arg32(0) & 0xff))); }
void s_isdigit(void) { ret_set(isdigit((int)(arg32(0) & 0xff)) ? 1 : 0); }
void s_isspace(void) { ret_set(isspace((int)(arg32(0) & 0xff)) ? 1 : 0); }

/* ---- MSVCR71 — stdio (best-effort: most calls happen during init,
 *      where reading missing resource files just degrades to defaults) ---- */

void s_fopen(void)   { ret_set(0); }              /* NULL FILE* */
void s_fclose(void)  { ret_set(0); }
void s_fread(void)   { ret_set(0); }              /* 0 items read */
void s_fwrite(void)  { ret_set(arg32(2)); }       /* pretend all written */
void s_fseek(void)   { ret_set(0); }
void s_ftell(void)   { ret_set(0); }
void s_rewind(void)  { /* void */ }
void s_fflush(void)  { ret_set(0); }
void s_fputs(void)   { ret_set(0); }
void s_fgets(void)   { ret_set(0); }
void s_getchar(void) { ret_set((uint32_t)-1); }   /* EOF */
void s_fileno(void)  { ret_set((uint32_t)-1); }
void s_stat(void)    { ret_set((uint32_t)-1); }   /* "no such file" */
void s_fstat(void)   { ret_set((uint32_t)-1); }

/* The _iob symbol is a *variable* in real MSVCR71: an array of three
 * FILE structs (stdin/stdout/stderr). The import dispatch can't return
 * a variable address through ret_set + ret_imm pop because the guest
 * loads it via `mov reg,[__imp__iob]`. The donor handles this for
 * __acrt_iob_func; for the variable-style _iob, the IAT slot has to
 * point at a real memory region rather than a function. For now we
 * point it at a static 64-byte zero region; any code that does
 * fprintf(stderr, ...) just gets nothing useful, but won't crash.
 *
 * NOTE: registering _iob as a "function" shim is wrong; the guest will
 * *read* the IAT slot directly. The proper fix is to allocate a guest
 * region and patch the IAT to point there post-load. Track as Phase-2
 * follow-up; the no-op function is harmless for boot. */
void s_iob_var(void) { ret_set(0); }

void s_vsprintf(void) {
    /* int vsprintf(char *buf, const char *fmt, va_list ap) */
    uint32_t bufp = arg32(0), fmtp = arg32(1), apv = arg32(2);
    char fmt[512]; guest_strn_in(fmtp, fmt, (int)sizeof fmt);
    /* Minimal printf: only "%s" / "%d" / "%u" / "%x" / "%c" / "%%" handled.
     * Full printf is a Phase-2 niceness; for boot we just need *something*. */
    char out[1024]; int oi = 0;
    int ap_off = 0;
    for (const char *p = fmt; *p && oi < (int)sizeof out - 2; p++) {
        if (*p != '%') { out[oi++] = *p; continue; }
        p++;
        if (!*p) break;
        if (*p == '%') { out[oi++] = '%'; continue; }
        uint32_t arg = rd32(apv + ap_off);
        ap_off += 4;
        char tmp[64];
        switch (*p) {
            case 's': {
                char s[256]; guest_strn_in(arg, s, (int)sizeof s);
                for (int i = 0; s[i] && oi < (int)sizeof out - 2; i++) out[oi++] = s[i];
                break;
            }
            case 'd': case 'i': {
                snprintf(tmp, sizeof tmp, "%d", (int32_t)arg);
                for (int i = 0; tmp[i] && oi < (int)sizeof out - 2; i++) out[oi++] = tmp[i];
                break;
            }
            case 'u': {
                snprintf(tmp, sizeof tmp, "%u", arg);
                for (int i = 0; tmp[i] && oi < (int)sizeof out - 2; i++) out[oi++] = tmp[i];
                break;
            }
            case 'x': case 'X': {
                snprintf(tmp, sizeof tmp, *p == 'X' ? "%X" : "%x", arg);
                for (int i = 0; tmp[i] && oi < (int)sizeof out - 2; i++) out[oi++] = tmp[i];
                break;
            }
            case 'c': out[oi++] = (char)arg; break;
            default:  out[oi++] = '?'; break;
        }
    }
    out[oi] = 0;
    guest_str_out(bufp, out);
    ret_set((uint32_t)oi);
}

void s_sprintf(void) {
    /* sprintf(buf, fmt, ...) — variadic. Treat the variadic stack as
     * a flat array starting at [arg2..] and forward to vsprintf logic. */
    uint32_t bufp = arg32(0), fmtp = arg32(1);
    /* Synthesize ap pointing at the first variadic arg slot. */
    char fmt[512]; guest_strn_in(fmtp, fmt, (int)sizeof fmt);
    char out[1024]; int oi = 0;
    int arg_idx = 2;
    for (const char *p = fmt; *p && oi < (int)sizeof out - 2; p++) {
        if (*p != '%') { out[oi++] = *p; continue; }
        p++; if (!*p) break;
        if (*p == '%') { out[oi++] = '%'; continue; }
        uint32_t a = arg32(arg_idx++);
        char tmp[64];
        switch (*p) {
            case 's': {
                char s[256]; guest_strn_in(a, s, (int)sizeof s);
                for (int i = 0; s[i] && oi < (int)sizeof out - 2; i++) out[oi++] = s[i];
                break;
            }
            case 'd': case 'i':
                snprintf(tmp, sizeof tmp, "%d", (int32_t)a);
                for (int i = 0; tmp[i] && oi < (int)sizeof out - 2; i++) out[oi++] = tmp[i];
                break;
            case 'u':
                snprintf(tmp, sizeof tmp, "%u", a);
                for (int i = 0; tmp[i] && oi < (int)sizeof out - 2; i++) out[oi++] = tmp[i];
                break;
            case 'x': case 'X':
                snprintf(tmp, sizeof tmp, *p == 'X' ? "%X" : "%x", a);
                for (int i = 0; tmp[i] && oi < (int)sizeof out - 2; i++) out[oi++] = tmp[i];
                break;
            case 'c': out[oi++] = (char)a; break;
            default:  out[oi++] = '?'; break;
        }
    }
    out[oi] = 0;
    guest_str_out(bufp, out);
    ret_set((uint32_t)oi);
}

void s_sscanf(void)  { ret_set(0); }            /* 0 fields parsed */
void s_vfprintf(void){ ret_set(0); }
void s_fprintf(void) { ret_set(0); }

/* ---- MSVCR71 — math (cdecl form, takes args on the stack as doubles
 *      and returns in ST(0)). The donor's s_libm_pow / s_libm_log /
 *      s_libm_exp use the SSE2 ABI (XMM regs). MSVCR71's pow/log/exp
 *      take their args on the cdecl stack and return in ST(0). ---- */

static double cdecl_pop_double(int slot) {
    uint64_t lo = rd32(CPU.r[ESP] + 4 + slot * 4);
    uint64_t hi = rd32(CPU.r[ESP] + 4 + slot * 4 + 4);
    uint64_t bits = lo | (hi << 32);
    double d;
    memcpy(&d, &bits, 8);
    return d;
}

static void cdecl_ret_double(double v) {
    CPU.fpu_top = (CPU.fpu_top - 1) & 7;
    CPU.st[CPU.fpu_top] = v;
}

void s_pow_cdecl(void) { cdecl_ret_double(pow(cdecl_pop_double(0), cdecl_pop_double(2))); }
void s_log_cdecl(void) { cdecl_ret_double(log(cdecl_pop_double(0))); }
void s_exp_cdecl(void) { cdecl_ret_double(exp(cdecl_pop_double(0))); }

/* ---- MSVCR71 — SEH / CRT misc ---- */

/* s_CxxFrameHandler is provided by win32_donor.c (returns 1 =
 * ExceptionContinueSearch). We re-use it for the unnumbered
 * `__CxxFrameHandler` MSVCR71 entry. */
void s_except_handler3(void)         { ret_set(0); }   /* ditto */
void s_CppXcptFilter(void)           { ret_set(0); }   /* EXCEPTION_CONTINUE_SEARCH */
void s_security_error_handler(void)  { /* void */ }
void s_terminate_msvc(void)          { CPU.halted = 1; CPU.faulted = 1; CPU.fault_msg = "?terminate@@YAXXZ"; }
void s_adjust_fdiv(void)             { ret_set(0); }
void s_dllonexit(void)               { ret_set(arg32(0)); }   /* return the registration arg unchanged */
void s_onexit(void)                  { ret_set(arg32(0)); }
void s_setjmp3(void)                 { ret_set(0); }   /* 0 = first call */
void s_longjmp(void)                 { CPU.halted = 1; CPU.faulted = 1; CPU.fault_msg = "longjmp not implemented"; }
void s_exit_cdecl(void)              { CPU.halted = 1; if (getenv("SPFY_EMU_VERBOSE")) fprintf(stderr, "[shim] exit(%u) called\n", arg32(0)); }
void s_clock(void)                   { ret_set((uint32_t)(g_clock_tick += 1000)); }
