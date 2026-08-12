/* Stage 6: SPR formatter -- builds per-slot ctx[5]/sp[5] for USel. */

#include "stage_spr.h"
#include "stage_prosody.h"
#include "phoneset.h"
#include "fe.h"
#include "stream.h"
#include "vocab.h"

#include <spfy/spfy.h>

#include <stdlib.h>
#include <string.h>

/* SAMPA-vocab-id -> ARPAbet name lookup. */
typedef struct {
    uint16_t    vocab_id;
    const char *arpa;
} sampa_to_arpa_t;

static const sampa_to_arpa_t SAMPA_ARPA[] = {
    { 271, "ae" }, { 259, "aa" }, { 258, "eh" }, { 257, "ey" },
    { 255, "iy" }, { 256, "ih" }, { 274, "ow" }, { 278, "ao" },
    { 272, "uw" }, { 273, "uh" }, { 266, "ax" }, { 276, "ay" },
    { 277, "aw" }, { 260, "ah" }, { 264, "er" }, { 265, "ah" },
    { 261, "ix" },
    { 229, "b"  }, { 230, "p"  }, { 231, "d"  }, { 232, "t"  },
    { 235, "k"  }, { 236, "g"  }, { 240, "f"  }, { 239, "v"  },
    { 238, "th" }, { 237, "dh" }, { 242, "s"  }, { 241, "z"  },
    { 244, "sh" }, { 243, "zh" }, { 246, "ch" }, { 245, "jh" },
    { 247, "hh" }, { 248, "m"  }, { 249, "n"  }, { 250, "ng" },
    { 251, "r"  }, { 252, "l"  }, { 253, "y"  }, { 254, "w"  },
    /* Allophones the engine emits as separate SPR letters. */
    { 233, "dx" }, { 265, "el" },
    {   0, NULL }
};

static const char *vocab_to_arpa(uint16_t vocab_id)
{
    for (int i = 0; SAMPA_ARPA[i].arpa; ++i) {
        if (SAMPA_ARPA[i].vocab_id == vocab_id) return SAMPA_ARPA[i].arpa;
    }
    return NULL;
}

/* Resolve vocab_id -> (phone_id, voiced). */
static int lookup_phone(const spfy_phoneset_t *ps,
                         uint16_t vocab_id,
                         uint8_t *out_phone, uint8_t *out_voiced)
{
    if (vocab_id == 0) {
        *out_phone  = ps ? ps->silence_phone_id : 0u;
        *out_voiced = 0;
        return 1;
    }
    const char *arpa = vocab_to_arpa(vocab_id);
    if (!arpa) {
        *out_phone = 0; *out_voiced = 0; return 0;
    }
    if (ps) {
        uint8_t pid = spfy_phoneset_lookup(ps, arpa);
        if (pid != 0xFF) {
            *out_phone  = pid;
            *out_voiced = ps->entries[pid].is_voiced;
            return 1;
        }
    }
    for (int i = 0; SAMPA_ARPA[i].arpa; ++i) {
        if (SAMPA_ARPA[i].vocab_id == vocab_id) {
            *out_phone  = (uint8_t)(i + 1);
            *out_voiced = 0;
            return 1;
        }
    }
    *out_phone = 0; *out_voiced = 0;
    return 0;
}

static void fill_slot(spfy_fe_slot_t       *slot,
                       uint8_t                phone_id,
                       uint8_t                side,
                       uint8_t                voiced,
                       uint16_t               emphasis,
                       int16_t                pitch,
                       int16_t                rate)
{
    /* HP-class encoding: phone_id*2 + side. */
    slot->ctx[2] = (int32_t)((uint32_t)phone_id * 2u + side);
    slot->is_voiced = voiced;
    slot->emphasis_level = (uint8_t)emphasis;
    slot->pitch_offset_st = (int8_t)pitch;
    slot->rate_offset_pct = (int8_t)rate;
}

/* Fill ctx[0..1, 3..4] for slot at index `i`. */
static void fill_neighbours(spfy_fe_slot_t *slots, uint32_t n,
                             const spfy_phoneset_t *ps)
{
    int32_t pau_side0 = 0, pau_side1 = 0;
    if (ps && ps->silence_phone_id != 0xff) {
        pau_side0 = (int32_t)ps->silence_phone_id * 2;
        pau_side1 = pau_side0 + 1;
    }
    for (uint32_t i = 0; i < n; ++i) {
        int32_t side_sentinel = (i & 1u) ? pau_side1 : pau_side0;
        slots[i].ctx[0] = (i >= 4)         ? slots[i - 4].ctx[2] : side_sentinel;
        slots[i].ctx[1] = (i >= 2)         ? slots[i - 2].ctx[2] : side_sentinel;
        slots[i].ctx[3] = (i + 2 < n)      ? slots[i + 2].ctx[2] : side_sentinel;
        slots[i].ctx[4] = (i + 4 < n)      ? slots[i + 4].ctx[2] : side_sentinel;
    }
}

