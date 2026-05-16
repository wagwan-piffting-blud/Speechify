/* Stage 1: Text normalisation.
 *
 * Builds %text / %token / %word / %phrase streams from raw input.
 */

#include "stage_textnorm.h"
#include "fe.h"
#include "vocab.h"
#include "stream.h"
#include "prosody.h"

#include <spfy/spfy.h>

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/* Symbol IDs from the vocabulary (cross-checked at runtime, not at
 * compile time, since the vocab lookup is data-driven). The block
 * below is the documented partition from the F2 catalog. */
#define SYM_LETTER     216
#define SYM_DIGIT      217
#define SYM_PUNCT      219
#define SYM_LOWER      211
#define SYM_UPPER      212
#define SYM_EOW_DLMTR  222

/* Walk the input bytes, classifying each. Returns the symbol ID of the
 * character TYPE (LETTER/DIGIT/PUNCT/eow_dlmtr) for the byte. */
static uint16_t classify_byte(uint8_t b)
{
    if (b == ' ' || b == '\t' || b == '\n' || b == '\r') return SYM_EOW_DLMTR;
    if (b >= 'a' && b <= 'z') return SYM_LETTER;
    if (b >= 'A' && b <= 'Z') return SYM_LETTER;
    if (b >= '0' && b <= '9') return SYM_DIGIT;
    return SYM_PUNCT;
}

static uint16_t case_of(uint8_t b)
{
    if (b >= 'A' && b <= 'Z') return SYM_UPPER;
    if (b >= 'a' && b <= 'z') return SYM_LOWER;
    return 0;
}

static int is_phrase_final(uint8_t b)
{
    return (b == '.' || b == '!' || b == '?');
}

static int is_phrase_break(uint8_t b)
{
    /* Internal phrase break: comma, semicolon, colon. Phrases continue
     * after these but tone may shift. */
    return (b == ',' || b == ';' || b == ':');
}

/* Apply prosody hints to a %text token by checking whether its source
 * byte offset falls within any hint's range. */
static void apply_hints(spfy_fe_token_t            *tok,
                         uint32_t                    byte_off,
                         const spfy_prosody_hints_t *hints)
{
    if (!hints) return;
    for (uint32_t i = 0; i < hints->n_hints; ++i) {
        const spfy_prosody_hint_t *h = &hints->hints[i];
        if (byte_off < h->byte_start || byte_off >= h->byte_end) continue;
        switch (h->kind) {
        case SPFY_HINT_EMPHASIS:
            tok->fields[SPFY_TEXT_FIELD_EMPHASIS] =
                (uint16_t)h->v.emphasis;
            break;
        case SPFY_HINT_PITCH:
            tok->fields[SPFY_TEXT_FIELD_PITCH_ST] =
                (uint16_t)(int16_t)h->v.pitch_st;
            break;
        case SPFY_HINT_RATE:
            tok->fields[SPFY_TEXT_FIELD_RATE_PCT] =
                (uint16_t)h->v.rate_pct;
            break;
        default:
            break;
        }
    }
}

