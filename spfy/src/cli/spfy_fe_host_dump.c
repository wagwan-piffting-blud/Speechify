/*
 * spfy_fe_host_dump.c — exercise the hosted FE via the public
 * spfy_fe.h API, fill ctx[5]/sp[5]/is_voiced via the loaded voice's
 * phoneset, and dump the slot table. This is the integration smoke
 * test for the hosted FE replacement.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Usage:
 *   spfy_fe_host_dump <voice.vcf> "<text>"
 */

#include "../fe/fe.h"
#include "../fe/phoneset.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr,
            "usage: %s <voice.vcf> \"<text>\"\n",
            argv[0]);
        return 2;
    }
    const char *vcf_path = argv[1];
    const char *text     = argv[2];

    setvbuf(stderr, NULL, _IONBF, 0);
    fprintf(stderr, "[host_dump] starting; vcf=%s text=%s\n", vcf_path, text);

    spfy_fe_t *fe = NULL;
    /* Hosted FE ignores the vocab/tables paths. */
    int rc = spfy_fe_open(NULL, NULL, NULL, &fe);
    fprintf(stderr, "[host_dump] spfy_fe_open -> %d (fe=%p)\n", rc, (void *)fe);
    if (rc != 0 || !fe) {
        fprintf(stderr, "spfy_fe_open failed rc=%d\n", rc);
        return 3;
    }

    rc = spfy_fe_set_voice_vcf(fe, vcf_path);
    if (rc != 0) {
        fprintf(stderr, "spfy_fe_set_voice_vcf failed rc=%d\n", rc);
        spfy_fe_close(fe);
        return 4;
    }

    spfy_fe_utterance_t *utt = NULL;
    rc = spfy_fe_synth_text(fe, text, NULL, &utt);
    if (rc != 0 || !utt) {
        fprintf(stderr, "spfy_fe_synth_text failed rc=%d\n", rc);
        spfy_fe_close(fe);
        return 5;
    }

    const spfy_phoneset_t *ps = spfy_fe_phoneset(fe);

    fprintf(stderr, "\n[host_dump] === SLOT TABLE (n=%u) ===\n", utt->n_slots);
    fprintf(stderr,
        " idx | ctx[5]                       | sp[5]               | vc emph | phone\n"
        "-----+-------------------------------+---------------------+---------+------\n");
    for (uint32_t i = 0; i < utt->n_slots; i++) {
        const spfy_fe_slot_t *s = &utt->slots[i];
        /* Decode phone from ctx[2] = phone_id*2 + side. */
        uint32_t enc = (uint32_t)s->ctx[2];
        uint32_t pid = enc / 2;
        uint32_t side = enc & 1u;
        const char *name = "?";
        if (ps && pid < ps->n_phones) name = ps->entries[pid].name;
        fprintf(stderr,
            " %3u | %4d %4d %4d %4d %4d | %3u %3u %3u %3u %3u |  %u    %u  | %-4s.%u\n",
            i,
            s->ctx[0], s->ctx[1], s->ctx[2], s->ctx[3], s->ctx[4],
            s->sp[0], s->sp[1], s->sp[2], s->sp[3], s->sp[4],
            s->is_voiced, s->emphasis_level,
            name, side);
    }

    spfy_fe_utterance_free(utt);
    spfy_fe_close(fe);
    fprintf(stderr, "\n[host_dump] DONE\n");
    return 0;
}
