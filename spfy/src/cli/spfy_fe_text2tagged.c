/* spfy_fe_text2tagged — dump the in-house FE's tagged-text output for
 * arbitrary input text. Compares against the SpeechWorks DLL's
 * spfy_fe_synth_text() format and feeds parse_fe_output_into_slots().
 *
 *   spfy_fe_text2tagged "Hello world."
 *   echo "Hello world." | spfy_fe_text2tagged --pipe */

#include "fe_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void dump_one(const char *text)
{
    /* Long-passage safe (the sioux pangram in the audit corpus runs
     * ~30 KB tagged). Match the buffer size spfy_synth.c uses. */
    static char buf[65536];
    int rc = spfy_fe_internal_text_to_tagged(text, buf, sizeof buf);
    if (rc < 0) { fprintf(stderr, "rc=%d\n", rc); return; }
    if (rc == 1) fprintf(stderr, "(warning: tagged output truncated)\n");
    printf("%s\n", buf);
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--pipe") != 0) {
        dump_one(argv[1]); return 0;
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
        "usage: %s \"<text>\"\n"
        "       %s --pipe   (one line per stdin)\n",
        argv[0], argv[0]);
    return 2;
}
