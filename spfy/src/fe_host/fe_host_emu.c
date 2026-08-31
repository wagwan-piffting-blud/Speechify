/*
 * spfy/src/fe_host/fe_host_emu.c - emulator-backed FE.
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
/* SPFY_SILENT: this file's two status lines are unconditional and reach
 * stderr even under -q, which is fine for a terminal and wrong for the
 * subprocess callers spfy_synth --silent exists for. */
#include "../common/env.h"
#include "fe_parse.h"
#include "phoneset.h"
#include "../voice/voice.h"
#include "../host_emu/spfy_dll_boot.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "swittsfe_registry.h"

#define SLOT_RELEASE       2
#define SLOT_INIT_STAGE1   3
#define SLOT_INIT_STAGE2   4
#define SLOT_FEED_CONFIG_A 5
#define SLOT_FEED_CONFIG_B 6
#define SLOT_RUN_OR_ABORT 11
#define SLOT_RESET        26
#define SLOT_DELEGATE_B   42

#define IOBJ_OFF_VTABLE    0x0
#define IOBJ_OFF_REFCOUNT  0x4
#define IOBJ_OFF_STATE     0x8
#define IOBJ_OFF_INIT_FLAG 0xc
#define IOBJ_OFF_ERR_FLAG  0xd

#define DRAIN_BUF_SIZE     256

typedef struct spfy_fe_s {
    uint32_t          iobj_va;
    uint32_t          vtable_va;
    spfy_phoneset_t   phoneset;
    int               phoneset_loaded;
    fe_phone_names_t  phone_names;
    fe_parsed_t       last_parsed;
    int               last_parsed_valid;
    int               espr_enabled;
    char              espr_header[512];
} hosted_fe_t;

/* cp1252 0x80..0x9F -> Unicode. */
static const unsigned short CP1252_HIGH[32] = {
    0x20AC, 0,      0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
    0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0,      0x017D, 0,
    0,      0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
    0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0,      0x017E, 0x0178,
};

/* Transcode to the FE's input codepage, ISO-8859-1, reproducing what the
 * engine does to the UTF-8 it is handed (spfy_dumpwav declares
 * "text/plain;charset=utf-8" and passes the bytes straight to SWIttsSpeak).
 *
 * THE RULE, measured: a codepoint Latin-1 cannot hold is DELETED, not
 * substituted -- and deleted rather than mapped to its cp1252 byte, even when
 * cp1252 has one. The euro is the single exception; the engine normalises it
 * to a word.
 *
 * The probe (charset_probe*.jsonl, engine fe_tree captures) covers 13
 * characters and every one agrees:
 *
 *   s + U+0153 + ur  -> "sur"      oe ligature deleted (cp1252 has it at 0x9C)
 *   S Z Y f c c hats -> deleted    U+0160/017D/0178/0192/0107/010D
 *   U+2030 per-mille -> deleted    (cp1252 0x89 -- so this is not cp1252)
 *   U+00A3 / U+00A5  -> kept       "livre sterling" / "yenne": Latin-1 holds them
 *   U+20AC euro      -> "euros"    reproduced by emitting cp1252 0x80, which
 *                                  our FE normalises the same way, rather than
 *                                  hard-coding a French word here
 *
 * Substituting '?' was the old behaviour and it is what made felix render
 * "Ma soeur" where the engine renders "Ma sur": '?' never reached the FE
 * because argv had already turned U+0153 into 0x9C, which the FE's LTS
 * expands to "oe". Three of felix's remaining audio failures were this.
 *
 * Pure-ASCII input is untouched, so en-US and es-MX are unaffected. */
static void text_to_latin1(const char *in, char *out, size_t out_n) {
    size_t o = 0;
    const unsigned char *p = (const unsigned char *)in;
    while (*p && o + 1 < out_n) {
        unsigned c = *p, cp;
        if (c < 0x80) { out[o++] = (char)c; ++p; continue; }
        int need = ((c & 0xE0) == 0xC0) ? 1
                 : ((c & 0xF0) == 0xE0) ? 2
                 : ((c & 0xF8) == 0xF0) ? 3 : 0;
        int ok = need > 0;
        for (int k = 1; k <= need && ok; ++k)
            if ((p[k] & 0xC0) != 0x80) ok = 0;
        if (ok) {
            cp = c & (0x7Fu >> need);
            for (int k = 1; k <= need; ++k) cp = (cp << 6) | (p[k] & 0x3F);
            p += need + 1;
        } else {
            cp = (c < 0xA0 && CP1252_HIGH[c - 0x80]) ? CP1252_HIGH[c - 0x80] : c;
            ++p;
        }
        if (cp == 0x20AC)   out[o++] = (char)0x80;
        else if (cp <= 0xFF) out[o++] = (char)cp;
    }
    out[o] = '\0';
}


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

