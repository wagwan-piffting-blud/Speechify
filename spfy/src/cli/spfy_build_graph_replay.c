/* spfy_build_graph_replay -- M3.4r Phase B2 validation harness.
 *
 * Loads a captured fe_tree JSONL (from fe_tree_hook.js, one event per
 * utterance), constructs our C-side BuildGraph slot tree from it, and
 * compares to the captured viterbi_dp slot count from the same text.
 * 100% match across the corpus validates that Phase B2's slot-shape
 * algorithm reproduces the engine's BuildGraph output.
 *
 * Usage:
 *   spfy_build_graph_replay <fe_tree_dir> <viterbi_dp_dir> [--verbose]
 *
 * Drives over the corpus by listing text_*.jsonl in fe_tree_dir.
 */

#include <spfy/spfy.h>

#include "../usel/build_graph.h"
#include "../usel/prsl.h"
#include "../voice/voice.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* JSONL helpers (the format is produced by our own hook so the parse */
/* is intentionally narrow).                                           */
/* ------------------------------------------------------------------ */

static const char *find_lit(const char *p, const char *end, const char *lit)
{
    size_t lp = strlen(lit);
    if ((size_t)(end - p) < lp) return NULL;
    for (const char *q = p; q + lp <= end; ++q) {
        if (memcmp(q, lit, lp) == 0) return q;
    }
    return NULL;
}

static int parse_u32_at(const char *p, uint32_t *out, const char **after)
{
    char *e = NULL;
    unsigned long v = strtoul(p, &e, 10);
    if (e == p) return -1;
    *out = (uint32_t)v;
    if (after) *after = e;
    return 0;
}

static int read_key_u32(const char *s, const char *e, const char *key,
                        uint32_t *out)
{
    char needle[64];
    snprintf(needle, sizeof needle, "\"%s\":", key);
    const char *p = find_lit(s, e, needle);
    if (!p) return -1;
    p += strlen(needle);
    return parse_u32_at(p, out, NULL);
}

/* ------------------------------------------------------------------ */
/* Parse one fe_tree event into a parsed-IR list, then build the FE   */
/* utterance struct that BuildGraph consumes.                         */
/* ------------------------------------------------------------------ */

typedef struct {
    char     rel[16];
    uint32_t ir;
    uint32_t shared;
    uint32_t next, prev, parent, daughter;
    /* Optional features extracted from feat block (B1.5). */
    char     name_str[32];   /* segment/word name; "" if absent */
    int      stress;         /* syllable stress; -1 if absent */
} parsed_ir_t;

static int parse_fe_event(const char *line, size_t n,
                          parsed_ir_t **out_irs, uint32_t *out_n)
{
    *out_irs = NULL;
    *out_n   = 0;
    const char *end = line + n;
    if (!find_lit(line, end, "\"type\":\"fe_tree\"")) return -1;

    const char *p = find_lit(line, end, "\"irs\":[");
    if (!p) return -1;
    p += strlen("\"irs\":[");

    uint32_t cap = 64;
    uint32_t cnt = 0;
    parsed_ir_t *arr = calloc(cap, sizeof *arr);
    if (!arr) return -1;

    while (p < end) {
        while (p < end && (*p == ' ' || *p == ',' || *p == '\t')) ++p;
        if (p >= end || *p == ']') break;
        if (*p != '{') break;
        /* find the closing brace at the same depth */
        const char *cur = p;
        int depth = 0;
        const char *cend = end;
        for (const char *q = p; q < end; ++q) {
            if (*q == '{') ++depth;
            else if (*q == '}') {
                --depth;
                if (depth == 0) { cend = q + 1; break; }
            }
        }
        if (cnt == cap) {
            cap *= 2;
            parsed_ir_t *na = realloc(arr, cap * sizeof *arr);
            if (!na) { free(arr); return -1; }
            arr = na;
        }
        parsed_ir_t *e = &arr[cnt];
        memset(e, 0, sizeof *e);
        e->stress = -1;

        /* relation name */
        const char *rp = find_lit(cur, cend, "\"rel\":\"");
        if (rp) {
            rp += strlen("\"rel\":\"");
            const char *rq = rp;
            while (rq < cend && *rq != '"') ++rq;
            size_t rl = (size_t)(rq - rp);
            if (rl >= sizeof e->rel) rl = sizeof e->rel - 1;
            memcpy(e->rel, rp, rl);
            e->rel[rl] = 0;
        }
        (void)read_key_u32(cur, cend, "ir",       &e->ir);
        (void)read_key_u32(cur, cend, "shared",   &e->shared);
        (void)read_key_u32(cur, cend, "next",     &e->next);
        (void)read_key_u32(cur, cend, "prev",     &e->prev);
        (void)read_key_u32(cur, cend, "parent",   &e->parent);
        (void)read_key_u32(cur, cend, "daughter", &e->daughter);

        /* Features (B1.5) */
        const char *np = find_lit(cur, cend, "\"name\":{");
        if (np) {
            const char *sp = find_lit(np, cend, "\"str\":\"");
            if (sp) {
                sp += strlen("\"str\":\"");
                const char *sq = sp;
                while (sq < cend && *sq != '"') ++sq;
                size_t sl = (size_t)(sq - sp);
                if (sl >= sizeof e->name_str) sl = sizeof e->name_str - 1;
                memcpy(e->name_str, sp, sl);
                e->name_str[sl] = 0;
            }
        }
        const char *stp = find_lit(cur, cend, "\"stress\":{");
        if (stp) {
            const char *pp = find_lit(stp, cend, "\"payload\":");
            if (pp) {
                pp += strlen("\"payload\":");
                uint32_t v = 0;
                if (parse_u32_at(pp, &v, NULL) == 0) e->stress = (int)v;
            }
        }

        ++cnt;
        p = cend;
    }
    *out_irs = arr;
    *out_n   = cnt;
    return 0;
}

/* Find an IR by relation + ir-ptr lookup. Linear scan; the corpus
 * utterance trees are small enough (a few hundred IRs at most). */
static const parsed_ir_t *find_ir(const parsed_ir_t *irs, uint32_t n,
                                  const char *rel, uint32_t ir)
{
    for (uint32_t i = 0; i < n; ++i) {
        if (ir != 0 && irs[i].ir == ir &&
            (rel == NULL || strcmp(irs[i].rel, rel) == 0)) {
            return &irs[i];
        }
    }
    return NULL;
}

/* Find the SylStructure IR that has the same shared item as `shared`. */
static const parsed_ir_t *find_ss_by_shared(const parsed_ir_t *irs,
                                            uint32_t n,
                                            uint32_t shared,
                                            int require_prev_zero)
{
    for (uint32_t i = 0; i < n; ++i) {
        if (strcmp(irs[i].rel, "SylStructure") != 0) continue;
        if (irs[i].shared != shared) continue;
        if (require_prev_zero && irs[i].prev != 0) continue;
        return &irs[i];
    }
    return NULL;
}

/* From a starting IR, walk its `next`-chain in the SylStructure
 * relation, collecting both (shared) ids AND the IR pointers
 * themselves. Caller-allocated dynamic arrays. The IR-ptr array lets
 * the caller directly access daughter pointers for each entry without
 * a follow-up shared->IR lookup (which is ambiguous for non-first
 * siblings where parent=0 on every entry). */
