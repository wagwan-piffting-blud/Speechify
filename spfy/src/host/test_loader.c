/*
 * host/test_loader.c — smoke test for the PE loader.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * On Windows this uses LoadLibrary/GetProcAddress to satisfy the DLL's
 * imports (kernel32, user32, winmm, msvcr71) — that's enough to prove
 * the loader works end-to-end on the primary target platform without
 * waiting for the Task 3 import stubs.
 *
 * On Linux/macOS the test is BLOCKED until host/imports.c (Task 3)
 * lands — those stubs are what makes cross-platform hosting actually
 * work.
 *
 * Usage:
 *   ./test_loader path/to/SWIttsFe-en-US.dll
 */

#include "loader.h"
#include "imports.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
#endif

/* getObject prototype matching the DLL's single export. */
typedef int32_t (__stdcall *getObject_fn)(int32_t kind, void **out);

/* IObject vtable signatures we care about — just the first three for
 * COM lifecycle. */
typedef int32_t (__stdcall *vfn_qi_t)(void *self, int32_t kind, void **out);
typedef uint32_t (__stdcall *vfn_addref_t)(void *self);
typedef uint32_t (__stdcall *vfn_release_t)(void *self);

static int load_file(const char *path, uint8_t **out, size_t *out_size) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return -1; }
    uint8_t *buf = (uint8_t *)malloc((size_t)sz);
    if (!buf) { fclose(f); return -1; }
    size_t r = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (r != (size_t)sz) { free(buf); return -1; }
    *out = buf;
    *out_size = (size_t)sz;
    return 0;
}

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "bin/SWIttsFe-en-US.dll";
    uint8_t *bytes; size_t size;
    if (load_file(path, &bytes, &size) != 0) {
        fprintf(stderr, "could not read %s\n", path);
        return 1;
    }
    fprintf(stderr, "[test_loader] loaded %zu bytes from %s\n", size, path);

    /* Task 3 lands the real resolver: covers all 80 imports across
     * KERNEL32/USER32/WINMM/MSVCR71. */
    host_dll_t *dll = host_dll_load(bytes, size, host_default_resolver, NULL);
    if (!dll) {
        fprintf(stderr, "[test_loader] host_dll_load failed: %s\n",
                host_dll_last_error());
        free(bytes);
        return 2;
    }
    fprintf(stderr, "[test_loader] DLL mapped at %p\n", host_dll_image_base(dll));

    getObject_fn getObject = (getObject_fn)host_dll_get_proc(dll, "getObject");
    if (!getObject) {
        fprintf(stderr, "[test_loader] getObject not resolved: %s\n",
                host_dll_last_error());
        host_dll_free(dll); free(bytes);
        return 3;
    }
    fprintf(stderr, "[test_loader] getObject @ %p\n", (void *)getObject);

    void *obj = NULL;
    int32_t rc = getObject(1, &obj);
    fprintf(stderr, "[test_loader] getObject(1, &obj) -> %d, obj=%p\n",
            rc, obj);
    if (!rc || !obj) { host_dll_free(dll); free(bytes); return 4; }

    void ***vt = (void ***)obj;
    /* QueryInterface(kind=2) should succeed too. */
    void *obj2 = NULL;
    int32_t r2 = ((vfn_qi_t)(*vt)[0])(obj, 2, &obj2);
    fprintf(stderr, "[test_loader] QI(kind=2) -> %d, obj2=%p\n", r2, obj2);
    if (obj2) ((vfn_release_t)(*(void ***)obj2)[2])(obj2);

    /* QueryInterface(kind=3) must fail. */
    void *obj3 = NULL;
    int32_t r3 = ((vfn_qi_t)(*vt)[0])(obj, 3, &obj3);
    fprintf(stderr, "[test_loader] QI(kind=3) -> %d, obj3=%p (expect 0/NULL)\n",
            r3, obj3);

    /* AddRef then Release. */
    uint32_t rc_after_add = ((vfn_addref_t)(*vt)[1])(obj);
    fprintf(stderr, "[test_loader] after AddRef: refcount=%u\n", rc_after_add);
    uint32_t rc_after_rel = ((vfn_release_t)(*vt)[2])(obj);
    fprintf(stderr, "[test_loader] after Release: refcount=%u\n", rc_after_rel);
    uint32_t rc_final = ((vfn_release_t)(*vt)[2])(obj);
    fprintf(stderr, "[test_loader] after final Release: refcount=%u (expect 0)\n",
            rc_final);

    host_dll_free(dll);
    free(bytes);
    fprintf(stderr, "[test_loader] PASS\n");
    return 0;
}
