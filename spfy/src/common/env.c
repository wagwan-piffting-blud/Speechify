#include "env.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>

/* ⚠ NOT getenv() ON WINDOWS.
 *
 * The CRT builds `_environ` from a SNAPSHOT taken when the HOST PROCESS
 * started, and never re-reads the OS environment block. In an .exe that is
 * invisible -- the snapshot is current. Inside a DLL loaded by somebody
 * else's program it is not: a host that sets a variable at runtime (which is
 * what every GUI app does, PowerShell's `$env:X=` included) updates the OS
 * block, and getenv() carries on returning the value from process start.
 *
 * Measured: with SPFY_4_MODE=1 exported by the host before loading the voice,
 * the SAPI DLL produced audio BYTE-IDENTICAL to plain -- it never saw the
 * variable. SPFY_SAPI_DEBUG was equally invisible, so the one diagnostic that
 * exists for SAPI hosts could not be switched on either.
 *
 * GetEnvironmentVariableA reads the live block, so the DLL now sees what the
 * host actually set. The matching writer is SetEnvironmentVariableA in
 * spfy4_env_default()/_restore() -- _putenv() only touches the CRT copy, so
 * mixing the two APIs would put values somewhere the reader cannot see. */
static const char *win_env(const char *name)
{
    /* Values are cached by the table below, so this buffer only has to
     * survive until the caller copies the pointer into it. */
    static char arena[64 * 1024];
    static size_t used = 0;
    DWORD n = GetEnvironmentVariableA(name, NULL, 0);
    if (n == 0) return NULL;
    if (used + n > sizeof arena) return NULL;
    char *dst = arena + used;
    DWORD got = GetEnvironmentVariableA(name, dst, n);
    if (got == 0 || got >= n) return NULL;
    used += (size_t)got + 1u;
    return dst;
}
#define SPFY_RAW_ENV(n) win_env(n)
#else
#define SPFY_RAW_ENV(n) getenv(n)
#endif

/* Power of two so the wrap is a mask. */
#define ENV_CACHE_N 256u

static const char *g_key[ENV_CACHE_N];
static const char *g_val[ENV_CACHE_N];

const char *spfy_env(const char *name)
{
    if (!name) return NULL;
    /* >>4 because string literals cluster on 4/8/16-byte boundaries, so the
     * low bits carry almost no entropy and every key would land in the same
     * few buckets. */
    uintptr_t h = ((uintptr_t)name >> 4) ^ ((uintptr_t)name >> 12);
    for (unsigned i = 0; i < ENV_CACHE_N; ++i) {
        unsigned j = (unsigned)((h + i) & (ENV_CACHE_N - 1u));
        if (g_key[j] == name) return g_val[j];
        if (g_key[j] == NULL) {
            /* Store the value FIRST: a concurrent reader that sees the key
             * published must not be able to read an unwritten value. */
            g_val[j] = SPFY_RAW_ENV(name);
            g_key[j] = name;
            return g_val[j];
        }
    }
    return SPFY_RAW_ENV(name);
}

int spfy_env_set(const char *name)
{
    return spfy_env(name) != NULL;
}

/* ⚠ stderr IS UNBUFFERED BY STANDARD, so a dump in a DP inner loop costs one
 * write() PER LINE. SPFY_JOIN_DUMP fires once per EDGE CONSIDERED -- not once
 * per join on the chosen path -- so on a 280k-unit voice it is millions of
 * syscalls per sentence, and a 20-line probe run hit a 600 s timeout with the
 * audio already written. The volume is inherent to a per-edge dump; the
 * syscall per line is not.
 *
 * Give the value a PATH and the stream is a fully-buffered file (the same
 * shape SPFY_PSOLA_GRAIN_DUMP already used). "1"/"-"/"stderr" keep the old
 * behaviour, so every existing invocation still works.
 *
 * Cached by the key POINTER, exactly like spfy_env above, so a hot-path caller
 * pays one load and a compare. */
#define DUMP_CACHE_N 16u
#define DUMP_BUF_N   (1u << 20)

static const char *g_dkey[DUMP_CACHE_N];
static FILE       *g_dfp [DUMP_CACHE_N];

FILE *spfy_dump_stream(const char *name)
{
    if (!name) return NULL;
    for (unsigned i = 0; i < DUMP_CACHE_N; ++i) {
        if (g_dkey[i] == name) return g_dfp[i];
        if (g_dkey[i] == NULL) {
            const char *v = spfy_env(name);
            FILE *fp = NULL;
            if (v && *v) {
                if (v[0] == '-' || !strcmp(v, "1") || !strcmp(v, "stderr")) {
                    fp = stderr;
                } else {
                    fp = fopen(v, "w");
                    if (fp) {
                        char *b = (char *)malloc(DUMP_BUF_N);
                        /* Leaked deliberately: it must outlive every write,
                         * and the process flushes these streams at exit. */
                        if (b) setvbuf(fp, b, _IOFBF, DUMP_BUF_N);
                    }
                }
            }
            g_dfp[i] = fp;
            g_dkey[i] = name;
            return fp;
        }
    }
    return NULL;
}

void spfy_env_reset(void)
{
    for (unsigned i = 0; i < ENV_CACHE_N; ++i) {
        g_key[i] = NULL;
        g_val[i] = NULL;
    }
    for (unsigned i = 0; i < DUMP_CACHE_N; ++i) {
        if (g_dfp[i] && g_dfp[i] != stderr) fclose(g_dfp[i]);
        g_dkey[i] = NULL;
        g_dfp[i] = NULL;
    }
}
