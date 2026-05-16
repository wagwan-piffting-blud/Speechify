#ifndef SPFY_FE_VOCAB_H
#define SPFY_FE_VOCAB_H

#include <stddef.h>
#include <stdint.h>

/* FE symbol vocabulary (469 entries).
 *
 * Decoded from .data offset 0x4d0f0 in SWIttsFe-en-US.dll -- a flat
 * array of 469 string pointers covering every named token the FE
 * pipeline references. Used as the universal "decoder ring" for
 * dictionary/table data: every byte in any of the 726 carved tables
 * is an index into this vocabulary.
 *
 * Partition (see project_fe_f0_eloquence.md for full catalog):
 *   0..96   ASCII alphabet (GAP, a-z, A-Z, 0-9, all punctuation)
 *   97..161 Latin-1 accented letters (à, è, ñ, ü, æ, ç ...)
 *   162..209 Western symbols (smart quotes, em/en dash, ¡¿«»£¢±°§©®™)
 *   210..321 phonetic features + SAMPA phoneme symbols
 *   322..360 morphology + dict names + POS tags
 *   361..410 ambiguous-POS classes + grammar features
 *   411..468 language IDs + number/stress/tone contours + format strings
 */

#define SPFY_FE_VOCAB_N        469
#define SPFY_FE_VOCAB_MAX_LEN  64

typedef struct {
    /* Display name (NUL-terminated). May be NULL for non-ASCII raw
     * byte slots that don't have a printable representation; in that
     * case the caller can fall back to the raw byte stored at .raw_byte
     * (used for accented Latin-1 chars). */
    const char *name;

    /* For symbol IDs whose vocabulary entry is a single non-ASCII byte
     * (Latin-1 accented chars, Western symbols), this holds the raw
     * code-point so we can convert input bytes back to symbol IDs. 0
     * for entries where the name is the canonical form. */
    uint8_t     raw_byte;
} spfy_fe_symbol_t;

typedef struct {
    spfy_fe_symbol_t entries[SPFY_FE_VOCAB_N];
    uint32_t         n;             /* always 469 after load */
    /* Reverse lookup: input byte -> symbol ID. For each byte value
     * 0..255, holds the vocab index whose .name is exactly that 1-char
     * string. UINT16_MAX = no mapping. Built at load time so the
     * text-norm stage can convert raw input bytes to symbol IDs in
     * O(1). Latin-1 / extended bytes (97-209 in vocab) need a
     * separate population path -- those names are NULL in the JSON
     * dump, so for now they fall through to UINT16_MAX and are
     * treated as unrecognised by the text stage. */
    uint16_t         byte_to_id[256];
} spfy_fe_vocab_t;

/* Load 469-entry vocabulary from the JSON file produced by
 * fe_symbol_table extraction. */
int  spfy_fe_vocab_load(const char *json_path, spfy_fe_vocab_t *out);

void spfy_fe_vocab_free(spfy_fe_vocab_t *v);

/* O(1) lookup: id -> name (NULL if id is OOR). */
const char *spfy_fe_vocab_name(const spfy_fe_vocab_t *v, uint32_t id);

/* Linear lookup: name -> id, or 0xFFFFFFFF if not found. Cache the
 * result in callers; this is not a hot path. */
uint32_t    spfy_fe_vocab_id(const spfy_fe_vocab_t *v, const char *name);

/* Predefined symbol-ID constants for stable references (a few of the
 * hottest ones across the pipeline). */
enum {
    SPFY_SYM_GAP        = 0,
    SPFY_SYM_LETTER_A   = 1,
    SPFY_SYM_LETTER_Z   = 31,
    SPFY_SYM_DIGIT_0    = 53,
    SPFY_SYM_DIGIT_9    = 62,
    SPFY_SYM_DICT       = 334,
    SPFY_SYM_HUGEDICT   = 335,
    SPFY_SYM_SPR        = 336,
    SPFY_SYM_WORDDICT   = 337,
    SPFY_SYM_ROOTDICT   = 338,
    SPFY_SYM_USERDICT   = 339,
    SPFY_SYM_DISAMBIG   = 360,
    SPFY_SYM_NAME       = 467,
    SPFY_SYM_PCT_D      = 468
};

#endif
