/*
 * spfy/src/fe_host/fe_host_emu.c — emulator-backed FE.
 *
 * Implements the same public API as fe_host.c (the native-PE host) but
 * drives the embedded SWIttsFe-en-US.dll through spfy/src/host_emu/
 * (portable x86 interpreter). Built when SPFY_FE_EMU=ON. Selected
 * automatically on non-x86 host platforms (Android arm64, WASM, Apple
 * Silicon) where the native PE loader won't work.
 *
 * Drives the same call sequence the native path uses (validated 2026-06-30):
 *   spfy_dll_emu_boot(swittsfe_dll_data, swittsfe_dll_size)
 *   getObject(2, &iobj_va)                 -> guest VA of the FE object
 *   read iobj.vtable
 *   call vtable[3]  initStage1(self)
 *   call vtable[5]  feedConfigA(self, text_va)
 *   call vtable[6]  feedConfigB(self, &empty)
 *   loop vtable[42] delegateB(self, buf_va, cap, &out_len)  -> tagged stream
 *   call vtable[11] runOrAbort(self, 0)
 *
 * Output is byte-identical to the native path (proven via test_emu_boot.c).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "fe.h"
#include "fe_parse.h"
#include "phoneset.h"
#include "../voice/voice.h"
#include "../host_emu/spfy_dll_boot.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern const unsigned char swittsfe_dll_data[];
extern const size_t        swittsfe_dll_size;

/* Vtable slots — same numbering the native path uses. */
#define SLOT_RELEASE       2
#define SLOT_INIT_STAGE1   3
#define SLOT_INIT_STAGE2   4
#define SLOT_FEED_CONFIG_A 5
#define SLOT_FEED_CONFIG_B 6
#define SLOT_RUN_OR_ABORT 11
#define SLOT_RESET        26
#define SLOT_DELEGATE_B   42

/* iobj layout (from disasm) — same field offsets the native struct uses. */
#define IOBJ_OFF_VTABLE    0x0
#define IOBJ_OFF_REFCOUNT  0x4
#define IOBJ_OFF_STATE     0x8
#define IOBJ_OFF_INIT_FLAG 0xc
#define IOBJ_OFF_ERR_FLAG  0xd

#define DRAIN_BUF_SIZE     256

typedef struct spfy_fe_s {
    uint32_t          iobj_va;            /* guest VA of the FE iobj */
    uint32_t          vtable_va;          /* cached: iobj_va -> +0 */
    spfy_phoneset_t   phoneset;
    int               phoneset_loaded;
    fe_parsed_t       last_parsed;
    int               last_parsed_valid;
} hosted_fe_t;

/* --- internal helpers --- */

static uint32_t emu_read32(uint32_t va) {
    uint32_t v;
    spfy_dll_emu_read(va, &v, 4);
    return v;
}

static uint8_t emu_read8(uint32_t va) {
    uint8_t v;
    spfy_dll_emu_read(va, &v, 1);
    return v;
}

/* Read a vtable slot's function VA. */
static uint32_t vfn_va(hosted_fe_t *fe, int slot) {
    return emu_read32(fe->vtable_va + (uint32_t)slot * 4u);
}

/* Call vtable[slot](self, args...). Returns guest EAX. */
static uint32_t call_vfn(hosted_fe_t *fe, int slot,
                         const uint32_t *args, int n) {
    uint32_t fn = vfn_va(fe, slot);
    uint32_t all[8];
    if (n + 1 > 8) { fprintf(stderr, "[fe_host_emu] too many args\n"); return 0; }
    all[0] = fe->iobj_va;
    for (int i = 0; i < n; i++) all[i + 1] = args[i];
    return spfy_dll_emu_call(fn, all, n + 1);
}

static uint8_t iobj_err_flag(hosted_fe_t *fe) {
    return emu_read8(fe->iobj_va + IOBJ_OFF_ERR_FLAG);
}

/* Drain delegateB into a malloc'd NUL-terminated buffer. Same shape as
 * the native drain_delegate_b but reads through guest memory. */
