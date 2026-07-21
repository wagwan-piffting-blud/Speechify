/*
 * spfy/src/fe_host/fe_host.c — hosted FE backed by SWIttsFe-en-US.dll.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Empirical drive sequence (captured 2026-05-10/11 via Frida — see
 * host/PROTOCOL.md and viz/frida_hooks/fe_vtable_trace.js):
 *
 *   default path (works, lossy phonemic detail):
 *     getObject(2,&fe) → initStage1 → feedConfigA(text) → feedConfigB
 *       → loop slot42 delegateB_call → runOrAbort
 *
 *   verbose path (SPFY_FE_HOST_VERBOSE=1, work in progress):
 *     getObject(2,&fe) → AddRef
 *       → installHookA/B → setPair_E/F/G/H   (eng-stub adapters,
 *           reimplemented from SWIttsEngine.dll +0x4c10..+0x4d40)
 *       → initStage1
 *       → feedConfigA(SWI_HEADER) → feedConfigB
 *       → feedConfigA(text)       → feedConfigB
 *       → loop slot42 delegateB_call (or harvest from wrapper.bs_E)
 *       → runOrAbort
 *
 * Verbose mode currently produces the same lossy tagged-text as the
 * default path: the FE's internal initStage1 doesn't allocate the
 * "verbose output buffer" at ctrl+0x648 in our process the way it
 * does in real Speechify. The differential bisection and disasm
 * tracing to find the missing trigger is the open work item.
 *
 * Diagnostic env vars (all opt-in, off by default):
 *   SPFY_FE_HOST_VERBOSE     enable verbose-mode init sequence
 *   SPFY_FE_HOST_DELTA       per-vtable-call ctrl[0..0x800] delta tracer
 *   SPFY_FE_HOST_DUMP_STATE  full state+ctrl dump before slot42 drain
 *   SPFY_FE_HOST_BISECT_CTRL <bin> — memcpy Speechify-captured
 *                            ctrl[0..0x800] into ours before initStage1
 *                            (differential bisection — see RESUME_K2.md)
 *   SPFY_FE_HOST_BISECT_STATE <bin> — memcpy Speechify-captured
 *                            state[0..0x1300] into ours before initStage1
 *                            (state+0x6c ctrl ptr preserved)
 *   SPFY_FE_HOST_DUMP_PRE_INIT_CTRL <bin> — write our ctrl[0..0x800]
 *                            just before initStage1, for diff vs Frida
 *   SPFY_HOST_NO_ZERO_MALLOC opt out of calloc-init for malloc/op_new
 *   SPFY_HOST_STARTUP_DELAY_MS pause after PE load (Frida-attach window)
 */

#include "fe.h"
#include "fe_parse.h"
#include "phoneset.h"
#include "../voice/voice.h"
#include "../host/loader.h"
#include "../host/imports.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
/* __stdcall is a calling-convention attribute. mingw/MSVC make it a
 * keyword; on Linux gcc i386 we expose it as the matching attribute so
 * the typedef'd vfnN signatures below have the right ABI. On amd64
 * stdcall has no semantic meaning so we leave it empty. */
#  if defined(__i386__)
#    define __stdcall __attribute__((stdcall))
#    define __cdecl   __attribute__((cdecl))
#  else
#    define __stdcall
#    define __cdecl
#  endif
#endif

#include "swittsfe_registry.h"

typedef struct {
    void   **vtable;       /* +0x0 */
    uint32_t refcount;     /* +0x4 */
    void    *state;        /* +0x8 */
    uint8_t  init_flag;    /* +0xc */
    uint8_t  err_flag;     /* +0xd */
} hosted_fe_iobj_t;

typedef struct spfy_fe_s {
    host_dll_t       *dll;
    hosted_fe_iobj_t *iobj;
    spfy_phoneset_t   phoneset;
    int               phoneset_loaded;
    fe_phone_names_t  phone_names;      /* voice feat["name"] order */
    fe_parsed_t       last_parsed;       /* result of most recent synth */
    int               last_parsed_valid;
    int               espr_enabled;      /* ESPR mode header is set */
    char              espr_header[512];  /* \!SWIcv... \!SWIespr1 header */
} hosted_fe_t;

