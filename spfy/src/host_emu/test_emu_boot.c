/*
 * spfy/src/host_emu/test_emu_boot.c — Phase 1 native smoke test.
 *
 * Reads SWIttsFe-en-US.dll from disk, runs it through the emulator's
 * DllMain. Reports whether the load succeeds, where it faults if it
 * does, and resolves the lone export (`getObject`) so we know we can
 * call into the DLL afterward.
 *
 * Build: linked by spfy/src/host_emu/CMakeLists.txt as a host-only
 * executable. Run from PowerShell:
 *
 *   $env:EMU_IATDUMP="1"; $env:EMU_VERBOSE="1"
 *   ./test_emu_boot.exe "Speechify/bin/SWIttsFe-en-US.dll"
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "spfy_dll_boot.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int read_file(const char *path, unsigned char **out_buf, long *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "fopen('%s'): can't open\n", path); return -1; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); fprintf(stderr, "empty file\n"); return -1; }
    unsigned char *buf = (unsigned char *)malloc((size_t)n);
    if (!buf) { fclose(f); return -1; }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
        fclose(f); free(buf); fprintf(stderr, "short read\n"); return -1;
    }
    fclose(f);
    *out_buf = buf;
    *out_len = n;
    return 0;
}

int main(int argc, char **argv) {
    const char *path = (argc > 1)
        ? argv[1]
        : "Speechify/bin/SWIttsFe-en-US.dll";

    fprintf(stderr, "[test] loading %s\n", path);
    unsigned char *bytes = NULL;
    long len = 0;
    if (read_file(path, &bytes, &len) != 0) return 1;
    fprintf(stderr, "[test] read %ld bytes\n", len);

    if (spfy_dll_emu_boot(bytes, (uint32_t)len) != 0) {
        fprintf(stderr, "[test] BOOT FAILED — see emulator output above\n");
        free(bytes);
        return 2;
    }
    free(bytes);

    fprintf(stderr, "[test] DllMain returned cleanly.\n");

    /* Resolve the single export the spfy host already uses. */
    uint32_t getObject_va = spfy_dll_emu_get_export("getObject");
    if (!getObject_va) {
        fprintf(stderr, "[test] WARN: getObject export not found — the FE host won't work\n");
        return 3;
    }
    fprintf(stderr, "[test] getObject -> guest VA %#x\n", getObject_va);

    /* Phase 2 validation: drive the engine through getObject + initStage1
     * via the emulator. This proves the full call path (export →
     * guest C function → vtable → vtable method → ret) works. */

    /* Allocate guest space for the out parameter. */
    uint32_t out_va = spfy_dll_emu_alloc(4, 1);
    if (!out_va) { fprintf(stderr, "[test] guest_alloc failed\n"); return 4; }

    /* Call getObject(kind=2, &raw) — cdecl, 2 args. Returns nonzero on
     * success, with *out filled in with the FE iobj pointer. */
    uint32_t args[2] = { 2, out_va };
    uint32_t rc = spfy_dll_emu_call(getObject_va, args, 2);
    uint32_t iobj_va;
    spfy_dll_emu_read(out_va, &iobj_va, 4);
    fprintf(stderr, "[test] getObject(2, &out) -> rc=%u  iobj_va=%#x\n", rc, iobj_va);
    if (!rc || !iobj_va) { fprintf(stderr, "[test] getObject FAILED\n"); return 5; }

    /* Read the iobj header: vtable[+0], refcount[+4], state[+8],
     * init_flag[+0xc], err_flag[+0xd]. */
    uint32_t vtable_va;
    spfy_dll_emu_read(iobj_va + 0, &vtable_va, 4);
    fprintf(stderr, "[test] iobj.vtable -> %#x\n", vtable_va);

    /* Vtable slot 3 = initStage1. Read the function pointer at vtable+12. */
    uint32_t initStage1_va;
    spfy_dll_emu_read(vtable_va + 3 * 4, &initStage1_va, 4);
    fprintf(stderr, "[test] vtable[3] (initStage1) -> %#x\n", initStage1_va);

    /* Call initStage1(self). __stdcall(this, ...) — but the call wrapper
     * pushes args in cdecl order; for thiscall under MSVC, `this` is in
     * ECX. Per the donor's ABI conventions, for now we approximate by
     * passing `this` as the first stack arg (Windows __stdcall thiscall
     * on x86 vtables) — the DLL was compiled this way in 2003. */
    uint32_t this_args[1] = { iobj_va };
    uint32_t r3 = spfy_dll_emu_call(initStage1_va, this_args, 1);
    fprintf(stderr, "[test] initStage1(self) -> %#x\n", r3);

    /* Read err_flag back from the iobj to see if init succeeded. */
    uint8_t err_flag;
    spfy_dll_emu_read(iobj_va + 0xd, &err_flag, 1);
    fprintf(stderr, "[test] iobj.err_flag = %u  (0 = OK)\n", err_flag);

    if (err_flag) {
        fprintf(stderr, "[test] initStage1 set err_flag -- engine refused init\n");
        return 6;
    }

    /* feedConfigA(text) — slot 5. Send a short input string. The
     * text must live in guest memory; spfy_dll_emu_alloc + write. */
    const char *input = "Hello world.";
    uint32_t input_len = (uint32_t)strlen(input) + 1;
    uint32_t input_va = spfy_dll_emu_alloc(input_len, 0);
    spfy_dll_emu_write(input_va, input, input_len);

    uint32_t feedConfigA_va;
    spfy_dll_emu_read(vtable_va + 5 * 4, &feedConfigA_va, 4);
    fprintf(stderr, "[test] vtable[5] (feedConfigA) -> %#x\n", feedConfigA_va);

    uint32_t fcA_args[2] = { iobj_va, input_va };
    uint32_t r5 = spfy_dll_emu_call(feedConfigA_va, fcA_args, 2);
    spfy_dll_emu_read(iobj_va + 0xd, &err_flag, 1);
    fprintf(stderr, "[test] feedConfigA(self, \"%s\") -> %#x  err_flag=%u\n",
            input, r5, err_flag);

    /* feedConfigB(self, &empty_cfg) — slot 6. The native fe_host.c passes
     * a pointer to a single NUL byte; we replicate. */
    uint32_t feedConfigB_va;
    spfy_dll_emu_read(vtable_va + 6 * 4, &feedConfigB_va, 4);
    uint32_t empty_va = spfy_dll_emu_alloc(1, 1);   /* 1 byte zero */
    uint32_t fcB_args[2] = { iobj_va, empty_va };
    uint32_t r6 = spfy_dll_emu_call(feedConfigB_va, fcB_args, 2);
    spfy_dll_emu_read(iobj_va + 0xd, &err_flag, 1);
    fprintf(stderr, "[test] feedConfigB(self, &empty) -> %#x  err_flag=%u\n", r6, err_flag);

    /* Drain delegateB (slot 42). The native code calls fn42 in a loop
     * until *out_len <= 1. Each call writes up to 256 bytes into buf
     * and sets out_len to bytes_copied+1. */
    uint32_t delegateB_va;
    spfy_dll_emu_read(vtable_va + 42 * 4, &delegateB_va, 4);

    uint32_t buf_va = spfy_dll_emu_alloc(256, 0);
    uint32_t outlen_va = spfy_dll_emu_alloc(4, 0);
    size_t total = 0;
    char *tagged = malloc(8192);
    size_t tagged_cap = 8192;
    if (!tagged) { fprintf(stderr, "[test] OOM\n"); return 7; }

    for (int safety = 0; safety < 4096; safety++) {
        spfy_dll_emu_write(outlen_va, "\0\0\0\0", 4);   /* clear */
        uint32_t dB_args[4] = { iobj_va, buf_va, 256, outlen_va };
        spfy_dll_emu_call(delegateB_va, dB_args, 4);
        uint32_t out_len;
        spfy_dll_emu_read(outlen_va, &out_len, 4);
        if (out_len <= 1) break;
        uint32_t copied = out_len - 1;
        if (copied > 256) copied = 256;
        if (total + copied + 1 > tagged_cap) {
            while (total + copied + 1 > tagged_cap) tagged_cap *= 2;
            char *p = realloc(tagged, tagged_cap);
            if (!p) { free(tagged); return 7; }
            tagged = p;
        }
        spfy_dll_emu_read(buf_va, tagged + total, copied);
        total += copied;
    }
    tagged[total] = '\0';
    fprintf(stderr, "[test] drain produced %zu bytes of tagged output\n", total);
    if (total > 0) {
        fprintf(stderr, "[test] tagged: %.300s%s\n",
                tagged, total > 300 ? "..." : "");
    }
    free(tagged);

    /* runOrAbort(self, 0) — slot 11. Commits any remaining work. */
    uint32_t runOrAbort_va;
    spfy_dll_emu_read(vtable_va + 11 * 4, &runOrAbort_va, 4);
    uint32_t roa_args[2] = { iobj_va, 0 };
    uint32_t r11 = spfy_dll_emu_call(runOrAbort_va, roa_args, 2);
    spfy_dll_emu_read(iobj_va + 0xd, &err_flag, 1);
    fprintf(stderr, "[test] runOrAbort(self, 0) -> %#x  err_flag=%u\n", r11, err_flag);

    fprintf(stderr, "[test] OK -- full synth chain drove emulator end-to-end\n");
    return 0;
}