static char *drain_tagged(hosted_fe_t *fe) {
    size_t cap = 4096;
    char *out = (char *)malloc(cap);
    if (!out) return NULL;
    size_t len = 0;

    /* Allocate guest scratch ONCE so we don't churn the guest heap on
     * every drain iteration. Both buffers stay across calls; if the
     * synth runs in a loop they get reused. */
    uint32_t buf_va    = spfy_dll_emu_alloc(DRAIN_BUF_SIZE, 0);
    uint32_t outlen_va = spfy_dll_emu_alloc(4, 0);
    if (!buf_va || !outlen_va) { free(out); return NULL; }

    for (int safety = 0; safety < 4096; safety++) {
        /* zero out_len before each call */
        uint32_t zero = 0;
        spfy_dll_emu_write(outlen_va, &zero, 4);

        uint32_t args[3] = { buf_va, DRAIN_BUF_SIZE, outlen_va };
        call_vfn(fe, SLOT_DELEGATE_B, args, 3);

        uint32_t out_len = emu_read32(outlen_va);
        if (out_len <= 1) break;

        uint32_t copied = out_len - 1;
        if (copied > DRAIN_BUF_SIZE) copied = DRAIN_BUF_SIZE;
        if (len + copied + 1 > cap) {
            while (len + copied + 1 > cap) cap *= 2;
            char *p = (char *)realloc(out, cap);
            if (!p) { free(out); return NULL; }
            out = p;
        }
        spfy_dll_emu_read(buf_va, out + len, copied);
        len += copied;
    }
    out[len] = '\0';
    return out;
}

/* Feed plain text into the FE via slot 5. Text gets copied into the
 * guest heap so the DLL has a stable pointer. */
static void feed_text(hosted_fe_t *fe, const char *s) {
    uint32_t n = (uint32_t)strlen(s) + 1;
    uint32_t va = spfy_dll_emu_alloc(n, 0);
    if (!va) return;
    spfy_dll_emu_write(va, s, n);
    uint32_t args[1] = { va };
    call_vfn(fe, SLOT_FEED_CONFIG_A, args, 1);
}

/* Same shape as fe_host.c::parse_fe_output_into_slots. The phoneset
 * (if loaded) drives full slot construction; otherwise we flatten. */
static int parse_fe_output_into_slots(hosted_fe_t *fe,
                                      const char *tagged,
                                      const spfy_prosody_hints_t *hints,
                                      spfy_fe_utterance_t *u) {
    (void)hints;
    if (fe->last_parsed_valid) {
        fe_parsed_free(&fe->last_parsed);
        fe->last_parsed_valid = 0;
    }
    if (fe_parse_tagged_output(tagged, &fe->last_parsed) != 0) {
        u->slots = NULL; u->n_slots = 0;
        return -1;
    }
    fe->last_parsed_valid = 1;
    const spfy_phoneset_t *ps = fe->phoneset_loaded ? &fe->phoneset : NULL;
    if (ps) {
        spfy_fe_slot_t *slots = NULL;
        uint32_t n_slots = 0;
        int rc = fe_parsed_to_full_slots(&fe->last_parsed, ps, &slots, &n_slots);
        if (rc != 0) return rc;
        u->slots   = slots;
        u->n_slots = n_slots;
    } else {
        int n = fe_parsed_count_phonemes(&fe->last_parsed);
        if (n > 0) {
            u->slots = (spfy_fe_slot_t *)calloc((size_t)n, sizeof(*u->slots));
            if (!u->slots) return -1;
            fe_parsed_flatten_to_slots(&fe->last_parsed, u->slots, n);
            u->n_slots = (uint32_t)n;
        } else {
            u->slots = NULL; u->n_slots = 0;
        }
    }
    if (getenv("SPFY_FE_HOST_DEBUG"))
        fe_parsed_debug_dump(&fe->last_parsed, stderr);
    return 0;
}

/* Drive the FE over plain text; return the cleaned tagged stream
 * (malloc'd; caller frees). NULL on error. */
static char *hosted_fe_drain_tagged(hosted_fe_t *fe, const char *text) {
    if (iobj_err_flag(fe)) {
        fprintf(stderr, "[fe_host_emu] err_flag latched before synth — bailing\n");
        return NULL;
    }
    feed_text(fe, text);

    /* feedConfigB takes a pointer to a single NUL byte (the empty cfg). */
    uint32_t empty_va = spfy_dll_emu_alloc(1, 1);
    uint32_t fcB_args[1] = { empty_va };
    call_vfn(fe, SLOT_FEED_CONFIG_B, fcB_args, 1);

    char *tagged = drain_tagged(fe);
    if (!tagged) return NULL;
    fe_clean_stream_inplace(tagged);

    /* runOrAbort(0). */
    uint32_t roa_args[1] = { 0 };
    call_vfn(fe, SLOT_RUN_OR_ABORT, roa_args, 1);
    return tagged;
}