/* getObject is exported as __cdecl despite the typical Win32 DLL
 * convention — verified via disassembly: epilogue is a plain `ret`,
 * not `ret 8`. The mismatch matters only on Linux where our caller
 * compiles with __stdcall and assumes callee-cleans-up; mingw's
 * Win32 ABI nominally tolerates the difference at call sites that use
 * EBP-relative addressing for locals, but our gcc-Linux build uses
 * ESP-relative addressing so the post-call ESP slop corrupts every
 * subsequent local access. */
typedef int32_t  (__cdecl *getObject_fn)(int32_t kind, void **out);
typedef uint32_t (__stdcall *vfn0)(void *self);
typedef uint32_t (__stdcall *vfn1)(void *self, uint32_t a);
typedef uint32_t (__stdcall *vfn3)(void *self, uint32_t a, uint32_t b, uint32_t c);
typedef uint32_t (__stdcall *vfn2)(void *self, uint32_t a, uint32_t b);

#define SLOT_RELEASE       2
#define SLOT_INIT_STAGE1   3
#define SLOT_INIT_STAGE2   4
#define SLOT_FEED_CONFIG_A 5
#define SLOT_RUN_OR_ABORT 11
#define SLOT_RESET        26
#define SLOT_DELEGATE_B   42

#define DRAIN_BUF_SIZE     256

/* ============================================================
 * Public API — open
 * ============================================================ */

