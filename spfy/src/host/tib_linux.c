/* Fake Thread Information Block setup for hosting a Windows i386 PE
 * inside a Linux process.
 *
 * The MSVC CRT and most non-trivial Win32 code reads FS:[0x00] (head of
 * the SEH frame chain) and FS:[0x18] (self pointer) during function
 * prologue/epilogue and exception handling. Linux 32-bit processes
 * start with FS = 0; reading fs:[anything] segfaults immediately.
 *
 * Wine solves this by allocating a TIB structure and pointing FS at it
 * via modify_ldt(2). We do the bare minimum: a single fake TIB shared
 * by the (single-threaded) hosted DLL, installed via set_thread_area
 * (the modern GDT-based equivalent). SWIttsFe calls
 * DisableThreadLibraryCalls in DllMain so we don't need per-thread TIBs.
 *
 * The fake TIB only needs to make these reads succeed cleanly:
 *   FS:[0x00]  pointer to a "current SEH frame", whose first DWORD is
 *              0xFFFFFFFF (end-of-chain marker)
 *   FS:[0x18]  self pointer (some MSVC stubs verify TIB.Self == TIB addr)
 *   FS:[0x34]  LastErrorValue — read by GetLastError after a stub call
 *
 * The rest can stay zeroed. */

#include "imports.h"

#ifdef _WIN32
/* On Windows the kernel sets up the real TIB; this file is a no-op. */
int host_tib_setup_if_needed(void) { return 0; }

#else

#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

#if defined(__i386__)
#include <asm/ldt.h>
#include <sys/syscall.h>
#include <unistd.h>

/* End-of-SEH-chain frame: just a sentinel DWORD = 0xFFFFFFFF that the
 * MSVC CRT recognises as "no more handlers; trigger UnhandledException
 * if we get here". The DLL we host never raises an actual Win32
 * exception so this is only consulted by frame setup. */
typedef struct {
    void *next;
    void *handler;
} seh_frame_t;

static seh_frame_t g_seh_end = {
    .next    = (void *)(uintptr_t)0xFFFFFFFF,
    .handler = NULL
};

/* TIB layout — we lay out the fields we touch, leave the rest as zero
 * padding. Total size 4 KB is overkill but cheap (one page). */
typedef struct {
    void    *exception_list;       /* +0x00 */
    void    *stack_base;           /* +0x04 */
    void    *stack_limit;          /* +0x08 */
    void    *sub_system_tib;       /* +0x0C */
    void    *fiber_data_version;   /* +0x10 */
    void    *arbitrary_user_ptr;   /* +0x14 */
    void    *self;                 /* +0x18 */
    /* TEB-only fields begin at +0x1C; we only need a few. */
    uint32_t environment_ptr;      /* +0x1C */
    uint32_t client_id_pid;        /* +0x20 */
    uint32_t client_id_tid;        /* +0x24 */
    uint32_t active_rpc_handle;    /* +0x28 */
    void    *tls_pointer;          /* +0x2C */
    void    *peb;                  /* +0x30 */
    uint32_t last_error;           /* +0x34 */
    uint8_t  pad[4096 - 0x38];
} __attribute__((aligned(16))) fake_tib_t;

static fake_tib_t g_tib;
static int        g_tib_installed = 0;

int host_tib_setup_if_needed(void)
{
    if (g_tib_installed) return 0;

    memset(&g_tib, 0, sizeof g_tib);
    g_tib.exception_list = &g_seh_end;
    g_tib.self           = &g_tib;
    /* Rough stack base/limit — most CRT code doesn't actually check
     * these values, just reads them. Use generous bounds. */
    g_tib.stack_base  = (void *)(uintptr_t)0xFFFFF000;
    g_tib.stack_limit = (void *)(uintptr_t)0x00001000;

    struct user_desc u;
    memset(&u, 0, sizeof u);
    u.entry_number    = (unsigned int)-1;  /* kernel picks a free GDT slot */
    u.base_addr       = (unsigned int)(uintptr_t)&g_tib;
    u.limit           = sizeof g_tib - 1u;
    u.seg_32bit       = 1;
    u.contents        = 0;                  /* data segment, expand-up */
    u.read_exec_only  = 0;
    u.limit_in_pages  = 0;
    u.seg_not_present = 0;
    u.useable         = 1;

    long r = syscall(SYS_set_thread_area, &u);
    if (r < 0) {
        fprintf(stderr, "host_tib_setup: set_thread_area failed: %s\n",
                strerror(errno));
        return -1;
    }
    /* set_thread_area returns the assigned entry_number in u.entry_number.
     * Build a GDT selector: (entry << 3) | RPL_3. TI bit stays 0 (GDT). */
    unsigned short selector =
        (unsigned short)((u.entry_number << 3) | 3u);
    /* Load FS with the new selector. The DLL's code reads FS:[*] from now on. */
    __asm__ volatile("movw %0, %%fs" :: "r"(selector));
    g_tib_installed = 1;
    if (getenv("SPFY_HOST_TRACE")) {
        fprintf(stderr,
                "[host] fake TIB installed: base=%p entry=%u selector=0x%04x\n",
                (void *)&g_tib, u.entry_number, selector);
    }
    return 0;
}

#else /* !__i386__ */

int host_tib_setup_if_needed(void)
{
    /* Hosting a 32-bit Windows PE only makes sense on a 32-bit process. */
    fprintf(stderr, "host_tib_setup: not on i386 — fake TIB unimplemented\n");
    return -1;
}

#endif  /* __i386__ */

#endif  /* !_WIN32 */