static uint32_t vfn_va(hosted_fe_t *fe, int slot) {
    return emu_read32(fe->vtable_va + (uint32_t)slot * 4u);
}

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

/* Drain delegateB into a malloc'd NUL-terminated buffer. */
static char *drain_tagged(hosted_fe_t *fe) {
    size_t cap = 4096;
    char *out = (char *)malloc(cap);
    if (!out) return NULL;
    size_t len = 0;

    /* Allocate guest scratch ONCE so we don't churn the guest heap on every
     * drain iteration. */
    uint32_t buf_va    = spfy_dll_emu_alloc(DRAIN_BUF_SIZE, 0);
    uint32_t outlen_va = spfy_dll_emu_alloc(4, 0);
    if (!buf_va || !outlen_va) { free(out); return NULL; }

    for (int safety = 0; safety < 4096; safety++) {
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

/* Feed plain text into the FE via slot 5. */
static void feed_text(hosted_fe_t *fe, const char *s) {
    uint32_t n = (uint32_t)strlen(s) + 1;
    uint32_t va = spfy_dll_emu_alloc(n, 0);
    if (!va) return;
    spfy_dll_emu_write(va, s, n);
    uint32_t args[1] = { va };
    call_vfn(fe, SLOT_FEED_CONFIG_A, args, 1);
}

/* Same shape as fe_host.c::parse_fe_output_into_slots. */
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
        int rc = fe_parsed_to_full_slots(&fe->last_parsed, ps,
                                        fe->phone_names.names
                                          ? &fe->phone_names : NULL,
                                        &slots, &n_slots);
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

/* Drive the FE over plain text; return the cleaned tagged stream (malloc'd;
 * caller frees). */
static char *hosted_fe_drain_tagged(hosted_fe_t *fe, const char *text) {
    if (iobj_err_flag(fe)) {
        fprintf(stderr, "[fe_host_emu] err_flag latched before synth - bailing\n");
        return NULL;
    }

    /* ESPR mode: feed the voice's control header first (see fe_host.c for
     * the full rationale). */
    if (fe->espr_enabled) {
        feed_text(fe, fe->espr_header);
        uint32_t hdr_empty_va = spfy_dll_emu_alloc(1, 1);
        uint32_t hdrB_args[1] = { hdr_empty_va };
        call_vfn(fe, SLOT_FEED_CONFIG_B, hdrB_args, 1);
    }

    char *latin1 = (char *)malloc(strlen(text) + 1);
    if (!latin1) return NULL;
    text_to_latin1(text, latin1, strlen(text) + 1);
    feed_text(fe, latin1);
    free(latin1);

    uint32_t empty_va = spfy_dll_emu_alloc(1, 1);
    uint32_t fcB_args[1] = { empty_va };
    call_vfn(fe, SLOT_FEED_CONFIG_B, fcB_args, 1);

    char *tagged = drain_tagged(fe);
    if (!tagged) return NULL;
    fe_clean_stream_inplace(tagged);

    uint32_t roa_args[1] = { 0 };
    call_vfn(fe, SLOT_RUN_OR_ABORT, roa_args, 1);
    return tagged;
}

/* ============================================================ Public API -
 * open ============================================================ */

int spfy_fe_open(const char *vocab_json,
                 const char *tables_a_dir,
                 const char *tables_b_dir,
                 spfy_fe_t **out) {
    return spfy_fe_open_lang(NULL, vocab_json, tables_a_dir, tables_b_dir,
                             out);
}

int spfy_fe_open_lang(const char *lang,
                      const char *vocab_json,
                      const char *tables_a_dir,
                      const char *tables_b_dir,
                      spfy_fe_t **out) {
    (void)vocab_json; (void)tables_a_dir; (void)tables_b_dir;
    if (!out) return -1;
    *out = NULL;

    const spfy_fe_dll_entry_t *img = spfy_fe_dll_for_lang(lang);
    if (!img) {
        if (spfy_fe_n_dlls == 0) {
            fprintf(stderr, "[fe_host_emu] no FE DLL images embedded\n");
            return -2;
        }
        if (lang && *lang) {
            fprintf(stderr,
                    "[fe_host_emu] no embedded FE for language '%s' - "
                    "falling back to '%s'\n", lang, spfy_fe_dlls[0].lang);
        }
        img = &spfy_fe_dlls[0];
    }

    /* fr-CA liaison stress inheritance (see fe_host.c / fe_parse). */
    fe_parse_set_liaison_inherit(img->lang && strcmp(img->lang, "fr-CA") == 0);

    hosted_fe_t *fe = (hosted_fe_t *)calloc(1, sizeof(*fe));
    if (!fe) return -1;

    /* Booting the image already mapped is a no-op; a DIFFERENT one re-maps
     * the guest from scratch. That is what makes switching language
     * mid-process work -- but it invalidates every guest VA, so the previous
     * FE must already be closed. spfy_voice_free() does that before the next
     * spfy_voice_load(), which is the only supported order. */
    if (spfy_dll_emu_boot(img->data, (uint32_t)*img->size) != 0) {
        fprintf(stderr, "[fe_host_emu] spfy_dll_emu_boot(%s) failed\n",
                img->lang);
        free(fe); return -2;
    }

    uint32_t getObject_va = spfy_dll_emu_get_export("getObject");
    if (!getObject_va) {
        fprintf(stderr, "[fe_host_emu] getObject export missing\n");
        free(fe); return -3;
    }

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

/* ============================================================ Public API -
 * close ============================================================ */

void spfy_fe_close(spfy_fe_t *opaque) {
    if (!opaque) return;
    hosted_fe_t *fe = (hosted_fe_t *)opaque;
    if (fe->last_parsed_valid) fe_parsed_free(&fe->last_parsed);
    if (fe->iobj_va && fe->vtable_va) {
        call_vfn(fe, SLOT_INIT_STAGE2, NULL, 0);
        call_vfn(fe, SLOT_RESET,       NULL, 0);
        call_vfn(fe, SLOT_RELEASE,     NULL, 0);
    }
    /* We deliberately don't tear down the emulator; the embedded DLL stays
     * mapped for the life of the process. */
    free(fe);
}

/* ============================================================ Public API -
 * synth ============================================================ */

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

    if (!spfy_env("SPFY_SILENT"))
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

/* ============================================================ Public API -
 * voice + stats
 * ============================================================ */

int spfy_fe_set_phone_names(spfy_fe_t *opaque, char *const *names,
                            uint32_t n) {
    if (!opaque) return -1;
    hosted_fe_t *fe = (hosted_fe_t *)opaque;
    /* Borrowed: spfy_voice_t owns the strings via its spfy_phone_order_t,
     * which outlives the FE. */
    fe->phone_names.names = names;
    fe->phone_names.n     = names ? n : 0u;
    return 0;
}

/* ESPR mode on the emulator backend. */
int spfy_fe_set_espr_config(spfy_fe_t *opaque, const char *name,
                            const char *gender, const char *phoneset,
                            const char *version) {
    if (!opaque) return -1;
    hosted_fe_t *fe = (hosted_fe_t *)opaque;

    if (getenv("SPFY_FE_HOST_NO_ESPR")) { fe->espr_enabled = 0; return -1; }

    if (!name || !*name)         name     = "Tom";
    if (!gender || !*gender)     gender   = "male";
    if (!phoneset || !*phoneset) phoneset = "swi_plus_ix";
    if (!version || !*version)   version  = "3.0.0.0";

    int n = snprintf(fe->espr_header, sizeof fe->espr_header,
        "\\\\\\\\!SWIcv%s \\\\\\\\!SWIcg%s \\\\\\\\!SWIcn%s "
        "\\\\\\\\!SWIcl%s \\\\\\\\!SWIespr1 \\\\\\\\!SWIwd0",
        version, gender, name, phoneset);
    if (n < 0 || (size_t)n >= sizeof fe->espr_header) {
        fe->espr_enabled = 0;
        return -1;
    }
    fe->espr_enabled = 1;
    fe_parse_set_refine(0);
    return 0;
}

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
    if (!spfy_env("SPFY_SILENT"))
        fprintf(stderr, "[fe_host_emu] phoneset loaded: %u phonemes, "
                        "silence=%u\n",
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
