/* Multi-stream token framework -- minimal Delta-style data structure. */

#include "stream.h"

#include <spfy/spfy.h>

#include <stdlib.h>
#include <string.h>

void spfy_fe_delta_init(spfy_fe_delta_t *d)
{
    memset(d, 0, sizeof *d);
    for (int i = 0; i < SPFY_STREAM__MAX; ++i) {
        d->streams[i].kind = (spfy_stream_kind_t)i;
    }
}

void spfy_fe_delta_free(spfy_fe_delta_t *d)
{
    if (!d) return;
    for (int i = 0; i < SPFY_STREAM__MAX; ++i) {
        free(d->streams[i].tokens);
    }
    memset(d, 0, sizeof *d);
}

uint32_t spfy_fe_stream_push(spfy_fe_delta_t   *d,
                              spfy_stream_kind_t kind,
                              spfy_fe_token_t    tok)
{
    if (kind < 0 || kind >= SPFY_STREAM__MAX) return UINT32_MAX;
    spfy_fe_stream_t *s = &d->streams[kind];
    if (s->n_tokens == s->cap) {
        uint32_t nc = s->cap ? s->cap * 2u : 16u;
        spfy_fe_token_t *nt = (spfy_fe_token_t *)
            realloc(s->tokens, nc * sizeof *nt);
        if (!nt) return UINT32_MAX;
        s->tokens = nt;
        s->cap    = nc;
    }
    s->tokens[s->n_tokens] = tok;
    return s->n_tokens++;
}

const spfy_fe_token_t *spfy_fe_stream_tokens(const spfy_fe_delta_t *d,
                                              spfy_stream_kind_t    kind,
                                              uint32_t             *out_n)
{
    if (kind < 0 || kind >= SPFY_STREAM__MAX) {
        if (out_n) *out_n = 0;
        return NULL;
    }
    if (out_n) *out_n = d->streams[kind].n_tokens;
    return d->streams[kind].tokens;
}