/* ============================================================
 * Public API — open
 * ============================================================ */

int spfy_fe_open(const char *vocab_json,
                 const char *tables_a_dir,
                 const char *tables_b_dir,
                 spfy_fe_t **out) {
    (void)vocab_json; (void)tables_a_dir; (void)tables_b_dir;
    if (!out) return -1;
    *out = NULL;

    hosted_fe_t *fe = (hosted_fe_t *)calloc(1, sizeof(*fe));
    if (!fe) return -1;

    if (spfy_dll_emu_boot(swittsfe_dll_data, (uint32_t)swittsfe_dll_size) != 0) {
        fprintf(stderr, "[fe_host_emu] spfy_dll_emu_boot failed\n");
        free(fe); return -2;
    }

    uint32_t getObject_va = spfy_dll_emu_get_export("getObject");
    if (!getObject_va) {
        fprintf(stderr, "[fe_host_emu] getObject export missing\n");
        free(fe); return -3;
    }

    /* getObject(2, &iobj) — cdecl, returns nonzero on success. */
    uint32_t out_va = spfy_dll_emu_alloc(4, 1);
    uint32_t args[2] = { 2, out_va };
    uint32_t rc = spfy_dll_emu_call(getObject_va, args, 2);
    fe->iobj_va = emu_read32(out_va);
    if (!rc || !fe->iobj_va) {
        fprintf(stderr, "[fe_host_emu] getObject(2) -> rc=%u iobj=%#x\n",
                rc, fe->iobj_va);
        free(fe); return -4;
    }
    fe->vtable_va = emu_read32(fe->iobj_va + IOBJ_OFF_VTABLE);

    /* initStage1(self). */
    uint32_t r3 = call_vfn(fe, SLOT_INIT_STAGE1, NULL, 0);
    if (iobj_err_flag(fe)) {
        fprintf(stderr, "[fe_host_emu] initStage1 set err_flag (ret=%#x)\n", r3);
    }

    if (getenv("SPFY_HOST_TRACE")) {
        fprintf(stderr,
                "[fe_host_emu] booted: iobj=%#x  vtable=%#x  initStage1 rc=%#x\n",
                fe->iobj_va, fe->vtable_va, r3);
    }

    *out = (spfy_fe_t *)fe;
    return 0;
}

/* ============================================================
 * Public API — close
 * ============================================================ */

void spfy_fe_close(spfy_fe_t *opaque) {
    if (!opaque) return;
    hosted_fe_t *fe = (hosted_fe_t *)opaque;
    if (fe->last_parsed_valid) fe_parsed_free(&fe->last_parsed);
    if (fe->iobj_va && fe->vtable_va) {
        call_vfn(fe, SLOT_INIT_STAGE2, NULL, 0);
        call_vfn(fe, SLOT_RESET,       NULL, 0);
        call_vfn(fe, SLOT_RELEASE,     NULL, 0);
    }
    /* We deliberately don't tear down the emulator; the embedded DLL
     * stays mapped for the life of the process. A future spfy_fe_open
     * call boots idempotently (spfy_dll_emu_boot returns 0 on already-
     * booted). Matches the native path which never unmaps the DLL. */
    free(fe);
}

/* ============================================================
 * Public API — synth
 * ============================================================ */

int spfy_fe_text_to_tagged(spfy_fe_t  *opaque,
                           const char *text,
                           char       *out,
                           size_t      out_n) {
    if (!opaque || !text || !out || out_n == 0) return -1;
    out[0] = '\0';
    hosted_fe_t *fe = (hosted_fe_t *)opaque;
    char *tagged = hosted_fe_drain_tagged(fe, text);
    if (!tagged) return -3;
    size_t n = strlen(tagged);
    if (n >= out_n) n = out_n - 1;
    memcpy(out, tagged, n);
    out[n] = '\0';
    free(tagged);
    return (int)n;
}

int spfy_fe_synth_text(spfy_fe_t                  *opaque,
                       const char                 *text,
                       const spfy_prosody_hints_t *hints,
                       spfy_fe_utterance_t       **out_utt) {
    if (!opaque || !text || !out_utt) return -1;
    hosted_fe_t *fe = (hosted_fe_t *)opaque;
    *out_utt = NULL;

    char *tagged = hosted_fe_drain_tagged(fe, text);
    if (!tagged) return iobj_err_flag(fe) ? -2 : -3;

    spfy_fe_utterance_t *u = (spfy_fe_utterance_t *)calloc(1, sizeof(*u));
    if (!u) { free(tagged); return -3; }
    u->hints = hints;
    parse_fe_output_into_slots(fe, tagged, hints, u);

    fprintf(stderr, "[fe_host_emu] tagged output (%zu bytes): %s\n",
            strlen(tagged), tagged);
    free(tagged);

    *out_utt = u;
    return 0;
}