int spfy_fe_open(const char *vocab_json,
                 const char *tables_a_dir,
                 const char *tables_b_dir,
                 spfy_fe_t **out) {
    /* Back-compat entry point: no language given, so use the first
     * embedded image (en-US by build convention). */
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

    /* Pick the embedded SWIttsFe image for this voice's language. Falls
     * back to the first entry (en-US) with a warning, which keeps a voice
     * whose language was not built in loadable, if wrong-sounding. */
    const spfy_fe_dll_entry_t *img = spfy_fe_dll_for_lang(lang);
    if (!img) {
        if (spfy_fe_n_dlls == 0) {
            fprintf(stderr, "[fe_host] no FE DLL images embedded\n");
            return -2;
        }
        if (lang && *lang) {
            fprintf(stderr,
                    "[fe_host] no embedded FE for language '%s' — falling "
                    "back to '%s'. Rebuild with -DSPFY_FE_LANGS=\"...;%s\" "
                    "for correct pronunciation.\n",
                    lang, spfy_fe_dlls[0].lang, lang);
        }
        img = &spfy_fe_dlls[0];
    }

    /* fr-CA words can arrive with bare leading phones (liaison/enchainement);
     * enable stress inheritance so they continue the previous word's final
     * syllable. No-op for en-US/es-MX (they never emit bare-leading words). */
    fe_parse_set_liaison_inherit(img->lang && strcmp(img->lang, "fr-CA") == 0);

    /* (delay moved below — needs to fire AFTER PE loading so the DLL
     * is mapped at expected addresses and Frida can attach hooks.) */

    hosted_fe_t *fe = (hosted_fe_t *)calloc(1, sizeof(*fe));
    if (!fe) return -1;

    fe->dll = host_dll_load(img->data, *img->size,
                            host_default_resolver, NULL);
    if (!fe->dll) {
        fprintf(stderr, "[fe_host] host_dll_load(%s) failed: %s\n",
                img->lang, host_dll_last_error());
        free(fe); return -2;
    }
    fprintf(stderr, "[fe_host] FE image: SWIttsFe-%s.dll (%zu bytes)\n",
            img->lang, *img->size);

    getObject_fn getObject =
        (getObject_fn)host_dll_get_proc(fe->dll, "getObject");
    if (!getObject) {
        fprintf(stderr, "[fe_host] getObject not exported: %s\n",
                host_dll_last_error());
        host_dll_free(fe->dll); free(fe); return -3;
    }

    void *raw = NULL;
    /* kind=2 matches what Speechify.exe uses (per Frida capture). */
    int32_t rc = getObject(2, &raw);
    if (getenv("SPFY_HOST_TRACE")) {
        fprintf(stderr, "[fe_host] getObject(2) -> rc=%d  raw=%p\n", rc, raw);
    }
    if (!rc || !raw) {
        fprintf(stderr, "[fe_host] getObject(2, ...) -> %d, obj=%p\n",
                rc, raw);
        host_dll_free(fe->dll); free(fe); return -4;
    }
    fe->iobj = (hosted_fe_iobj_t *)raw;
    vfn0 *vt = (vfn0 *)fe->iobj->vtable;

    /* slot 3 = initStage1 — installs the internal delegates. */
    uint32_t r3 = vt[SLOT_INIT_STAGE1](fe->iobj);
    if (fe->iobj->err_flag) {
        fprintf(stderr, "[fe_host] initStage1 set err_flag (ret=0x%x)\n", r3);
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
    if (fe->iobj && fe->iobj->vtable) {
        vfn0 *vt = (vfn0 *)fe->iobj->vtable;
        vt[SLOT_INIT_STAGE2](fe->iobj);
        vt[SLOT_RESET](fe->iobj);
        vt[SLOT_RELEASE](fe->iobj);
    }
    if (fe->dll) host_dll_free(fe->dll);
    free(fe);
}

/* ============================================================
 * Synth: drive the engine and drain the tagged-output stream
 * ============================================================ */

/* Drain an output delegate slot into a growing buffer. Returns malloc'd
 * NUL-terminated string (caller frees). NULL on OOM. `slot` is the vtable
 * index of a delegateB-style pull method (42 = state[+0x2dc],
 * 44 = state[+0x2e0]). */
static char *drain_delegate_slot(hosted_fe_iobj_t *iobj, int slot) {
    size_t cap = 4096;
    char *out = (char *)malloc(cap);
    if (!out) return NULL;
    size_t len = 0;

    vfn3 fnd = (vfn3)((vfn0 *)iobj->vtable)[slot];

    for (int safety = 0; safety < 4096; safety++) {
        char buf[DRAIN_BUF_SIZE];
        uint32_t out_len = 0;
        uint32_t r = fnd(iobj, (uint32_t)(uintptr_t)buf,
                              (uint32_t)sizeof(buf),
                              (uint32_t)(uintptr_t)&out_len);
        (void)r;
        /* Per FUN_0836c420: *out_len = bytes_copied + 1; if bytes_copied
         * == 0 the stream is exhausted. */
        if (out_len <= 1) break;
        uint32_t copied = out_len - 1;
        if (copied > sizeof(buf)) copied = sizeof(buf);
        if (len + copied + 1 > cap) {
            while (len + copied + 1 > cap) cap *= 2;
            char *p = (char *)realloc(out, cap);
            if (!p) { free(out); return NULL; }
            out = p;
        }
        memcpy(out + len, buf, copied);
        len += copied;
    }
    out[len] = '\0';
    return out;
}

static char *drain_delegate_b(hosted_fe_iobj_t *iobj) {
    return drain_delegate_slot(iobj, SLOT_DELEGATE_B);
}

/* Push a NUL-terminated string into the FE via slot 5 (feedConfigA).
 * The engine's capture shows feedConfigA accepts a const char *
 * directly — the wrapper takes (this, char *) as 2 stack args. */
static void feed_text(hosted_fe_iobj_t *iobj, const char *s) {
    vfn1 fn5 = (vfn1)((vfn0 *)iobj->vtable)[SLOT_FEED_CONFIG_A];
    fn5(iobj, (uint32_t)(uintptr_t)s);
}

/* Transcode input text to ISO-8859-1 (Latin-1) into `out`, tolerating
 * EITHER UTF-8 or already-Latin-1 input.
 *
 * The Eloquence FE expects Latin-1: the real engine's text layer converts
 * the UTF-8 it receives (SWIttsSpeak charset=utf-8) down to the FE's
 * single-byte codepage first. Passing raw UTF-8 makes the FE see each
 * accented byte pair as two garbage characters and hallucinate words
 * (e.g. "dîner" -> "<registr...e () undef>"), which also throws our
 * tagged-output parser.
 *
 * But our two input routes deliver DIFFERENT encodings: spfy_synth's
 * narrow main() gets command-line args in the Windows system codepage
 * (Windows-1252 ~= Latin-1, so `í` arrives as a lone 0xED), while a -f
 * file is read as raw bytes (UTF-8, `í` = 0xC3 0xAD). So we decode valid
 * UTF-8 multi-byte sequences, but pass a high byte that is NOT a valid
 * UTF-8 sequence straight through as already-Latin-1. ASCII (en-US) is
 * identical either way, so Tom/Jill are unaffected. Latin-1 also matches
 * the engine on byte OFFSETS (1 byte per accent), which the FE reports in
 * its word (offset,len) tags. Codepoints > 0xFF become '?'. */
static void text_to_latin1(const char *in, char *out, size_t out_n) {
    size_t o = 0;
    const unsigned char *p = (const unsigned char *)in;
    while (*p && o + 1 < out_n) {
        unsigned c = *p;
        if (c < 0x80) { out[o++] = (char)c; ++p; continue; }

        int need = ((c & 0xE0) == 0xC0) ? 1
                 : ((c & 0xF0) == 0xE0) ? 2
                 : ((c & 0xF8) == 0xF0) ? 3 : 0;
        /* Valid UTF-8 iff all `need` continuation bytes are 0x80..0xBF. */
        int ok = need > 0;
        for (int k = 1; k <= need && ok; ++k)
            if ((p[k] & 0xC0) != 0x80) ok = 0;

        if (!ok) {
            /* Not valid UTF-8 here -> treat this byte as Latin-1 already. */
            out[o++] = (char)c; ++p; continue;
        }
        unsigned cp = c & (0x7Fu >> need);
        for (int k = 1; k <= need; ++k) cp = (cp << 6) | (p[k] & 0x3F);
        out[o++] = (cp <= 0xFF) ? (char)cp : '?';
        p += need + 1;
    }
    out[o] = '\0';
}

static int parse_fe_output_into_slots(hosted_fe_t *fe,
                                      const char *tagged,
                                      const spfy_prosody_hints_t *hints,
                                      spfy_fe_utterance_t *u) {
    (void)hints;
    if (getenv("SPFY_FE_HOST_DUMP_TAGGED"))
        fprintf(stderr, "[fe_host tagged] %s\n", tagged ? tagged : "(null)");
    /* Free any prior parsed result before reusing the slot. */
    if (fe->last_parsed_valid) {
        fe_parsed_free(&fe->last_parsed);
        fe->last_parsed_valid = 0;
    }
    if (fe_parse_tagged_output(tagged, &fe->last_parsed) != 0) {
        u->slots = NULL; u->n_slots = 0;
        return -1;
    }
    fe->last_parsed_valid = 1;
    /* Use the full slot-builder when we have a phoneset; otherwise
     * fall back to the lightweight emphasis-only flattener. */
    const spfy_phoneset_t *ps = fe->phoneset_loaded ? &fe->phoneset : NULL;
    if (getenv("SPFY_FE_HOST_DEBUG")) {
        /* Which phone-id table is in force. When phone_names is absent the
         * FE falls back to the compiled-in en-US ARPAbet table, which
         * silently mis-numbers every non-en-US voice. */
        fprintf(stderr, "[fe_host] phone-id source: %s (n=%u), "
                        "vcf phoneset n=%u silence=%u\n",
                fe->phone_names.names ? "VIN feat[\"name\"]"
                                      : "compiled-in en-US ARPAbet",
                fe->phone_names.n,
                ps ? ps->n_phones : 0u,
                ps ? ps->silence_phone_id : 0xffu);
    }
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

const void *spfy_fe_get_parsed(const spfy_fe_t *opaque) {
    if (!opaque) return NULL;
    const hosted_fe_t *fe = (const hosted_fe_t *)opaque;
    return fe->last_parsed_valid ? (const void *)&fe->last_parsed : NULL;
}

/* Drive the FE over plain text and return the cleaned tagged-output
 * stream (malloc'd; caller frees). NULL on error. Shared by
 * spfy_fe_synth_text (which parses it into slots) and
 * spfy_fe_text_to_tagged (which hands it back raw so the caller can
 * splice DLL-FE words together with inline-SPR tagged blocks). */
static char *hosted_fe_drain_tagged(hosted_fe_t *fe, const char *text) {
    if (fe->iobj->err_flag) {
        fprintf(stderr, "[fe_host] err_flag latched before synth — bailing\n");
        return NULL;
    }
    static const char EMPTY_CFG = '\0';
    vfn1 fn6 = (vfn1)((vfn0 *)fe->iobj->vtable)[6];
    vfn1 fn11 = (vfn1)((vfn0 *)fe->iobj->vtable)[SLOT_RUN_OR_ABORT];

    /* ESPR MODE: feed the voice's ESPR control header before the text.
     * This switches Eloquence into its extended concatenative phoneme mode,
     * so consprout carries the engine's fully-reduced phones (barred-i
     * `ix`, flapped `dx`) instead of the raw lexicon phones. The header is
     * built per-voice from the VCF in spfy_fe_set_espr_config().
     * Reverse-engineered from ConcatTTSEngine::initializeFrontEnd /
     * ::getPronunciation (SWIttsEngine.dll): fed via feedConfigA(slot5) +
     * feedConfigB(slot6), the same pair used for text. When enabled the
     * R1/R3/flap heuristic is turned off (fe_parse_set_refine(0), done in
     * set_espr_config) since the FE now does that reduction exactly.
     * (The SPFY_FE_HOST_NO_ESPR A/B switch is honoured at load in
     * set_espr_config, which leaves espr_enabled == 0.) */
    if (fe->espr_enabled) {
        feed_text(fe->iobj, fe->espr_header);                  /* slot 5 */
        fn6(fe->iobj, (uint32_t)(uintptr_t)&EMPTY_CFG);        /* slot 6 */
    }

    /* The FE wants Latin-1, not UTF-8 (see utf8_to_latin1). Convert into a
     * heap buffer sized for the worst case (input already all single-byte)
     * plus NUL; the conversion only ever shrinks or preserves length. */
    char *latin1 = (char *)malloc(strlen(text) + 1);
    if (!latin1) return NULL;
    text_to_latin1(text, latin1, strlen(text) + 1);
    feed_text(fe->iobj, latin1);
    fn6(fe->iobj, (uint32_t)(uintptr_t)&EMPTY_CFG);
    free(latin1);

    /* Drain the tagged output stream + clean chunk-seam whitespace. */
    char *tagged = drain_delegate_b(fe->iobj);
    if (!tagged) return NULL;
    fe_clean_stream_inplace(tagged);

    /* slot 11 = runOrAbort — commits any remaining synth work. */
    fn11(fe->iobj, 0);
    return tagged;
}

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
    if (!tagged) return (fe->iobj->err_flag) ? -2 : -3;

    /* 5. Build utterance struct + parse. */
    spfy_fe_utterance_t *u = (spfy_fe_utterance_t *)calloc(1, sizeof(*u));
    if (!u) { free(tagged); return -3; }
    u->hints = hints;
    parse_fe_output_into_slots(fe, tagged, hints, u);

    /* Stash the raw tagged stream on the FE so debug callers can
     * inspect it via spfy_fe_print_stats. We leak this on
     * spfy_fe_utterance_free; that's intentional during bring-up. */
    fprintf(stderr, "[fe_host] tagged output (%zu bytes): %s\n",
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
 * Stubs for the rest of the FE API
 * ============================================================ */

int spfy_fe_set_phone_names(spfy_fe_t *opaque, char *const *names,
                            uint32_t n) {
    if (!opaque) return -1;
    hosted_fe_t *fe = (hosted_fe_t *)opaque;
    /* Borrowed: spfy_voice_t owns the strings via its
     * spfy_phone_order_t, which outlives the FE. */
    fe->phone_names.names = names;
    fe->phone_names.n     = names ? n : 0u;
    return 0;
}

int spfy_fe_set_espr_config(spfy_fe_t  *opaque,
                            const char *name,
                            const char *gender,
                            const char *phoneset,
                            const char *version) {
    if (!opaque) return -1;
    hosted_fe_t *fe = (hosted_fe_t *)opaque;

    /* A/B escape hatch: SPFY_FE_HOST_NO_ESPR keeps the legacy heuristic
     * path (no ESPR header, refinement left on). Decided here at load so
     * the two paths are cleanly exclusive. */
    if (getenv("SPFY_FE_HOST_NO_ESPR")) {
        fe->espr_enabled = 0;
        return -1;
    }

    /* Fall back to the en-US Tom defaults when a field is absent. */
    if (!name || !*name)         name     = "Tom";
    if (!gender || !*gender)     gender   = "male";
    if (!phoneset || !*phoneset) phoneset = "swi_plus_ix";
    if (!version || !*version)   version  = "3.0.0.0";

    /* Build the exact header the engine feeds. FOUR real backslash bytes
     * per control token: each `\\\\` in this C literal is one byte, so
     * `\\\\\\\\` = 4 bytes. Verified by hooking the real engine's
     * feedConfigA (viz/frida_hooks/fe_feedconfig_hook.js). */
    int n = snprintf(fe->espr_header, sizeof fe->espr_header,
        "\\\\\\\\!SWIcv%s \\\\\\\\!SWIcg%s \\\\\\\\!SWIcn%s "
        "\\\\\\\\!SWIcl%s \\\\\\\\!SWIespr1 \\\\\\\\!SWIwd0",
        version, gender, name, phoneset);
    if (n < 0 || (size_t)n >= sizeof fe->espr_header) {
        fe->espr_enabled = 0;
        return -1;
    }
    fe->espr_enabled = 1;

    /* The FE now emits the engine's reduced phones directly, so the
     * built-in R1/R3/flap heuristic must not run on top of them. */
    fe_parse_set_refine(0);
    return 0;
}

int spfy_fe_set_voice_vcf(spfy_fe_t *opaque, const char *vcf_path) {
    if (!opaque || !vcf_path) return -1;
    hosted_fe_t *fe = (hosted_fe_t *)opaque;
    spfy_vcf_t vcf;
    int rc = spfy_vcf_load(vcf_path, &vcf);
    if (rc != 0) {
        fprintf(stderr, "[fe_host] spfy_vcf_load(%s) -> %d\n", vcf_path, rc);
        return rc;
    }
    memset(&fe->phoneset, 0, sizeof fe->phoneset);
    rc = spfy_phoneset_load_from_vcf(&vcf, &fe->phoneset);
    spfy_vcf_free(&vcf);
    if (rc != 0) {
        fprintf(stderr, "[fe_host] spfy_phoneset_load_from_vcf -> %d\n", rc);
        return rc;
    }
    fe->phoneset_loaded = 1;
    fprintf(stderr, "[fe_host] phoneset loaded: %u phonemes, silence=%u\n",
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

int spfy_fe_textnorm_only(const spfy_fe_t *fe, const char *text,
                          const spfy_prosody_hints_t *hints,
                          spfy_fe_delta_t *delta) {
    (void)fe; (void)text; (void)hints; (void)delta;
    return -1;
}

void spfy_fe_print_stats(const spfy_fe_t *fe) {
    if (!fe) return;
    const hosted_fe_t *h = (const hosted_fe_t *)fe;
    fprintf(stderr, "[fe_host] DLL base=%p refcount=%u init_flag=%u err_flag=%u\n",
            host_dll_image_base(h->dll),
            h->iobj ? h->iobj->refcount : 0,
            h->iobj ? h->iobj->init_flag : 0,
            h->iobj ? h->iobj->err_flag : 0);
}
