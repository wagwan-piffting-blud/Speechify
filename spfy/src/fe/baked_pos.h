#ifndef SPFY_BAKED_POS_H
#define SPFY_BAKED_POS_H

typedef enum {
    POS_UNKNOWN = 0,
    POS_NOUN, POS_ADJ, POS_VERB, POS_ADV, POS_INTERJ,
    POS_QUANT, POS_NOUN_ADJ, POS_NOUN_VERB, POS_VERB_ADJ,
    POS_NOUN_VERB_ADJ, POS_ADJ_ADV,
    POS_DET, POS_AUX, POS_PREP, POS_PRO, POS_PRO2,
    POS_WH, POS_CONJ, POS_DEM, POS_UNDEF,
    POS_THERE, POS_NOT, POS_POSTPOS, POS_DISAMBIG, POS_OTHER,
} spfy_pos_class_t;

int spfy_baked_pos_lookup(const char *word_lower, spfy_pos_class_t *out);
int spfy_pos_is_open_class(spfy_pos_class_t p);

#endif