static int walk_ss_chain(const parsed_ir_t *irs, uint32_t n,
                         const parsed_ir_t *start,
                         uint32_t **out_shared,
                         uint32_t **out_ir,
                         uint32_t  *out_n)
{
    uint32_t cap = 8, cnt = 0;
    uint32_t *arr_s = (uint32_t *)malloc(cap * sizeof *arr_s);
    uint32_t *arr_i = (uint32_t *)malloc(cap * sizeof *arr_i);
    if (!arr_s || !arr_i) { free(arr_s); free(arr_i); return -1; }

    const parsed_ir_t *cur = start;
    while (cur != NULL) {
        if (cnt == cap) {
            cap *= 2;
            uint32_t *na = realloc(arr_s, cap * sizeof *arr_s);
            if (!na) { free(arr_s); free(arr_i); return -1; }
            arr_s = na;
            na = realloc(arr_i, cap * sizeof *arr_i);
            if (!na) { free(arr_s); free(arr_i); return -1; }
            arr_i = na;
        }
        arr_s[cnt] = cur->shared;
        arr_i[cnt] = cur->ir;
        ++cnt;
        if (cur->next == 0) break;
        cur = find_ir(irs, n, "SylStructure", cur->next);
    }
    *out_shared = arr_s;
    *out_ir     = arr_i;
    *out_n      = cnt;
    return 0;
}

/* Build the FE utterance struct from a parsed fe_tree event. */
static int build_fe_utt(const parsed_ir_t *irs, uint32_t n_irs,
                        spfy_fe_utt_t *out)
{
    memset(out, 0, sizeof *out);

    /* 1. Word relation -> word_shareds in order. */
    uint32_t word_cap = 0;
    for (uint32_t i = 0; i < n_irs; ++i)
        if (strcmp(irs[i].rel, "Word") == 0) ++word_cap;
    if (word_cap == 0) return SPFY_E_FORMAT;
    out->word_shareds = (uint32_t *)calloc(word_cap, sizeof *out->word_shareds);
    out->word_names   = (char    **)calloc(word_cap, sizeof *out->word_names);
    if (!out->word_shareds || !out->word_names) goto oom;

    /* Find the head of Word relation: prev == 0. Then walk via next. */
    const parsed_ir_t *w_head = NULL;
    for (uint32_t i = 0; i < n_irs; ++i) {
        if (strcmp(irs[i].rel, "Word") == 0 && irs[i].prev == 0) {
            w_head = &irs[i]; break;
        }
    }
    if (!w_head) {
        if (getenv("SPFY_BG_DEBUG"))
            fprintf(stderr, "  badf: no Word head\n");
        goto badf;
    }
    {
        const parsed_ir_t *cur = w_head;
        while (cur != NULL && out->n_words < word_cap) {
            out->word_shareds[out->n_words] = cur->shared;
            /* Word IRs carry a "name" feature whose `str` is the word
             * text (or "_NULL_" for boundary silence wrappers). */
            if (cur->name_str[0] != 0) {
                out->word_names[out->n_words] = strdup(cur->name_str);
                if (!out->word_names[out->n_words]) goto oom;
            } else {
                out->word_names[out->n_words] = NULL;
            }
            ++out->n_words;
            if (cur->next == 0) break;
            cur = find_ir(irs, n_irs, "Word", cur->next);
        }
    }

    /* 2. For each word, navigate to SylStructure (same shared) and
     *    walk its daughter chain to collect syllables. We track the
     *    in-tree IR ptrs alongside shared ids -- a non-first daughter
     *    has both parent=0 and an entry-by-shared lookup is ambiguous,
     *    so direct IR ptrs are required to walk the segment subchain
     *    in step 3. */
    out->word_syls    = (uint32_t **)calloc(out->n_words, sizeof *out->word_syls);
    out->word_n_syls  = (uint32_t  *)calloc(out->n_words, sizeof *out->word_n_syls);
    uint32_t **word_syl_irs   = (uint32_t **)calloc(out->n_words,
                                                    sizeof *word_syl_irs);
    if (!out->word_syls || !out->word_n_syls || !word_syl_irs) {
        free(word_syl_irs);
        goto oom;
    }

    for (uint32_t w = 0; w < out->n_words; ++w) {
        const parsed_ir_t *word_ss = find_ss_by_shared(irs, n_irs,
                                                       out->word_shareds[w],
                                                       /*require_prev_zero=*/0);
        if (!word_ss) {
            if (getenv("SPFY_BG_DEBUG"))
                fprintf(stderr, "  badf: no word_ss for w=%u shared=%u\n",
                        w, out->word_shareds[w]);
            for (uint32_t k = 0; k < out->n_words; ++k) free(word_syl_irs[k]);
            free(word_syl_irs);
            goto badf;
        }
        if (word_ss->daughter == 0) {
            out->word_syls[w]   = NULL;
            out->word_n_syls[w] = 0;
            word_syl_irs[w]     = NULL;
            continue;
        }
        const parsed_ir_t *first_syl =
            find_ir(irs, n_irs, "SylStructure", word_ss->daughter);
        if (!first_syl) {
            if (getenv("SPFY_BG_DEBUG"))
                fprintf(stderr, "  badf: no first_syl for w=%u "
                        "daughter=%u\n", w, word_ss->daughter);
            for (uint32_t k = 0; k < out->n_words; ++k) free(word_syl_irs[k]);
            free(word_syl_irs);
            goto badf;
        }
        if (walk_ss_chain(irs, n_irs, first_syl,
                          &out->word_syls[w], &word_syl_irs[w],
                          &out->word_n_syls[w]) != 0) {
            for (uint32_t k = 0; k < out->n_words; ++k) free(word_syl_irs[k]);
            free(word_syl_irs);
            goto oom;
        }
        out->n_syls += out->word_n_syls[w];
    }

    /* 3. For each global syllable, walk its daughter chain via the
     *    stored IR pointer (so we don't need shared->IR lookups for
     *    non-first daughter siblings). */
    out->syl_segs   = (uint32_t **)calloc(out->n_syls, sizeof *out->syl_segs);
    out->syl_n_segs = (uint32_t  *)calloc(out->n_syls, sizeof *out->syl_n_segs);
    if (!out->syl_segs || !out->syl_n_segs) {
        for (uint32_t k = 0; k < out->n_words; ++k) free(word_syl_irs[k]);
        free(word_syl_irs);
        goto oom;
    }

    uint32_t g = 0;
    for (uint32_t w = 0; w < out->n_words; ++w) {
        for (uint32_t s = 0; s < out->word_n_syls[w]; ++s, ++g) {
            uint32_t syl_ir_ptr = word_syl_irs[w][s];
            const parsed_ir_t *syl_ss = find_ir(irs, n_irs, "SylStructure",
                                                syl_ir_ptr);
            if (!syl_ss) {
                if (getenv("SPFY_BG_DEBUG"))
                    fprintf(stderr, "  badf: syl IR %u not found "
                            "(g=%u w=%u s=%u)\n", syl_ir_ptr, g, w, s);
                for (uint32_t k = 0; k < out->n_words; ++k)
                    free(word_syl_irs[k]);
                free(word_syl_irs);
                goto badf;
            }
            if (syl_ss->daughter == 0) {
                out->syl_segs[g]   = NULL;
                out->syl_n_segs[g] = 0;
                continue;
            }
            const parsed_ir_t *first_seg =
                find_ir(irs, n_irs, "SylStructure", syl_ss->daughter);
            if (!first_seg) {
                if (getenv("SPFY_BG_DEBUG"))
                    fprintf(stderr, "  badf: no first_seg for daughter=%u "
                            "(g=%u syl IR=%u)\n",
                            syl_ss->daughter, g, syl_ir_ptr);
                for (uint32_t k = 0; k < out->n_words; ++k)
                    free(word_syl_irs[k]);
                free(word_syl_irs);
                goto badf;
            }
            uint32_t *seg_ir_unused = NULL;
            if (walk_ss_chain(irs, n_irs, first_seg,
                              &out->syl_segs[g], &seg_ir_unused,
                              &out->syl_n_segs[g]) != 0) {
                for (uint32_t k = 0; k < out->n_words; ++k)
                    free(word_syl_irs[k]);
                free(word_syl_irs);
                goto oom;
            }
            free(seg_ir_unused);   /* segment IRs not needed yet */
            out->n_segs += out->syl_n_segs[g];
        }
    }

    /* Free the temporary IR-ptr storage. */
    for (uint32_t k = 0; k < out->n_words; ++k) free(word_syl_irs[k]);
    free(word_syl_irs);

    /* B4.3h: populate per-syllable stress + accent and phrase terminator. */
    out->syl_stress = (int32_t *)calloc(out->n_syls, sizeof *out->syl_stress);
    out->syl_accent = (uint32_t *)calloc(out->n_syls, sizeof *out->syl_accent);
    if (!out->syl_stress || !out->syl_accent) goto oom;
    for (uint32_t k = 0; k < out->n_syls; ++k) out->syl_stress[k] = -1;

    /* Phrase terminator: first char of FE Phrase relation's name. */
    out->phrase_term = '?';   /* default if not found */
    for (uint32_t i = 0; i < n_irs; ++i) {
        if (strcmp(irs[i].rel, "Phrase") == 0 && irs[i].name_str[0]) {
            out->phrase_term = irs[i].name_str[0];
            break;
        }
    }

    /* For each global syllable index, find its Syllable IR (by walking
     * the SylStructure chain) and copy its stress feature. */
    {
        uint32_t gsyl = 0;
        for (uint32_t w = 0; w < out->n_words; ++w) {
            for (uint32_t s = 0; s < out->word_n_syls[w]; ++s, ++gsyl) {
                uint32_t syl_shared = out->word_syls[w][s];
                /* Find Syllable-rel IR with this shared. */
                for (uint32_t i = 0; i < n_irs; ++i) {
                    if (strcmp(irs[i].rel, "Syllable") == 0 &&
                        irs[i].shared == syl_shared) {
                        out->syl_stress[gsyl] = irs[i].stress;
                        break;
                    }
                }
            }
        }
    }

    /* Build syl_accent: for each syllable, find Intonation root with
     * matching shared, get its daughter (Intonation tree-leaf, shared
     * with IntEvent), look up IntEvent.name and map to accent code. */
    {
        /* Map IR ptr -> parsed_ir_t* for fast lookup. */
        /* Linear scans suffice for our small corpus. */
        static const struct { const char *s; uint32_t code; } accent_map[] = {
            { "H*",   1 }, { "H+L*", 2 }, { "L*",   3 },
            { "L+H*", 4 }, { "H*+L", 5 }, { "L*+H", 6 },
            { NULL, 0 }
        };
        uint32_t gsyl = 0;
        for (uint32_t w = 0; w < out->n_words; ++w) {
            for (uint32_t s = 0; s < out->word_n_syls[w]; ++s, ++gsyl) {
                uint32_t syl_shared = out->word_syls[w][s];
                /* Find Intonation root IR (rel=Intonation, shared=syl_shared,
                 * has daughter). */
                const parsed_ir_t *intonation_root = NULL;
                for (uint32_t i = 0; i < n_irs; ++i) {
                    if (strcmp(irs[i].rel, "Intonation") == 0 &&
                        irs[i].shared == syl_shared &&
                        irs[i].daughter != 0) {
                        intonation_root = &irs[i];
                        break;
                    }
                }
                if (!intonation_root) {
                    out->syl_accent[gsyl] = 0;
                    continue;
                }
                /* Get daughter IR and read its shared. */
                const parsed_ir_t *daughter =
                    find_ir(irs, n_irs, "Intonation", intonation_root->daughter);
                if (!daughter) {
                    out->syl_accent[gsyl] = 0;
                    continue;
                }
                /* Find IntEvent IR with daughter->shared, get its name. */
                const char *acc_name = NULL;
                for (uint32_t i = 0; i < n_irs; ++i) {
                    if (strcmp(irs[i].rel, "IntEvent") == 0 &&
                        irs[i].shared == daughter->shared) {
                        acc_name = irs[i].name_str;
                        break;
                    }
                }
                uint32_t code = 0;
                if (acc_name) {
                    for (int k = 0; accent_map[k].s; ++k) {
                        if (strcmp(acc_name, accent_map[k].s) == 0) {
                            code = accent_map[k].code;
                            break;
                        }
                    }
                }
                out->syl_accent[gsyl] = code;
            }
        }
    }

    if (getenv("SPFY_BG_DEBUG")) {
        fprintf(stderr, "  done: n_words=%u n_syls=%u n_segs=%u "
                "phrase_term='%c'\n",
                out->n_words, out->n_syls, out->n_segs,
                out->phrase_term);
    }
    return SPFY_OK;

oom:
    spfy_fe_utt_free(out);
    return SPFY_E_NOMEM;
badf:
    spfy_fe_utt_free(out);
    return SPFY_E_FORMAT;
}

