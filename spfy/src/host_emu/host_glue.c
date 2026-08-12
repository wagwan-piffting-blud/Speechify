/* spfy/src/host_emu/host_glue.c — universal host-side helpers needed by
 * cpu.c / loader.c / win32_donor.c. */

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
    CPU.fpu_sw = 0;
    int rc = cpu_run(2000000000ULL);
    if (rc == 0) emu_log("[call_guest] fn=%08x ran out of instructions\n", fn);
    return CPU.r[EAX];
}

/* One-time pickup of $EMU_VERBOSE so the donor TUs see the flag. */
static void __attribute__((constructor)) emu_pickup_verbose(void) {
    const char *v = getenv("EMU_VERBOSE");
    EMU_VERBOSE = (v && *v && *v != '0') ? 1 : 0;
}