int spfy_fe_synth_tagged(spfy_fe_t                  *opaque,
                         const char                 *tagged,
                         const spfy_prosody_hints_t *hints,
                         spfy_fe_utterance_t       **out_utt) {
    if (!opaque || !tagged || !out_utt) return -1;
    hosted_fe_t *fe = (hosted_fe_t *)opaque;
    *out_utt = NULL;
    spfy_fe_utterance_t *u = (spfy_fe_utterance_t *)calloc(1, sizeof(*u));
    if (!u) return -3;
    u->hints = hints;
    if (parse_fe_output_into_slots(fe, tagged, hints, u) != 0) {
        free(u);
        return -1;
    }
    *out_utt = u;
    return 0;
}

void spfy_fe_utterance_free(spfy_fe_utterance_t *u) {
    if (!u) return;
    free(u->slots);
    free(u);
}

/* ============================================================
 * Public API — voice + stats
 * ============================================================ */

int spfy_fe_set_voice_vcf(spfy_fe_t *opaque, const char *vcf_path) {
    if (!opaque || !vcf_path) return -1;
    hosted_fe_t *fe = (hosted_fe_t *)opaque;
    spfy_vcf_t vcf;
    int rc = spfy_vcf_load(vcf_path, &vcf);
    if (rc != 0) {
        fprintf(stderr, "[fe_host_emu] spfy_vcf_load(%s) -> %d\n", vcf_path, rc);
        return rc;
    }
    memset(&fe->phoneset, 0, sizeof fe->phoneset);
    rc = spfy_phoneset_load_from_vcf(&vcf, &fe->phoneset);
    spfy_vcf_free(&vcf);
    if (rc != 0) {
        fprintf(stderr, "[fe_host_emu] spfy_phoneset_load_from_vcf -> %d\n", rc);
        return rc;
    }
    fe->phoneset_loaded = 1;
    fprintf(stderr, "[fe_host_emu] phoneset loaded: %u phonemes, silence=%u\n",
            fe->phoneset.n_phones, fe->phoneset.silence_phone_id);
    return 0;
}

const spfy_fe_vocab_t  *spfy_fe_vocab   (const spfy_fe_t *fe) { (void)fe; return NULL; }
const spfy_fe_tables_t *spfy_fe_tables  (const spfy_fe_t *fe) { (void)fe; return NULL; }
const spfy_phoneset_t  *spfy_fe_phoneset(const spfy_fe_t *opaque) {
    if (!opaque) return NULL;
    const hosted_fe_t *fe = (const hosted_fe_t *)opaque;
    return fe->phoneset_loaded ? &fe->phoneset : NULL;
}

const void *spfy_fe_get_parsed(const spfy_fe_t *opaque) {
    if (!opaque) return NULL;
    const hosted_fe_t *fe = (const hosted_fe_t *)opaque;
    return fe->last_parsed_valid ? (const void *)&fe->last_parsed : NULL;
}

int spfy_fe_textnorm_only(const spfy_fe_t *fe, const char *text,
                          const spfy_prosody_hints_t *hints,
                          spfy_fe_delta_t *delta) {
    (void)fe; (void)text; (void)hints; (void)delta;
    return -1;
}

void spfy_fe_print_stats(const spfy_fe_t *opaque) {
    if (!opaque) return;
    const hosted_fe_t *fe = (const hosted_fe_t *)opaque;
    uint8_t err = fe->iobj_va ? emu_read8(fe->iobj_va + IOBJ_OFF_ERR_FLAG) : 0;
    uint8_t init = fe->iobj_va ? emu_read8(fe->iobj_va + IOBJ_OFF_INIT_FLAG) : 0;
    uint32_t refc = fe->iobj_va ? emu_read32(fe->iobj_va + IOBJ_OFF_REFCOUNT) : 0;
    fprintf(stderr,
            "[fe_host_emu] iobj=%#x  vtable=%#x  refcount=%u  init_flag=%u  err_flag=%u\n",
            fe->iobj_va, fe->vtable_va, refc, init, err);
}
