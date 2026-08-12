/*
 * spfy/src/host_emu/spfy_dll_boot.h — public surface for the emulator-
 * backed SWIttsFe-en-US.dll host.
 *
 * Call order in a fresh process:
 *   spfy_dll_emu_boot(bytes, len)          // mem_init/cpu_reset/load/DllMain
 *   fn = spfy_dll_emu_get_export("name");  // resolve guest VA by export name
 *   spfy_dll_emu_call(fn, args, n);        // drive CPU until it returns
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef SPFY_DLL_BOOT_H
#define SPFY_DLL_BOOT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Boot the emulator and load the embedded SWIttsFe DLL bytes. Runs
 * DllMain(DLL_PROCESS_ATTACH). Returns 0 on success, -1 if anything
 * faults or the bytes don't parse as a PE32. A second call with the SAME
 * bytes is a no-op (returns 0). A call with DIFFERENT bytes re-maps the
 * guest from scratch, which is how a language switch works -- and which
 * invalidates every guest VA handed out so far, so release those first. */
int      spfy_dll_emu_boot(const uint8_t *dll_bytes, uint32_t dll_len);

int      spfy_dll_emu_is_booted(void);

/* Resolve an export by name. */
uint32_t spfy_dll_emu_get_export(const char *name);

/* Call a guest function (cdecl/stdcall: args go on the stack low-to-high). */
uint32_t spfy_dll_emu_call(uint32_t fn, const uint32_t *args, int n);

/* Allocate `n` bytes in the guest heap. */
uint32_t spfy_dll_emu_alloc(uint32_t n, int zero);

void     spfy_dll_emu_read(uint32_t guest_va, void *host_dst, uint32_t n);
void     spfy_dll_emu_write(uint32_t guest_va, const void *host_src, uint32_t n);

#ifdef __cplusplus
}
#endif

#endif