/* ------------------------------------------------------------------ */
/* Engine slot graph from viterbi_dp (n_slots + per-slot pred lists)   */
/* ------------------------------------------------------------------ */

typedef struct {
    uint32_t  n_slots;
    /* per_slot_preds[s] = malloced array of pred slot indices
     * (already mapped from raw slice ptrs to slot indices). */
    uint32_t **per_slot_preds;
    uint32_t  *per_slot_n_preds;
} engine_slot_graph_t;

typedef struct {
    engine_slot_graph_t *utts;
    uint32_t             n_utts;
} engine_slot_graphs_t;

static void engine_slot_graphs_free(engine_slot_graphs_t *g)
{
    if (!g || !g->utts) return;
    for (uint32_t u = 0; u < g->n_utts; ++u) {
        engine_slot_graph_t *e = &g->utts[u];
        if (e->per_slot_preds) {
            for (uint32_t s = 0; s < e->n_slots; ++s) free(e->per_slot_preds[s]);
            free(e->per_slot_preds);
        }
        free(e->per_slot_n_preds);
    }
    free(g->utts);
    memset(g, 0, sizeof *g);
}

/* Parse one viterbi_enter line into (n_slots, per-slot pred slot indices).
 * The trace stores raw slice ptrs in `preds`; we build a slice_ptr ->
 * slot_idx map then translate. */
