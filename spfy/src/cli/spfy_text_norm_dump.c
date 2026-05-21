/* spfy_text_norm_dump — print tokens produced by the text normalizer.
 *
 *   spfy_text_norm_dump "The year 2024 had 1990 things to do."
 *   spfy_text_norm_dump --pipe   (one line per arg via stdin)
 *
 * Each token prints on its own line as
 *
 *   <TYPE> <text>
 *
 * where TYPE is WORD / PHRASE_BREAK / SENTENCE_BREAK. */

#include "text_norm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *type_name(spfy_token_type_t t)
{
    switch (t) {
        case SPFY_TOKEN_WORD:           return "WORD";
        case SPFY_TOKEN_PHRASE_BREAK:   return "PHRASE_BREAK";
        case SPFY_TOKEN_SENTENCE_BREAK: return "SENTENCE_BREAK";
        default:                        return "?";
    }
}

static void dump_one(const char *text)
{
    spfy_token_t toks[512];
    size_t n = 0;
    int rc = spfy_text_normalize(text, toks, sizeof toks / sizeof toks[0], &n);
    if (rc < 0) {
        fprintf(stderr, "normalize rc=%d\n", rc);
        return;
    }
    if (rc == 1) fprintf(stderr, "(warning: token buffer overflow)\n");
    for (size_t i = 0; i < n; ++i) {
        printf("%-14s %s\n", type_name(toks[i].type), toks[i].text);
    }
    printf("(%zu tokens)\n", n);
}

int main(int argc, char **argv)
{
    if (argc == 2) {
        dump_one(argv[1]);
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "--pipe") == 0) {
        char line[2048];
        while (fgets(line, sizeof line, stdin)) {
            size_t L = strlen(line);
            if (L && line[L-1] == '\n') line[L-1] = '\0';
            printf("--- %s ---\n", line);
            dump_one(line);
        }
        return 0;
    }
    fprintf(stderr,
        "spfy_text_norm_dump — tokenize + normalize text\n"
        "usage: %s \"<text>\"\n"
        "       %s --pipe       (one line per stdin)\n",
        argv[0], argv[0]);
    return 2;
}