int spfy_fe_textnorm_run(const spfy_fe_t            *fe,
                         const char                 *text,
                         const spfy_prosody_hints_t *hints,
                         spfy_fe_delta_t            *delta)
{
    if (!fe || !text || !delta) return SPFY_E_INVAL;

    /* Get vocab via the public accessor so this stage stays oblivious
     * to fe_t internals. We store a copy of the byte->id map locally
     * for the inner loop. */
    extern const spfy_fe_vocab_t *spfy_fe_vocab(const spfy_fe_t *fe);
    const spfy_fe_vocab_t *vocab = spfy_fe_vocab(fe);
    if (!vocab) return SPFY_E_INVAL;

    size_t n = strlen(text);
    uint16_t cur_word_id   = 0xFFFFu;       /* "no current word" */
    uint16_t cur_phrase_id = 0;
    uint32_t word_start    = 0;
    int      in_word       = 0;

    /* First pass: emit one %text token per byte; track word and
     * phrase boundaries inline. */
    for (size_t i = 0; i < n; ++i) {
        uint8_t  b   = (uint8_t)text[i];
        uint16_t sid = vocab->byte_to_id[b];
        uint16_t typ = classify_byte(b);
        uint16_t cse = case_of(b);

        int is_letter = (typ == SYM_LETTER || typ == SYM_DIGIT);
        /* Apostrophe glue: a `'` between two letters belongs INSIDE the
         * word, not at a word boundary. Without this, "I've" tokenises
         * as ["I", "ve"], and the orphaned "ve" syllable runs through
         * syllable_to_phonemes which emits /v iy/ (open-syllable magic-e
         * detection is skipped at len<3), producing a phantom /iy/.
         * Engine `--g2p "I've"` returns /ay v/ — one word, no phantom.
         * Provenance: 03-FE-AUDIT segment-shift records (49 records,
         * shift=-1) decoded to the apostrophe-tokeniser split per
         * .planning/phases/03-fe-convergence/03-03-DESIGN.md.
         * Disable via SPFY_NO_APOSTROPHE_GLUE=1 for diagnostic isolation. */
        int is_intra_apostrophe = 0;
        if (b == '\'' && in_word && i + 1 < n) {
            uint8_t nx = (uint8_t)text[i + 1];
            int nx_is_letter = ((nx >= 'a' && nx <= 'z')
                                || (nx >= 'A' && nx <= 'Z')
                                || (nx >= '0' && nx <= '9'));
            if (nx_is_letter && !getenv("SPFY_NO_APOSTROPHE_GLUE")) {
                is_intra_apostrophe = 1;
            }
        }
        int treat_as_letter = is_letter || is_intra_apostrophe;
        if (treat_as_letter && !in_word) {
            /* Open a new word. Allocate a tentative word_id; the actual
             * %word token is appended at word close so its name field
             * can hold the first-letter symbol ID. */
            cur_word_id = (uint16_t)delta->streams[SPFY_STREAM_WORD].n_tokens;
            word_start  = (uint32_t)i;
            in_word     = 1;
        } else if (!treat_as_letter && in_word) {
            /* Close current word. Push a %word token with name = first
             * letter's symbol ID, plus span info in fields. */
            spfy_fe_token_t wt = {0};
            uint8_t fb = (uint8_t)text[word_start];
            wt.name       = vocab->byte_to_id[fb];
            wt.word_id    = cur_word_id;
            wt.phrase_id  = cur_phrase_id;
            wt.fields[0]  = (uint16_t)word_start;            /* byte_start */
            wt.fields[1]  = (uint16_t)(i - word_start);      /* byte_len */
            wt.fields[2]  = case_of(fb);                     /* lead_case */
            spfy_fe_stream_push(delta, SPFY_STREAM_WORD, wt);
            in_word     = 0;
            cur_word_id = 0xFFFFu;
        }

        /* Emit the per-byte %text token. */
        spfy_fe_token_t t = {0};
        t.name      = (sid != 0xFFFFu) ? sid : 0;          /* GAP fallback */
        t.word_id   = cur_word_id;
        t.phrase_id = cur_phrase_id;
        t.fields[SPFY_TEXT_FIELD_TYPE]     = typ;
        t.fields[SPFY_TEXT_FIELD_CASE]     = cse;
        t.fields[SPFY_TEXT_FIELD_BYTE_OFF] = (uint16_t)i;
        apply_hints(&t, (uint32_t)i, hints);
        spfy_fe_stream_push(delta, SPFY_STREAM_TEXT, t);

        /* Phrase-final punctuation closes the current phrase and
         * advances the phrase_id for subsequent tokens. */
        if (is_phrase_final(b)) {
            spfy_fe_token_t pt = {0};
            pt.name      = vocab->byte_to_id[b];
            pt.phrase_id = cur_phrase_id;
            pt.fields[0] = 1;        /* final */
            spfy_fe_stream_push(delta, SPFY_STREAM_PHRASE, pt);
            cur_phrase_id++;
        } else if (is_phrase_break(b)) {
            spfy_fe_token_t pt = {0};
            pt.name      = vocab->byte_to_id[b];
            pt.phrase_id = cur_phrase_id;
            pt.fields[0] = 0;        /* medial */
            spfy_fe_stream_push(delta, SPFY_STREAM_PHRASE, pt);
            /* Engine treats comma/semicolon/colon as utterance boundaries
             * (separate USel passes), evidenced by engine prsl_slot
             * traces emitting only utt 0's slots for "Hello, world." Bump
             * phrase_id so downstream tokens land in a fresh phrase that
             * spfy_synth.c can split on (SPFY_FIRST_PHRASE_ONLY). */
            cur_phrase_id++;
        }
    }
    /* Close trailing word if any. */
    if (in_word) {
        spfy_fe_token_t wt = {0};
        uint8_t fb = (uint8_t)text[word_start];
        wt.name       = vocab->byte_to_id[fb];
        wt.word_id    = cur_word_id;
        wt.phrase_id  = cur_phrase_id;
        wt.fields[0]  = (uint16_t)word_start;
        wt.fields[1]  = (uint16_t)(n - word_start);
        wt.fields[2]  = case_of(fb);
        spfy_fe_stream_push(delta, SPFY_STREAM_WORD, wt);
    }

    /* Build %token stream by walking %text and grouping consecutive
     * non-space chars into one logical token. Each %token's name is
     * the first char's symbol ID; fields[0] = byte_start, fields[1]
     * = char_count. */
    uint32_t n_text = 0;
    const spfy_fe_token_t *tt =
        spfy_fe_stream_tokens(delta, SPFY_STREAM_TEXT, &n_text);
    int      tk_open  = 0;
    uint32_t tk_start = 0;
    for (uint32_t i = 0; i < n_text; ++i) {
        uint16_t typ = tt[i].fields[SPFY_TEXT_FIELD_TYPE];
        int      is_break = (typ == SYM_EOW_DLMTR);
        if (!is_break && !tk_open) {
            tk_open = 1;
            tk_start = i;
        } else if (is_break && tk_open) {
            spfy_fe_token_t kt = {0};
            kt.name      = tt[tk_start].name;
            kt.word_id   = tt[tk_start].word_id;
            kt.phrase_id = tt[tk_start].phrase_id;
            kt.fields[0] = (uint16_t)tk_start;
            kt.fields[1] = (uint16_t)(i - tk_start);
            spfy_fe_stream_push(delta, SPFY_STREAM_TOKEN, kt);
            tk_open = 0;
        }
    }
    if (tk_open) {
        spfy_fe_token_t kt = {0};
        kt.name      = tt[tk_start].name;
        kt.word_id   = tt[tk_start].word_id;
        kt.phrase_id = tt[tk_start].phrase_id;
        kt.fields[0] = (uint16_t)tk_start;
        kt.fields[1] = (uint16_t)(n_text - tk_start);
        spfy_fe_stream_push(delta, SPFY_STREAM_TOKEN, kt);
    }

    /* Plan 03-05 / FE-04 — clause-boundary intonation breaks.
     *
     * Engine inserts a phrase break in long sentences (>= 12 words) that
     * have no internal punctuation, before a structural-boundary closed-
     * class word at-or-after sentence midpoint.
     *
     * Empirical evidence (from `bin/spfy_dumpwav --phonemes` corpus
     * extraction; see `c:/tmp/engine_breaks_v2.jsonl`):
     *   text_029 (16 words): break before "to"        (infinitive marker)
     *   nat_036  (16 words): break before "and"       (coordinator)
     *   nat_042  (16 words): break before "while"     (subordinator)
     *   nat_043  (15 words): break before "will"      (auxiliary)
     *   nat_035  (15 words): break before "that"      (complementizer)
     *   nat_050  (16 words): break before "followed"  (past participle —
     *      not handled by this rule; lexical verb form)
     *
     * Trigger word list is closed-class only: subordinators, coordinators,
     * infinitive marker, complementizer, modals. Each entry has empirical
     * provenance per the corpus extraction.
     *
     * Disable via SPFY_NO_INTONATION_BREAK=1 (mirrors SPFY_NO_BAKED_POS,
     * SPFY_NO_LTS_REDUCTIONS, SPFY_NO_APOSTROPHE_GLUE, SPFY_NO_SILENCE_CART). */
    if (!getenv("SPFY_NO_INTONATION_BREAK")) {
        /* Trigger list trimmed to closed-class words EMPIRICALLY observed in
         * the engine_breaks_v2.jsonl corpus extraction, plus close siblings.
         * Excludes copulas (`is`/`are`/`was`/`were`) and other auxiliaries
         * which over-fire on copula constructions like nat_049 ("...devices
         * are switched..."); excludes general modals lacking empirical
         * evidence. Each retained trigger has a corpus_id provenance. */
        static const char *INTONATION_TRIGGERS[] = {
            "to",        /* infinitive marker — text_029 (verified) */
            "that",      /* complementizer / relative — nat_035 */
            "and", "or", "but",  /* coordinators — nat_036 (and) */
            "while",     /* subordinator — nat_042 (verified) */
            "because", "if", "unless", "since", "although",
            "when", "where", "who", "which",  /* subordinators / relatives */
            "will",      /* modal — nat_043 (verified) */
            NULL
        };
        const uint32_t MIN_WORDS_FOR_BREAK = 12u;
        /* MIN_BREAK_POSITION is computed per-phrase as ceil(word_count/2) —
         * stricter than the originally-coded constant 6. This excludes
         * pre-midpoint over-fires (e.g., nat_049 's `to` at idx 8 of 17;
         * ceil(17/2)=9 → does not fire). All Group-B empirical cases land
         * at exact midpoint or later, so this filter preserves them. */

        /* Walk %word stream grouped by phrase_id; for each phrase that has
         * no internal phrase break and >= MIN_WORDS_FOR_BREAK words,
         * search for the first trigger word at-or-after MIN_BREAK_POSITION.
         * If found, insert a synthetic phrase break and bump phrase_id
         * for downstream tokens. */
        uint32_t n_words = 0;
        spfy_fe_token_t *words_arr = (spfy_fe_token_t *)
            spfy_fe_stream_tokens(delta, SPFY_STREAM_WORD, &n_words);
        uint32_t n_phrases_existing = 0;
        const spfy_fe_token_t *phrases_arr =
            spfy_fe_stream_tokens(delta, SPFY_STREAM_PHRASE,
                                  &n_phrases_existing);
        uint32_t n_text2 = 0;
        spfy_fe_token_t *text_arr = (spfy_fe_token_t *)
            spfy_fe_stream_tokens(delta, SPFY_STREAM_TEXT, &n_text2);
        uint32_t n_tok = 0;
        spfy_fe_token_t *tok_arr = (spfy_fe_token_t *)
            spfy_fe_stream_tokens(delta, SPFY_STREAM_TOKEN, &n_tok);

        for (uint32_t pid = 0; pid <= cur_phrase_id; ++pid) {
            /* Phrase already has internal break (medial: fields[0] == 0)?
             * Skip if any phrase token with phrase_id == pid is medial. */
            int has_internal_break = 0;
            for (uint32_t k = 0; k < n_phrases_existing; ++k) {
                if (phrases_arr[k].phrase_id == pid
                 && phrases_arr[k].fields[0] == 0u) {
                    has_internal_break = 1;
                    break;
                }
            }
            if (has_internal_break) continue;

            /* Index this phrase's words. */
            uint32_t first_word_idx = UINT32_MAX;
            uint32_t last_word_idx  = 0;
            uint32_t phrase_word_count = 0;
            for (uint32_t w = 0; w < n_words; ++w) {
                if (words_arr[w].phrase_id != pid) continue;
                if (first_word_idx == UINT32_MAX) first_word_idx = w;
                last_word_idx = w;
                phrase_word_count++;
            }
            if (phrase_word_count < MIN_WORDS_FOR_BREAK) continue;

            /* Strict midpoint filter: trigger position must be >= ceil(words/2)
             * to exclude pre-midpoint over-fires (preposition `to`, etc.).
             * Empirically all Group-B engine breaks land at exact midpoint or
             * later. */
            uint32_t midpoint = (phrase_word_count + 1u) / 2u;

            /* Find first trigger word at relative position >= midpoint. */
            uint32_t break_word_idx = UINT32_MAX;
            uint32_t rel = 0;
            for (uint32_t w = first_word_idx; w <= last_word_idx; ++w) {
                if (words_arr[w].phrase_id != pid) continue;
                if (rel >= midpoint) {
                    /* Lowercase word against trigger list. */
                    uint32_t off = words_arr[w].fields[0];
                    uint32_t len = words_arr[w].fields[1];
                    if (len > 0 && len < 32) {
                        char wbuf[32];
                        for (uint32_t i = 0; i < len; ++i)
                            wbuf[i] = (char)tolower((unsigned char)text[off + i]);
                        wbuf[len] = '\0';
                        for (int t = 0; INTONATION_TRIGGERS[t]; ++t) {
                            if (strcmp(wbuf, INTONATION_TRIGGERS[t]) == 0) {
                                break_word_idx = w;
                                break;
                            }
                        }
                    }
                    if (break_word_idx != UINT32_MAX) break;
                }
                rel++;
            }
            if (break_word_idx == UINT32_MAX) continue;

            /* Insert synthetic phrase break: push a new %phrase token
             * (medial; phrase_id == pid) and bump phrase_id on
             * words[break_word_idx..last_word_idx] + their associated
             * %text tokens + their %token tokens to (cur_phrase_id+1). */
            uint32_t new_pid = cur_phrase_id + 1u;
            spfy_fe_token_t pt = {0};
            pt.name      = 0; /* synthetic — no source byte */
            pt.phrase_id = pid;
            pt.fields[0] = 0; /* medial */
            spfy_fe_stream_push(delta, SPFY_STREAM_PHRASE, pt);

            /* Re-fetch %word array (push may have invalidated pointers if
             * %phrase shares a stream backing — but in this codebase each
             * stream has its own backing, so words_arr stays valid). */
            uint32_t break_byte_off = words_arr[break_word_idx].fields[0];

            for (uint32_t w = 0; w < n_words; ++w) {
                if (words_arr[w].phrase_id != pid) continue;
                if (words_arr[w].fields[0] >= break_byte_off) {
                    words_arr[w].phrase_id = (uint16_t)new_pid;
                }
            }
            for (uint32_t i = 0; i < n_text2; ++i) {
                if (text_arr[i].phrase_id != pid) continue;
                uint16_t boff = text_arr[i].fields[SPFY_TEXT_FIELD_BYTE_OFF];
                if ((uint32_t)boff >= break_byte_off) {
                    text_arr[i].phrase_id = (uint16_t)new_pid;
                }
            }
            for (uint32_t i = 0; i < n_tok; ++i) {
                if (tok_arr[i].phrase_id != pid) continue;
                uint16_t tk_first_text_idx = tok_arr[i].fields[0];
                if (tk_first_text_idx < n_text2
                 && text_arr[tk_first_text_idx].phrase_id != pid) {
                    tok_arr[i].phrase_id = (uint16_t)new_pid;
                }
            }
            cur_phrase_id = new_pid;
        }
    }

    return SPFY_OK;
}
