/*
 * spfy/src/host_emu/host_glue.c — universal host-side helpers needed by
 * cpu.c / loader.c / win32_donor.c. The donor expects these to live in
 * its harness TU (vst_host.c / ladspa_host.c); spfy is neither VST nor
 * LADSPA, so this is the minimal extract:
 *
 *   - EMU_VERBOSE: env-gated log flag (set once from $EMU_VERBOSE).
 *   - emu_log(fmt, ...): vfprintf wrapper, no-op unless EMU_VERBOSE.
 *   - call_guest(fn, args, n): the cdecl/stdcall trampoline. Pushes
 *     args right-to-left, pushes RET_SENTINEL, sets EIP=fn, runs the
 *     CPU until ret-to-sentinel or fault. Returns EAX.
 *
 * Lifted verbatim from _emu/vst_host.c lines 11–34 (no behavioural
 * change) so future fixes there can be cherry-picked unchanged.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "emu.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

int EMU_VERBOSE = 0;

void emu_log(const char *fmt, ...) {
    if (!EMU_VERBOSE) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

uint32_t call_guest(uint32_t fn, const uint32_t *args, int n) {
    CPU.r[ESP] = STACK_ESP0;
    for (int i = n - 1; i >= 0; i--) cpu_push32(args[i]);
    cpu_push32(RET_SENTINEL);
    CPU.eip = fn;
    CPU.halted = 0;
    CPU.faulted = 0;
    CPU.fpu_top = 8;
    CPU.fpu_sw = 0;                                    /* FPU stack empty at call boundary */
    int rc = cpu_run(2000000000ULL);
    if (rc == 0) emu_log("[call_guest] fn=%08x ran out of instructions\n", fn);
    return CPU.r[EAX];
}

/* One-time pickup of $EMU_VERBOSE so the donor TUs see the flag. The
 * donor's vst_host.c relied on the harness `main()` to set EMU_VERBOSE
 * after argv parsing; we don't have that path, so do it lazily on the
 * first emu_log call site instead. Note: cheap branch; no race because
 * spfy is single-threaded at the FE level. */
static void __attribute__((constructor)) emu_pickup_verbose(void) {
    const char *v = getenv("EMU_VERBOSE");
    EMU_VERBOSE = (v && *v && *v != '0') ? 1 : 0;
}