static int parse_viterbi_enter(const char *buf, size_t n,
                               engine_slot_graph_t *out)
{
    memset(out, 0, sizeof *out);
    const char *end = buf + n;
    if (!find_lit(buf, end, "\"type\":\"viterbi_enter\"")) return -1;
    uint32_t v = 0;
    if (read_key_u32(buf, end, "n_slots", &v) != 0) return -1;
    out->n_slots = v;
    out->per_slot_preds   = (uint32_t **)calloc(v, sizeof *out->per_slot_preds);
    out->per_slot_n_preds = (uint32_t  *)calloc(v, sizeof *out->per_slot_n_preds);
    if (!out->per_slot_preds || !out->per_slot_n_preds) return -1;

    /* Walk slots[]. Build slice_ptr -> slot_idx map first. */
    const char *p = find_lit(buf, end, "\"slots\":[");
    if (!p) return -1;
    p += strlen("\"slots\":[");

    /* Two-pass: pass 1 builds slice_ptr -> slot_idx by scanning
     * "slot":N and "slice_ptr":M pairs. We allocate a hash-free
     * linear list (small N). */
    uint32_t  *sl_idx = (uint32_t *)calloc(v, sizeof *sl_idx);
    uint32_t  *sl_ptr = (uint32_t *)calloc(v, sizeof *sl_ptr);
    uint32_t **sl_raw_preds = (uint32_t **)calloc(v, sizeof *sl_raw_preds);
    uint32_t  *sl_n_preds   = (uint32_t  *)calloc(v, sizeof *sl_n_preds);
    if (!sl_idx || !sl_ptr || !sl_raw_preds || !sl_n_preds) {
        free(sl_idx); free(sl_ptr); free(sl_raw_preds); free(sl_n_preds);
        return -1;
    }

    const char *q = p;
    uint32_t k = 0;
    while (q < end && k < v) {
        while (q < end && (*q == ' ' || *q == ',' || *q == '\t')) ++q;
        if (q >= end || *q == ']') break;
        if (*q != '{') break;
        /* find balanced } */
        const char *qend = end;
        int depth = 0;
        for (const char *r = q; r < end; ++r) {
            if (*r == '{') ++depth;
            else if (*r == '}') {
                --depth;
                if (depth == 0) { qend = r + 1; break; }
            }
        }
        uint32_t slot_idx = 0, slice_ptr = 0, n_preds = 0;
        (void)read_key_u32(q, qend, "slot",      &slot_idx);
        (void)read_key_u32(q, qend, "slice_ptr", &slice_ptr);
        (void)read_key_u32(q, qend, "n_preds",   &n_preds);
        sl_idx[k] = slot_idx;
        sl_ptr[k] = slice_ptr;
        sl_n_preds[k] = n_preds;
        if (n_preds > 0) {
            sl_raw_preds[k] = (uint32_t *)calloc(n_preds, sizeof **sl_raw_preds);
            if (!sl_raw_preds[k]) {
                free(sl_idx); free(sl_ptr);
                for (uint32_t z = 0; z < k; ++z) free(sl_raw_preds[z]);
                free(sl_raw_preds); free(sl_n_preds);
                return -1;
            }
            const char *pp = find_lit(q, qend, "\"preds\":[");
            if (pp) {
                pp += strlen("\"preds\":[");
                for (uint32_t z = 0; z < n_preds; ++z) {
                    while (pp < qend && (*pp == ' ' || *pp == ',')) ++pp;
                    if (pp >= qend || *pp == ']') break;
                    uint32_t rv = 0;
                    if (parse_u32_at(pp, &rv, &pp) != 0) break;
                    sl_raw_preds[k][z] = rv;
                }
            }
        }
        ++k;
        q = qend;
    }

    /* Map raw slice_ptrs to slot indices using sl_ptr. Then store in
     * out->per_slot_preds indexed by slot_idx. */
    for (uint32_t i = 0; i < k; ++i) {
        uint32_t si = sl_idx[i];
        if (si >= v) continue;
        out->per_slot_n_preds[si] = sl_n_preds[i];
        if (sl_n_preds[i] > 0) {
            out->per_slot_preds[si] = (uint32_t *)
                calloc(sl_n_preds[i], sizeof **out->per_slot_preds);
            if (!out->per_slot_preds[si]) {
                /* nominal */
                continue;
            }
            for (uint32_t j = 0; j < sl_n_preds[i]; ++j) {
                uint32_t raw = sl_raw_preds[i][j];
                /* find slot whose slice_ptr matches raw */
                uint32_t mapped = UINT32_MAX;
                for (uint32_t z = 0; z < k; ++z) {
                    if (sl_ptr[z] == raw) { mapped = sl_idx[z]; break; }
                }
                out->per_slot_preds[si][j] = mapped;
            }
        }
    }

    for (uint32_t z = 0; z < k; ++z) free(sl_raw_preds[z]);
    free(sl_idx); free(sl_ptr); free(sl_raw_preds); free(sl_n_preds);
    return 0;
}

static int load_engine_slot_graphs(const char *path,
                                   engine_slot_graphs_t *out)
{
    memset(out, 0, sizeof *out);
    FILE *fp = fopen(path, "rb");
    if (!fp) return -1;
    char *buf = malloc(1 << 20);
    if (!buf) { fclose(fp); return -1; }
    uint32_t cap = 8, cnt = 0;
    out->utts = (engine_slot_graph_t *)calloc(cap, sizeof *out->utts);
    if (!out->utts) { free(buf); fclose(fp); return -1; }
    while (fgets(buf, 1 << 20, fp)) {
        size_t n = strlen(buf);
        if (!find_lit(buf, buf + n, "\"type\":\"viterbi_enter\"")) continue;
        if (cnt == cap) {
            cap *= 2;
            engine_slot_graph_t *na = realloc(out->utts, cap * sizeof *na);
            if (!na) break;
            out->utts = na;
        }
        if (parse_viterbi_enter(buf, n, &out->utts[cnt]) == 0) ++cnt;
    }
    out->n_utts = cnt;
    free(buf);
    fclose(fp);
    return 0;
}

/* ------------------------------------------------------------------ */
/* prsl_slot ctx loader                                                */
/* ------------------------------------------------------------------ */
/* Each line: {"type":"prsl_slot","n":N,"slot":S,...,"ctx":[a,b,c,d,e],...} */

typedef struct {
    uint32_t (*ctx_per_hp)[5];   /* heap, n_halfphones in utt order */
    uint32_t **uids_per_hp;      /* heap, n_halfphones arrays */
    uint32_t  *n_uids_per_hp;    /* heap, n_halfphones */
    uint32_t  n_halfphones;
} engine_prsl_utt_t;

typedef struct {
    engine_prsl_utt_t *utts;
    uint32_t           n_utts;
} engine_prsl_t;

static void engine_prsl_free(engine_prsl_t *p)
{
    if (!p || !p->utts) return;
    for (uint32_t u = 0; u < p->n_utts; ++u) {
        free(p->utts[u].ctx_per_hp);
        if (p->utts[u].uids_per_hp) {
            for (uint32_t k = 0; k < p->utts[u].n_halfphones; ++k)
                free(p->utts[u].uids_per_hp[k]);
            free(p->utts[u].uids_per_hp);
        }
        free(p->utts[u].n_uids_per_hp);
    }
    free(p->utts);
    memset(p, 0, sizeof *p);
}

