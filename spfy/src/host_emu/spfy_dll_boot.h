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
 * faults or the bytes don't parse as a PE32. Idempotent: a second call
 * with the same bytes is a no-op (returns 0). */
int      spfy_dll_emu_boot(const uint8_t *dll_bytes, uint32_t dll_len);

/* True if boot has succeeded. */
int      spfy_dll_emu_is_booted(void);

/* Resolve an export by name. Returns 0 if the export isn't found. The
 * value is a guest VA (image-base + RVA), suitable for spfy_dll_emu_call. */
uint32_t spfy_dll_emu_get_export(const char *name);

/* Call a guest function (cdecl/stdcall: args go on the stack low-to-high).
 * Returns guest EAX. */
uint32_t spfy_dll_emu_call(uint32_t fn, const uint32_t *args, int n);

/* Allocate `n` bytes in the guest heap. Returns a guest VA, 0 on OOM.
 * Useful for handing buffers to the DLL (text input, voice file path,
 * etc.). */
uint32_t spfy_dll_emu_alloc(uint32_t n, int zero);

/* Read/write the guest's address space directly. */
void     spfy_dll_emu_read(uint32_t guest_va, void *host_dst, uint32_t n);
void     spfy_dll_emu_write(uint32_t guest_va, const void *host_src, uint32_t n);

#ifdef __cplusplus
}
#endif

#endif /* SPFY_DLL_BOOT_H */
