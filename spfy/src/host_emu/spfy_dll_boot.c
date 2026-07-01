/*
 * spfy/src/host_emu/spfy_dll_boot.c — high-level boot wrapper for the
 * embedded SWIttsFe-en-US.dll.
 *
 * Wraps the emulator's mem_init → cpu_reset → pe_load_mem → win32_init →
 * pe_run_dllmain dance behind a single entry point that fe_host.c (and
 * the Phase 1 native test harness) can call without knowing emulator
 * internals.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "spfy_dll_boot.h"
#include "emu.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_booted = 0;

int spfy_dll_emu_is_booted(void) { return g_booted; }

uint32_t spfy_dll_emu_get_export(const char *name) {
    if (!g_booted) {
        fprintf(stderr, "[spfy_dll_emu] get_export(%s) before boot\n", name ? name : "?");
        return 0;
    }
    return pe_get_export(name);
}

uint32_t spfy_dll_emu_call(uint32_t fn, const uint32_t *args, int n) {
    if (!g_booted) {
        fprintf(stderr, "[spfy_dll_emu] call(%#x) before boot\n", fn);
        return 0;
    }
    return call_guest(fn, args, n);
}

uint32_t spfy_dll_emu_alloc(uint32_t n, int zero) {
    if (!g_booted) {
        fprintf(stderr, "[spfy_dll_emu] alloc(%u) before boot\n", n);
        return 0;
    }
    return guest_alloc(n, zero);
}

void spfy_dll_emu_read(uint32_t guest_va, void *host_dst, uint32_t n) {
    mem_read(guest_va, host_dst, n);
}
void spfy_dll_emu_write(uint32_t guest_va, const void *host_src, uint32_t n) {
    mem_write(guest_va, host_src, n);
}

int spfy_dll_emu_boot(const uint8_t *dll_bytes, uint32_t dll_len) {
    if (g_booted) return 0;       /* idempotent */
    if (!dll_bytes || dll_len < 0x40) {
        fprintf(stderr, "[spfy_dll_emu] boot: bad blob (ptr=%p len=%u)\n",
                (const void *)dll_bytes, dll_len);
        return -1;
    }

    mem_init();
    cpu_reset();

    /* Reset import table BEFORE pe_load_mem because pe_load_mem walks
     * the import directory and calls win32_register_import per name —
     * which appends into g_imp[]. A stale table from a previous boot
     * would shift the import indices and corrupt IAT patching. */
    win32_reset();

    if (pe_load_mem(dll_bytes, dll_len) != 0) {
        fprintf(stderr, "[spfy_dll_emu] boot: pe_load_mem failed\n");
        return -1;
    }

    win32_init();
    pe_init_tls();
    pe_run_dllmain();

    if (CPU.faulted) {
        fprintf(stderr, "[spfy_dll_emu] boot: DllMain FAULTED  eip=%#x  msg=%s  addr=%#x\n",
                CPU.eip,
                CPU.fault_msg ? CPU.fault_msg : "(none)",
                CPU.fault_addr);
        return -1;
    }

    g_booted = 1;
    if (getenv("SPFY_EMU_VERBOSE")) {
        fprintf(stderr, "[spfy_dll_emu] boot: OK  image_base=%#x  entry_rva=%#x  size=%u\n",
                PE.image_base, PE.entry_rva, PE.size_of_image);
    }
    return 0;
}
