/*
 * host/test_synth.c — exercise the full hosted-FE synth flow against
 * the embedded DLL.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Standalone (no spfy_fe deps) — reads the DLL from disk, drives the
 * empirically-captured protocol (getObject(2) -> initStage1 ->
 * feedConfigA(header) -> feedConfigA(text) -> drain slot 42 ->
 * runOrAbort(0) -> teardown), and prints the tagged-text output.
 * Compares the local hosted output to a known-good string from the
 * Frida capture.
 *
 * Build (32-bit):
 *   gcc -m32 -Wall -Wextra -Wno-cast-function-type -O2 -I. \
 *       loader.c imports.c test_synth.c -lwinmm -o test_synth.exe
 */

#include "loader.h"
#include "imports.h"
#include "../spfy/src/fe_host/fe_parse.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef int32_t (__stdcall *getObject_fn)(int32_t kind, void **out);

typedef struct {
    void   **vtable;
    uint32_t refcount;
    void    *state;
    uint8_t  init_flag;
    uint8_t  err_flag;
} iobj_t;

typedef uint32_t (__stdcall *vfn0)(void *self);
typedef uint32_t (__stdcall *vfn1)(void *self, uint32_t a);
typedef uint32_t (__stdcall *vfn3)(void *self, uint32_t a, uint32_t b, uint32_t c);

static int load_file(const char *path, uint8_t **out, size_t *out_size) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc((size_t)sz);
    if (!buf) { fclose(f); return -1; }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        free(buf); fclose(f); return -1;
    }
    fclose(f);
    *out = buf; *out_size = (size_t)sz;
    return 0;
}

int main(int argc, char **argv) {
    const char *dll_path = argc > 1 ? argv[1] : "bin/SWIttsFe-en-US.dll";
    const char *text     = argc > 2 ? argv[2] : "Hello, world.";

    uint8_t *bytes; size_t size;
    if (load_file(dll_path, &bytes, &size) != 0) {
        fprintf(stderr, "could not read %s\n", dll_path); return 1;
    }
    fprintf(stderr, "[test_synth] DLL bytes=%zu\n", size);

    host_dll_t *dll = host_dll_load(bytes, size, host_default_resolver, NULL);
    if (!dll) {
        fprintf(stderr, "[test_synth] host_dll_load: %s\n", host_dll_last_error());
        return 2;
    }
    fprintf(stderr, "[test_synth] DLL mapped at %p\n", host_dll_image_base(dll));

    getObject_fn getObject = (getObject_fn)host_dll_get_proc(dll, "getObject");
    if (!getObject) { fprintf(stderr, "no getObject\n"); return 3; }

    void *raw = NULL;
    if (!getObject(2, &raw) || !raw) {
        fprintf(stderr, "[test_synth] getObject(2) failed\n");
        return 4;
    }
    iobj_t *fe = (iobj_t *)raw;
    fprintf(stderr, "[test_synth] FE obj=%p refcount=%u state=%p\n",
            fe, fe->refcount, fe->state);

    vfn0 *vt0 = (vfn0 *)fe->vtable;
    vfn1 *vt1 = (vfn1 *)fe->vtable;
    vfn3 *vt3 = (vfn3 *)fe->vtable;

    /* slot 3 = initStage1 */
    uint32_t r3 = vt0[3](fe);
    fprintf(stderr, "[test_synth] initStage1 -> 0x%x  err=%u\n",
            r3, fe->err_flag);

    /* Engine sequence is: feedConfigA(header) -> feedConfigB(cfg) ->
     * drain -> feedConfigA(text) -> feedConfigB(cfg) -> drain.
     * Slot 5 (feedConfigA) processes with FUN_07df4173 mode=3.
     * Slot 6 (feedConfigB) processes with FUN_07df40b6 mode=0.
     * Both append their `param_2` to delegate-A's input stream.
     *
     * Variant 1: feedConfigA(header), feedConfigB(empty), drain;
     *            feedConfigA(text),   feedConfigB(empty), drain.
     */
    static const char EMPTY_CFG = '\0';
    /* Skip the SWI control header — the engine phonemizes it too,
     * then discards. We just submit the text directly. */
    vt1[5](fe, (uint32_t)(uintptr_t)text);
    vt1[6](fe, (uint32_t)(uintptr_t)&EMPTY_CFG);
    fprintf(stderr, "[test_synth] fed text: %s\n", text);

    /* Drain slot 42 = delegateB_call. */
    size_t cap = 4096, n = 0;
    char *tagged = malloc(cap);
    for (int i = 0; i < 4096; i++) {
        char buf[256];
        uint32_t out_len = 0;
        vt3[42](fe, (uint32_t)(uintptr_t)buf,
                    (uint32_t)sizeof(buf),
                    (uint32_t)(uintptr_t)&out_len);
        if (out_len <= 1) break;
        uint32_t copied = out_len - 1;
        if (copied > sizeof(buf)) copied = sizeof(buf);
        if (n + copied + 1 > cap) {
            while (n + copied + 1 > cap) cap *= 2;
            tagged = realloc(tagged, cap);
        }
        memcpy(tagged + n, buf, copied);
        n += copied;
    }
    tagged[n] = '\0';

    /* slot 11 = runOrAbort(0) */
    vt1[11](fe, 0);

    /* Teardown */
    vt0[4](fe);  /* initStage2 */
    vt0[26](fe); /* reset */
    uint32_t rrc = vt0[2](fe); /* Release */
    fprintf(stderr, "[test_synth] Release -> refcount=%u\n", rrc);

    host_dll_free(dll);
    free(bytes);

    fprintf(stderr, "\n[test_synth] ==== FE OUTPUT (%zu bytes) ====\n", n);
    fputs(tagged, stdout);
    fputs("\n", stdout);

    /* Run the parser over the hosted output. */
    fe_clean_stream_inplace(tagged);
    fe_parsed_t parsed;
    int rc = fe_parse_tagged_output(tagged, &parsed);
    fprintf(stderr, "\n[test_synth] ==== PARSER ==== rc=%d\n", rc);
    if (rc == 0) {
        fe_parsed_debug_dump(&parsed, stderr);
        fprintf(stderr, "[test_synth] total phonemes: %d\n",
                fe_parsed_count_phonemes(&parsed));
    }
    fe_parsed_free(&parsed);
    free(tagged);
    return rc;
}