static int load_engine_prsl(const char *path, engine_prsl_t *out)
{
    memset(out, 0, sizeof *out);
    FILE *fp = fopen(path, "rb");
    if (!fp) return -1;
    char buf[4096];
    /* Two-pass: pass 1 sizes utts/halfphones; pass 2 fills ctx. We
     * use a simpler one-pass with realloc. Slot resets to 0 mark utt
     * boundaries. */
    uint32_t utt_cap = 4, utt_n = 0;
    out->utts = calloc(utt_cap, sizeof *out->utts);
    if (!out->utts) { fclose(fp); return -1; }
    int prev_slot = -1;
    uint32_t hp_cap = 0;
    engine_prsl_utt_t *cur = NULL;
    while (fgets(buf, sizeof buf, fp)) {
        size_t n = strlen(buf);
        const char *end = buf + n;
        if (!find_lit(buf, end, "\"type\":\"prsl_slot\"")) continue;
        uint32_t slot = 0;
        if (read_key_u32(buf, end, "slot", &slot) != 0) continue;
        if ((int)slot == 0 && prev_slot > 0) {
            /* New utterance. */
            if (utt_n == utt_cap) {
                utt_cap *= 2;
                engine_prsl_utt_t *na = realloc(out->utts, utt_cap * sizeof *na);
                if (!na) break;
                out->utts = na;
            }
            cur = NULL;
            hp_cap = 0;
        }
        if (cur == NULL) {
            cur = &out->utts[utt_n++];
            memset(cur, 0, sizeof *cur);
            hp_cap = 16;
            cur->ctx_per_hp     = calloc(hp_cap, sizeof *cur->ctx_per_hp);
            cur->uids_per_hp    = calloc(hp_cap, sizeof *cur->uids_per_hp);
            cur->n_uids_per_hp  = calloc(hp_cap, sizeof *cur->n_uids_per_hp);
            if (!cur->ctx_per_hp || !cur->uids_per_hp || !cur->n_uids_per_hp) break;
        }
        if (slot >= hp_cap) {
            uint32_t nc = hp_cap * 2;
            while (slot >= nc) nc *= 2;
            uint32_t (*na)[5] = realloc(cur->ctx_per_hp, nc * sizeof *na);
            if (!na) break;
            for (uint32_t k = hp_cap; k < nc; ++k) memset(na[k], 0, sizeof na[k]);
            cur->ctx_per_hp = na;
            uint32_t **nu = realloc(cur->uids_per_hp, nc * sizeof *nu);
            if (!nu) break;
            for (uint32_t k = hp_cap; k < nc; ++k) nu[k] = NULL;
            cur->uids_per_hp = nu;
            uint32_t  *nn2 = realloc(cur->n_uids_per_hp, nc * sizeof *nn2);
            if (!nn2) break;
            for (uint32_t k = hp_cap; k < nc; ++k) nn2[k] = 0;
            cur->n_uids_per_hp = nn2;
            hp_cap = nc;
        }
        const char *cp = find_lit(buf, end, "\"ctx\":[");
        if (cp) {
            cp += strlen("\"ctx\":[");
            for (int k = 0; k < 5; ++k) {
                while (cp < end && (*cp == ' ' || *cp == ',')) ++cp;
                if (cp >= end || *cp == ']') break;
                uint32_t v = 0;
                if (parse_u32_at(cp, &v, &cp) != 0) break;
                cur->ctx_per_hp[slot][k] = v;
            }
        }
        /* Parse uids[]. */
        uint32_t n_cands = 0;
        if (read_key_u32(buf, end, "n_cands", &n_cands) == 0 && n_cands > 0) {
            uint32_t *arr = calloc(n_cands, sizeof *arr);
            if (arr) {
                const char *up = find_lit(buf, end, "\"uids\":[");
                if (up) {
                    up += strlen("\"uids\":[");
                    for (uint32_t k = 0; k < n_cands; ++k) {
                        while (up < end && (*up == ' ' || *up == ',')) ++up;
                        if (up >= end || *up == ']') break;
                        uint32_t v = 0;
                        if (parse_u32_at(up, &v, &up) != 0) break;
                        arr[k] = v;
                    }
                }
                cur->uids_per_hp[slot] = arr;
                cur->n_uids_per_hp[slot] = n_cands;
            }
        }
        if (slot + 1 > cur->n_halfphones) cur->n_halfphones = slot + 1;
        prev_slot = (int)slot;
    }
    out->n_utts = utt_n;
    fclose(fp);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Per-text validation                                                 */
/* ------------------------------------------------------------------ */

typedef struct {
    int      utts_run;
    int      utts_count_match;
    int      utts_pred_match;
    int      slots_pred_total;
    int      slots_pred_match;
    /* Phase B4 step 1 metrics: slice ctx derivation. */
    int      utts_ctx_match;
    int      slots_ctx_total;
    int      slots_ctx_match;
    int      utts_prsl_match;
    int      slots_prsl_total;
    int      slots_prsl_match;
    /* Phase B4 step 3h metrics: SP_target populator validation. */
    int      slots_sp_total;
    int      slots_sp_match;
    /* Per-field counters (sp[0..4]). */
    int      sp_field_match[5];
} stats_t;

/* Per-utterance SP target capture from inner_scorer JSONL. */
typedef struct {
    /* per_slot_sp[s][k] = SP_target[k] for slot s. NULL if not captured. */
    uint32_t (**per_slot_sp)[5];   /* malloc'd ptr per utt to per-slot 5-tuple */
    uint32_t  *per_utt_n_slots;
    uint32_t   n_utts;
} inner_scorer_caps_t;

static void inner_scorer_caps_free(inner_scorer_caps_t *c)
{
    if (!c) return;
    for (uint32_t u = 0; u < c->n_utts; ++u) free(c->per_slot_sp[u]);
    free(c->per_slot_sp);
    free(c->per_utt_n_slots);
    memset(c, 0, sizeof *c);
}

static int load_inner_scorer(const char *path, inner_scorer_caps_t *out)
{
    memset(out, 0, sizeof *out);
    FILE *fp = fopen(path, "r");
    if (!fp) return -1;

    /* First pass: count utts (slot=0 boundaries) and per-utt slot counts. */
    uint32_t cap_utts = 8;
    uint32_t (**utts)[5] = calloc(cap_utts, sizeof *utts);
    uint32_t *utt_n     = calloc(cap_utts, sizeof *utt_n);
    if (!utts || !utt_n) { free(utts); free(utt_n); fclose(fp); return -1; }
    uint32_t n_utts = 0;

    uint32_t (*cur_slots)[5] = NULL;
    uint32_t cur_cap = 0, cur_n = 0;

    char *buf = NULL;
    size_t bufcap = 0;
    ssize_t nl;
    /* getline is POSIX; on Windows we read in chunks. */
    char tmp[8192];
    (void)bufcap; (void)nl;
    (void)buf;

    while (fgets(tmp, sizeof tmp, fp)) {
        const char *line = tmp;
        const char *end  = tmp + strlen(tmp);
        if (!find_lit(line, end, "\"type\":\"inner_scorer\"")) continue;
        uint32_t slot = UINT32_MAX;
        if (read_key_u32(line, end, "slot", &slot) != 0) continue;

        /* New utt boundary: slot=0 starts a new utt. */
        if (slot == 0 && cur_n > 0) {
            if (n_utts == cap_utts) {
                cap_utts *= 2;
                utts  = realloc(utts,  cap_utts * sizeof *utts);
                utt_n = realloc(utt_n, cap_utts * sizeof *utt_n);
                if (!utts || !utt_n) goto loadfail;
            }
            utts[n_utts]  = cur_slots;
            utt_n[n_utts] = cur_n;
            ++n_utts;
            cur_slots = NULL; cur_cap = 0; cur_n = 0;
        }

        /* Parse sp_target [a,b,c,d,e] */
        const char *sp_p = find_lit(line, end, "\"sp_target\":[");
        if (!sp_p) continue;
        sp_p += strlen("\"sp_target\":[");
        uint32_t vals[5] = {0,0,0,0,0};
        const char *q = sp_p;
        for (int k = 0; k < 5; ++k) {
            while (q < end && (*q == ' ' || *q == ',')) ++q;
            const char *after = NULL;
            uint32_t v = 0;
            if (parse_u32_at(q, &v, &after) != 0) break;
            vals[k] = v;
            q = after;
        }

        if (slot >= cur_cap) {
            uint32_t nc = cur_cap ? cur_cap * 2 : 16;
            while (nc <= slot) nc *= 2;
            cur_slots = realloc(cur_slots, nc * sizeof *cur_slots);
            if (!cur_slots) goto loadfail;
            for (uint32_t k = cur_cap; k < nc; ++k)
                memset(&cur_slots[k], 0, sizeof cur_slots[k]);
            cur_cap = nc;
        }
        for (int k = 0; k < 5; ++k) cur_slots[slot][k] = vals[k];
        if (slot + 1 > cur_n) cur_n = slot + 1;
    }
    if (cur_n > 0) {
        if (n_utts == cap_utts) {
            cap_utts *= 2;
            utts  = realloc(utts,  cap_utts * sizeof *utts);
            utt_n = realloc(utt_n, cap_utts * sizeof *utt_n);
            if (!utts || !utt_n) goto loadfail;
        }
        utts[n_utts]  = cur_slots;
        utt_n[n_utts] = cur_n;
        ++n_utts;
    }

    out->per_slot_sp     = utts;
    out->per_utt_n_slots = utt_n;
    out->n_utts          = n_utts;
    fclose(fp);
    return 0;

loadfail:
    free(cur_slots);
    for (uint32_t u = 0; u < n_utts; ++u) free(utts[u]);
    free(utts); free(utt_n);
    fclose(fp);
    return -1;
}

static void process_entry(const char *fe_path, const char *vit_path,
                          const char *prsl_path,
                          const char *is_path,
                          const spfy_prsl_t *prsl_voice,
                          stats_t *st, int verbose)
{
    /* Extract entry id for nice printing. */
    const char *base = fe_path;
    for (const char *q = fe_path; *q; ++q)
        if (*q == '/' || *q == '\\') base = q + 1;
    char eid[64] = {0};
    snprintf(eid, sizeof eid, "%s", base);
    char *dot = strrchr(eid, '.');
    if (dot) *dot = 0;

    /* Read engine slot graphs (n_slots + per-slot pred lists). */
    engine_slot_graphs_t eng = {0};
    if (load_engine_slot_graphs(vit_path, &eng) != 0) {
        fprintf(stderr, "warn: cannot read viterbi_dp at %s\n", vit_path);
        return;
    }

    /* Optional: prsl_slot ctx for ctx derivation comparison. */
    engine_prsl_t prsl = {0};
    int have_prsl = (prsl_path && load_engine_prsl(prsl_path, &prsl) == 0);

    /* Optional: inner_scorer captures for SP_target validation. */
    inner_scorer_caps_t is_caps = {0};
    int have_is = (is_path && load_inner_scorer(is_path, &is_caps) == 0);

    /* Open fe_tree and parse one event at a time. */
    FILE *fp = fopen(fe_path, "rb");
    if (!fp) {
        fprintf(stderr, "could not open %s\n", fe_path);
        if (have_is) inner_scorer_caps_free(&is_caps);
        return;
    }
    char *buf = malloc(1 << 22);   /* 4 MiB scratch -- fe_tree lines can be big */
    if (!buf) { fclose(fp); if (have_is) inner_scorer_caps_free(&is_caps); return; }
    uint32_t utt_idx = 0;
    while (fgets(buf, 1 << 22, fp)) {
        size_t n = strlen(buf);
        if (!find_lit(buf, buf + n, "\"type\":\"fe_tree\"")) continue;

        parsed_ir_t *irs = NULL;
        uint32_t n_irs = 0;
        if (parse_fe_event(buf, n, &irs, &n_irs) != 0) {
            fprintf(stderr, "  %s utt %u: parse_fe_event failed\n",
                    eid, utt_idx + 1);
            continue;
        }
        if (getenv("SPFY_BG_DEBUG") && utt_idx == 0) {
            fprintf(stderr, "DEBUG %s utt 1: n_irs=%u\n", eid, n_irs);
            uint32_t per_rel[16] = {0};
            const char *names[] = {"Word","Syllable","Segment","Phrase",
                                   "SylStructure","Intonation","IntEvent",
                                   "Target","WordStructure"};
            for (uint32_t i = 0; i < n_irs; ++i) {
                for (size_t k = 0; k < sizeof names/sizeof names[0]; ++k) {
                    if (strcmp(irs[i].rel, names[k]) == 0) {
                        per_rel[k]++; break;
                    }
                }
            }
            for (size_t k = 0; k < sizeof names/sizeof names[0]; ++k) {
                if (per_rel[k]) fprintf(stderr, "  %s: %u\n", names[k], per_rel[k]);
            }
        }
        spfy_fe_utt_t fe = {0};
        int rc = build_fe_utt(irs, n_irs, &fe);
        if (getenv("SPFY_BG_DEBUG") && utt_idx == 0 && rc == SPFY_OK) {
            fprintf(stderr, "  fe.n_words=%u fe.n_syls=%u fe.n_segs=%u\n",
                    fe.n_words, fe.n_syls, fe.n_segs);
            for (uint32_t w = 0; w < fe.n_words; ++w) {
                fprintf(stderr, "    word[%u] shared=%u n_syls=%u\n",
                        w, fe.word_shareds[w], fe.word_n_syls[w]);
            }
        }
        if (rc != SPFY_OK) {
            fprintf(stderr, "  %s utt %u: build_fe_utt rc=%d (%s)\n",
                    eid, utt_idx + 1, rc, spfy_strerror(rc));
            free(irs);
            continue;
        }
        spfy_slot_tree_t tree = {0};
        rc = spfy_build_graph(&fe, &tree);
        if (rc != SPFY_OK) {
            fprintf(stderr, "  %s utt %u: build_graph rc=%d (%s)\n",
                    eid, utt_idx + 1, rc, spfy_strerror(rc));
            spfy_fe_utt_free(&fe);
            free(irs);
            continue;
        }

        const engine_slot_graph_t *e = (utt_idx < eng.n_utts)
                                       ? &eng.utts[utt_idx] : NULL;
        uint32_t eng_n = e ? e->n_slots : 0;
        int count_match = (eng_n == tree.n_slots);

        /* Compute LinkGraph predecs and compare to engine's. */
        spfy_slot_preds_table_t preds = {0};
        int pred_match_full = 0;
        int slots_match = 0, slots_total = 0;
        if (count_match) {
            int rc2 = spfy_link_graph(&tree, &preds);
            if (rc2 == SPFY_OK && e) {
                pred_match_full = 1;
                for (uint32_t s = 0; s < tree.n_slots; ++s) {
                    slots_total++;
                    uint32_t our_n = preds.per_slot[s].n_preds;
                    uint32_t eng_np = e->per_slot_n_preds[s];
                    int match_s = 1;
                    if (our_n != eng_np) {
                        match_s = 0;
                    } else {
                        for (uint32_t k = 0; k < our_n; ++k) {
                            if (preds.per_slot[s].preds[k] !=
                                e->per_slot_preds[s][k]) {
                                match_s = 0; break;
                            }
                        }
                    }
                    if (match_s) slots_match++;
                    else pred_match_full = 0;
                }
            }
        }
        /* Phase B4 steps 1-2: derive ctx[5] + PRSL pool per halfphone-
         * leaf and compare to captured prsl_slot.ctx + uids. Need a
         * flat array of segment names in utterance order. */
        int ctx_match_full = 0;
        int ctx_match_n = 0, ctx_total_n = 0;
        int prsl_match_full = 0;
        int prsl_match_n = 0, prsl_total_n = 0;
        if (have_prsl && utt_idx < prsl.n_utts) {
            const engine_prsl_utt_t *p = &prsl.utts[utt_idx];
            /* Build segment-name array in utterance order from fe. */
            const char **seg_names = (const char **)
                calloc(fe.n_segs ? fe.n_segs : 1, sizeof *seg_names);
            if (seg_names) {
                uint32_t si = 0;
                for (uint32_t w = 0; w < fe.n_words; ++w) {
                    for (uint32_t s = 0; s < fe.word_n_syls[w]; ++s) {
                        uint32_t g = 0;
                        for (uint32_t ww = 0; ww < w; ++ww)
                            g += fe.word_n_syls[ww];
                        g += s;
                        for (uint32_t k = 0; k < fe.syl_n_segs[g]; ++k) {
                            uint32_t shared = fe.syl_segs[g][k];
                            const char *name = "";
                            for (uint32_t i = 0; i < n_irs; ++i) {
                                if (strcmp(irs[i].rel, "Segment") == 0 &&
                                    irs[i].shared == shared) {
                                    name = irs[i].name_str; break;
                                }
                            }
                            seg_names[si++] = name;
                        }
                    }
                }
                spfy_slice_ctx_table_t ctx_table = {0};
                /* NULL inventory -> Tom's hardcoded table. This harness
                 * replays captured Tom traces only. */
                int rc3 = spfy_derive_slice_ctx(&tree, seg_names,
                                                fe.n_segs, NULL, 0,
                                                &ctx_table);
                if (rc3 == SPFY_OK) {
                    ctx_match_full = 1;
                    if (prsl_voice) prsl_match_full = 1;
                    /* Compare halfphone-leaf slots in post-order (=
                     * prsl_slot order) to engine's ctx_per_hp + uids. */
                    uint32_t hp_idx = 0;
                    for (uint32_t s = 0; s < tree.n_slots; ++s) {
                        if (tree.slots[s].kind != SPFY_SK_HALFPHONE) continue;
                        if (hp_idx >= p->n_halfphones) break;
                        ctx_total_n++;
                        if (!ctx_table.has[s]) {
                            ctx_match_full = 0;
                            prsl_match_full = 0;
                            ++hp_idx;
                            continue;
                        }
                        /* ctx comparison */
                        int ok = 1;
                        for (int k = 0; k < 5; ++k) {
                            if (ctx_table.ctx[s][k] !=
                                p->ctx_per_hp[hp_idx][k]) { ok = 0; break; }
                        }
                        if (ok) ctx_match_n++;
                        else ctx_match_full = 0;
                        /* PRSL pool comparison */
                        if (prsl_voice) {
                            prsl_total_n++;
                            uint32_t left  = ctx_table.ctx[s][1];
                            uint32_t cent  = ctx_table.ctx[s][2];
                            uint32_t right = ctx_table.ctx[s][3];
                            uint32_t key = spfy_prsl_context_key(left, cent, right);
                            const uint32_t *cands = NULL;
                            uint32_t n_cands = 0;
                            int rc4 = spfy_prsl_lookup(prsl_voice, key,
                                                       &cands, &n_cands);
                            int pook = 1;
                            if (rc4 != SPFY_OK) pook = 0;
                            else if (n_cands != p->n_uids_per_hp[hp_idx]) pook = 0;
                            else {
                                for (uint32_t k = 0; k < n_cands; ++k) {
                                    if (cands[k] != p->uids_per_hp[hp_idx][k]) {
                                        pook = 0; break;
                                    }
                                }
                            }
                            if (pook) prsl_match_n++;
                            else prsl_match_full = 0;
                        }
                        ++hp_idx;
                    }
                }
                spfy_slice_ctx_table_free(&ctx_table);
                free(seg_names);
            }
        }

        printf("  %-12s utt %u: slots ours=%-3u eng=%-3u  %s  "
               "preds %d/%d  ctx %d/%d  %s\n",
               eid, utt_idx + 1, tree.n_slots, eng_n,
               count_match ? "CNT-OK " : "CNT-DIF",
               slots_match, slots_total,
               ctx_match_n, ctx_total_n,
               (count_match && pred_match_full && ctx_match_full)
                 ? "MATCH"
                 : (count_match ? "DIFF" : "skip"));
        if (verbose && count_match && !pred_match_full && e) {
            printf("    pred mismatches:\n");
            for (uint32_t s = 0; s < tree.n_slots && s < 64; ++s) {
                uint32_t our_n = preds.per_slot[s].n_preds;
                uint32_t eng_np = e->per_slot_n_preds[s];
                if (our_n == eng_np) {
                    int m = 1;
                    for (uint32_t k = 0; k < our_n; ++k)
                        if (preds.per_slot[s].preds[k]
                            != e->per_slot_preds[s][k]) { m = 0; break; }
                    if (m) continue;
                }
                printf("      slot %u: ours=[", s);
                for (uint32_t k = 0; k < our_n; ++k)
                    printf("%s%u", k ? "," : "",
                           preds.per_slot[s].preds[k]);
                printf("]  eng=[");
                for (uint32_t k = 0; k < eng_np; ++k)
                    printf("%s%u", k ? "," : "",
                           e->per_slot_preds[s][k]);
                printf("]\n");
            }
        }
        /* Phase B4 step 3h: SP_target derivation + validation. */
        if (have_is && utt_idx < is_caps.n_utts && count_match) {
            spfy_sp_target_table_t spt = {0};
            int rc4 = spfy_derive_sp_targets(&tree, &fe,
                                             utt_idx, /* sentence_idx */
                                             0,       /* voice_d4_flag */
                                             &spt);
            if (rc4 == SPFY_OK) {
                uint32_t (*cap)[5] = is_caps.per_slot_sp[utt_idx];
                uint32_t cap_n     = is_caps.per_utt_n_slots[utt_idx];
                int debug = (getenv("SPFY_SP_DEBUG") != NULL);
                if (debug) {
                    fprintf(stderr, "  %s utt %u sp_targets (n_slots=%u "
                            "cap_n=%u phrase_term='%c'):\n",
                            eid, utt_idx + 1, tree.n_slots, cap_n,
                            fe.phrase_term);
                    fprintf(stderr, "    syl_stress:");
                    for (uint32_t k = 0; k < fe.n_syls; ++k)
                        fprintf(stderr, " %d", fe.syl_stress[k]);
                    fprintf(stderr, "\n    syl_accent:");
                    for (uint32_t k = 0; k < fe.n_syls; ++k)
                        fprintf(stderr, " %u", fe.syl_accent[k]);
                    fprintf(stderr, "\n");
                }
                /* Engine InnerScorer indexes halfphones contiguously
                 * (0..n_halfphone-1), not by post-order tree slot. Map
                 * by iterating tree halfphone leaves in post-order. */
                uint32_t hp_idx = 0;
                for (uint32_t s = 0; s < tree.n_slots; ++s) {
                    if (tree.slots[s].kind != SPFY_SK_HALFPHONE) continue;
                    if (hp_idx >= cap_n) break;
                    if (!spt.has[s]) { ++hp_idx; continue; }
                    st->slots_sp_total++;
                    int slot_match = 1;
                    for (int k = 0; k < 5; ++k) {
                        if (spt.sp[s][k] == cap[hp_idx][k]) {
                            st->sp_field_match[k]++;
                        } else {
                            slot_match = 0;
                        }
                    }
                    if (slot_match) st->slots_sp_match++;
                    if (debug && !slot_match && st->slots_sp_total < 30) {
                        fprintf(stderr, "    hp%u (slot %u): "
                                "ours=[%u %u %u %u %u] "
                                "eng=[%u %u %u %u %u]\n", hp_idx, s,
                                spt.sp[s][0], spt.sp[s][1], spt.sp[s][2],
                                spt.sp[s][3], spt.sp[s][4],
                                cap[hp_idx][0], cap[hp_idx][1], cap[hp_idx][2],
                                cap[hp_idx][3], cap[hp_idx][4]);
                    }
                    ++hp_idx;
                }
            }
            spfy_sp_target_table_free(&spt);
        }

        st->utts_run++;
        if (count_match) st->utts_count_match++;
        if (count_match && pred_match_full) st->utts_pred_match++;
        if (count_match && ctx_match_full) st->utts_ctx_match++;
        if (count_match && prsl_match_full) st->utts_prsl_match++;
        st->slots_pred_total += slots_total;
        st->slots_pred_match += slots_match;
        st->slots_ctx_total += ctx_total_n;
        st->slots_ctx_match += ctx_match_n;
        st->slots_prsl_total += prsl_total_n;
        st->slots_prsl_match += prsl_match_n;

        spfy_slot_preds_table_free(&preds);
        spfy_slot_tree_free(&tree);
        spfy_fe_utt_free(&fe);
        free(irs);
        ++utt_idx;
    }
    free(buf);
    fclose(fp);
    engine_slot_graphs_free(&eng);
    if (have_prsl) engine_prsl_free(&prsl);
    if (have_is) inner_scorer_caps_free(&is_caps);
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr,
                "usage: spfy_build_graph_replay <fe_tree_dir> "
                "<viterbi_dp_dir> [<prsl_slot_dir>] [--vin <voice.vin>] "
                "[--verbose]\n");
        return 2;
    }
    int verbose = 0;
    const char *vin_path = NULL;
    const char *is_dir = NULL;
    int positional[3] = {0, 0, 0};
    int pos_n = 0;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--verbose") == 0) verbose = 1;
        else if (strcmp(argv[i], "--vin") == 0 && i + 1 < argc) {
            vin_path = argv[++i];
        }
        else if (strcmp(argv[i], "--inner-scorer") == 0 && i + 1 < argc) {
            is_dir = argv[++i];
        }
        else if (argv[i][0] != '-' && pos_n < 3) {
            positional[pos_n++] = i;
        }
    }
    if (pos_n < 2) {
        fprintf(stderr, "missing required dirs\n");
        return 2;
    }
    const char *fe_dir   = argv[positional[0]];
    const char *vit_dir  = argv[positional[1]];
    const char *prsl_dir = (pos_n >= 3) ? argv[positional[2]] : NULL;

    /* Optional: load voice's PRSL chunk for pool comparison. */
    spfy_vin_t vin = {0};
    spfy_prsl_t prsl_voice = {0};
    int have_voice_prsl = 0;
    if (vin_path) {
        if (spfy_vin_load(vin_path, &vin) == SPFY_OK &&
            spfy_prsl_load(&vin, &prsl_voice) == SPFY_OK) {
            have_voice_prsl = 1;
            fprintf(stderr, "loaded prsl from %s\n", vin_path);
        } else {
            fprintf(stderr, "warn: could not load PRSL from %s\n", vin_path);
        }
    }

    stats_t st = {0};
    /* Drive over text_*.jsonl in fe_dir. We don't have a portable
     * directory iterator; rely on the runner script to enumerate. For
     * now we accept individual files via the wrapper -- if a directory
     * is given we look for text_001..text_030 and any spr_*. */
    static const char *known_ids[] = {
        "text_001","text_002","text_003","text_004","text_005",
        "text_006","text_007","text_008","text_009","text_010",
        "text_011","text_012","text_013","text_014","text_015",
        "text_016","text_017","text_018","text_019","text_020",
        "text_021","text_022","text_023","text_024","text_025",
        "text_026","text_027","text_028","text_029","text_030",
    };
    char fe_path[1024], vit_path[1024], prsl_path[1024], is_path[1024];
    for (size_t i = 0; i < sizeof known_ids / sizeof known_ids[0]; ++i) {
        snprintf(fe_path,  sizeof fe_path,  "%s/%s.jsonl", fe_dir,
                 known_ids[i]);
        snprintf(vit_path, sizeof vit_path, "%s/%s.jsonl", vit_dir,
                 known_ids[i]);
        const char *pp = NULL;
        if (prsl_dir) {
            snprintf(prsl_path, sizeof prsl_path, "%s/%s.jsonl", prsl_dir,
                     known_ids[i]);
            pp = prsl_path;
        }
        const char *ipp = NULL;
        if (is_dir) {
            snprintf(is_path, sizeof is_path, "%s/%s.jsonl", is_dir,
                     known_ids[i]);
            ipp = is_path;
        }
        process_entry(fe_path, vit_path, pp, ipp,
                      have_voice_prsl ? &prsl_voice : NULL,
                      &st, verbose);
    }
    printf("\n----- aggregate -----\n");
    printf("utterances slot-count match: %d / %d  (%.2f%%)\n",
           st.utts_count_match, st.utts_run,
           100.0 * st.utts_count_match / (st.utts_run > 0 ? st.utts_run : 1));
    printf("utterances pred-list match:  %d / %d  (%.2f%%)\n",
           st.utts_pred_match, st.utts_run,
           100.0 * st.utts_pred_match / (st.utts_run > 0 ? st.utts_run : 1));
    printf("slot-level pred match:       %d / %d  (%.2f%%)\n",
           st.slots_pred_match, st.slots_pred_total,
           100.0 * st.slots_pred_match /
           (st.slots_pred_total > 0 ? st.slots_pred_total : 1));
    if (prsl_dir) {
        printf("utterances ctx match:        %d / %d  (%.2f%%)\n",
               st.utts_ctx_match, st.utts_run,
               100.0 * st.utts_ctx_match / (st.utts_run > 0 ? st.utts_run : 1));
        printf("halfphone-slot ctx match:    %d / %d  (%.2f%%)\n",
               st.slots_ctx_match, st.slots_ctx_total,
               100.0 * st.slots_ctx_match /
               (st.slots_ctx_total > 0 ? st.slots_ctx_total : 1));
        if (have_voice_prsl) {
            printf("utterances PRSL pool match:  %d / %d  (%.2f%%)\n",
                   st.utts_prsl_match, st.utts_run,
                   100.0 * st.utts_prsl_match / (st.utts_run > 0 ? st.utts_run : 1));
            printf("halfphone PRSL pool match:   %d / %d  (%.2f%%)\n",
                   st.slots_prsl_match, st.slots_prsl_total,
                   100.0 * st.slots_prsl_match /
                   (st.slots_prsl_total > 0 ? st.slots_prsl_total : 1));
        }
    }
    if (is_dir) {
        printf("halfphone SP_target match:   %d / %d  (%.2f%%)\n",
               st.slots_sp_match, st.slots_sp_total,
               100.0 * st.slots_sp_match /
               (st.slots_sp_total > 0 ? st.slots_sp_total : 1));
        const char *fnames[5] = {"sylInPhrase","sylType","sylInWord",
                                 "wordInPhrase","phoneInSyl"};
        for (int k = 0; k < 5; ++k) {
            printf("  sp[%d] %-13s match: %d / %d  (%.2f%%)\n",
                   k, fnames[k], st.sp_field_match[k], st.slots_sp_total,
                   100.0 * st.sp_field_match[k] /
                   (st.slots_sp_total > 0 ? st.slots_sp_total : 1));
        }
    }
    if (have_voice_prsl) spfy_prsl_free(&prsl_voice);
    if (vin_path) spfy_vin_free(&vin);
    return 0;
}
