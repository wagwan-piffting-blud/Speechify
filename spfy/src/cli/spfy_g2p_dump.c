/* spfy_g2p_dump — print ARPAbet phonemes for words via the multi-stage
 * G2P pipeline (CMU dict → suffix strip → letter-to-sound).
 *
 *   spfy_g2p_dump hello                       single word
 *   spfy_g2p_dump --batch foo bar baz         many words
 *   spfy_g2p_dump --verbose word              also show which stage hit */

#include "g2p.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *origin_name(spfy_g2p_origin_t o)
{
    switch (o) {
        case SPFY_G2P_HIT_DICT:   return "dict";
        case SPFY_G2P_HIT_SUFFIX: return "suffix";
        case SPFY_G2P_HIT_LTS:    return "lts";
        default:                  return "?";
    }
}

static void dump_one(const char *word, int verbose)
{
    char buf[160];
    spfy_g2p_origin_t origin;
    int rc = spfy_g2p_word_lookup_ex(word, buf, sizeof buf, &origin);
    if (rc != 0) {
        fprintf(stderr, "%s: rc=%d\n", word, rc);
        return;
    }
    if (verbose) {
        printf("%-24s [%-6s] %s\n", word, origin_name(origin), buf);
    } else {
        printf("%-24s %s\n", word, buf);
    }
}

int main(int argc, char **argv)
{
    int verbose = 0;
    int argi = 1;
    int batch = 0;
    while (argi < argc && argv[argi][0] == '-') {
        if (strcmp(argv[argi], "--verbose") == 0
            || strcmp(argv[argi], "-v") == 0) {
            verbose = 1; ++argi;
        } else if (strcmp(argv[argi], "--batch") == 0) {
            batch = 1; ++argi;
        } else {
            fprintf(stderr, "unknown option: %s\n", argv[argi]);
            return 2;
        }
    }

    if (argi < argc) {
        if (batch) {
            for (int i = argi; i < argc; ++i) dump_one(argv[i], verbose);
        } else {
            dump_one(argv[argi], verbose);
        }
        return 0;
    }

    fprintf(stderr,
        "spfy_g2p_dump — multi-stage G2P lookup (%zu dict entries)\n"
        "usage: %s [-v|--verbose] <word>\n"
        "       %s [-v|--verbose] --batch <word1> [<word2> ...]\n",
        spfy_g2p_dict_size(), argv[0], argv[0]);
    return 2;
}