/* Fill sp[5] from positional info in the streams. */
static void fill_sp_features(spfy_fe_slot_t       *slots,
                              uint32_t              n,
                              const spfy_fe_token_t *phons,
                              uint32_t              n_phons,
                              const spfy_fe_token_t *syls,
                              uint32_t              n_syls,
                              const spfy_fe_token_t *words,
                              uint32_t              n_words)
{
    (void)words; (void)n_words;
    /* Slot layout: 2 leading pau pads, then 2-per-phoneme, then 2 trailing
     * pau pads. */
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t hp = i / 2;
        if (hp == 0 || hp > n_phons) continue;
        uint32_t pi = hp - 1;
        const spfy_fe_token_t *p = &phons[pi];

        uint16_t syl_in_phrase = 0;
        if (p->syl_id < n_syls) {
            uint16_t target_phrase = syls[p->syl_id].phrase_id;
            for (uint16_t k = 0; k <= p->syl_id; ++k) {
                if (k < n_syls && syls[k].phrase_id == target_phrase)
                    ++syl_in_phrase;
            }
        }
        uint16_t syl_type = 0;
        if (p->syl_id < n_syls) {
            uint16_t st = syls[p->syl_id].name;
            syl_type = (st == 442) ? 1u :
                       (st == 443) ? 2u :
                       0u;
        }
        uint16_t syl_in_word = 0;
        if (p->syl_id < n_syls) {
            syl_in_word = (uint16_t)(syls[p->syl_id].fields[2] + 1);
        }
        uint16_t word_in_phrase = (uint16_t)(p->word_id + 1);
        uint16_t phon_in_syl = p->fields[3];

        slots[i].sp[0] = syl_in_phrase;
        slots[i].sp[1] = syl_type;
        slots[i].sp[2] = syl_in_word;
        slots[i].sp[3] = word_in_phrase;
        slots[i].sp[4] = phon_in_syl;
    }
}

int spfy_fe_spr_run(const spfy_fe_t       *fe,
                    spfy_fe_delta_t       *delta,
                    spfy_fe_utterance_t   *utt)
{
    if (!delta || !utt) return SPFY_E_INVAL;
    const spfy_phoneset_t *ps = fe ? spfy_fe_phoneset(fe) : NULL;

    uint32_t n_phons = 0;
    const spfy_fe_token_t *phons =
        spfy_fe_stream_tokens(delta, SPFY_STREAM_PHONEME, &n_phons);
    uint32_t n_syls = 0;
    const spfy_fe_token_t *syls =
        spfy_fe_stream_tokens(delta, SPFY_STREAM_SYL, &n_syls);
    uint32_t n_words = 0;
    const spfy_fe_token_t *words =
        spfy_fe_stream_tokens(delta, SPFY_STREAM_WORD, &n_words);

    /* 2 halfphone slots per phoneme + 4 pau pad slots (2 leading silence +
     * 2 trailing silence). */
    uint8_t pau_id = (ps && ps->silence_phone_id != 0xff)
                       ? ps->silence_phone_id : 0u;
    uint32_t n_slots = (n_phons + 2u) * 2u;
    if (n_phons == 0) {
        utt->slots   = NULL;
        utt->n_slots = 0;
        return SPFY_OK;
    }
    spfy_fe_slot_t *slots = (spfy_fe_slot_t *)
        calloc(n_slots, sizeof *slots);
    if (!slots) return SPFY_E_NOMEM;

    fill_slot(&slots[0], pau_id, 0, 0, 0, 0, 0);
    fill_slot(&slots[1], pau_id, 1, 0, 0, 0, 0);

    for (uint32_t i = 0; i < n_phons; ++i) {
        uint8_t pid = 0, voiced = 0;
        lookup_phone(ps, phons[i].name, &pid, &voiced);
        uint16_t emph = phons[i].fields[SPFY_PROSODY_FIELD_EMPHASIS];
        int16_t  ptch = (int16_t)phons[i].fields[SPFY_PROSODY_FIELD_PITCH_ST];
        int16_t  rate = (int16_t)phons[i].fields[SPFY_PROSODY_FIELD_RATE_PCT];
        uint32_t base = (i + 1u) * 2u;
        fill_slot(&slots[base + 0], pid, 0, voiced, emph, ptch, rate);
        fill_slot(&slots[base + 1], pid, 1, voiced, emph, ptch, rate);
    }
    uint32_t tail = (n_phons + 1u) * 2u;
    fill_slot(&slots[tail + 0], pau_id, 0, 0, 0, 0, 0);
    fill_slot(&slots[tail + 1], pau_id, 1, 0, 0, 0, 0);

    fill_neighbours(slots, n_slots, ps);
    fill_sp_features(slots, n_slots, phons, n_phons, syls, n_syls,
                      words, n_words);

    utt->slots   = slots;
    utt->n_slots = n_slots;
    return SPFY_OK;
}
