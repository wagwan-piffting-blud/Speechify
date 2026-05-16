/* Prosody hints + minimal SSML parser stub. */

#include "prosody.h"

#include <spfy/spfy.h>

#include <stdlib.h>
#include <string.h>

void spfy_prosody_hints_init(spfy_prosody_hints_t *h)
{
    memset(h, 0, sizeof *h);
}

void spfy_prosody_hints_free(spfy_prosody_hints_t *h)
{
    if (!h) return;
    free(h->hints);
    memset(h, 0, sizeof *h);
}

int spfy_prosody_hints_add(spfy_prosody_hints_t *h, spfy_prosody_hint_t hint)
{
    if (h->n_hints == h->cap) {
        uint32_t nc = h->cap ? h->cap * 2u : 8u;
        spfy_prosody_hint_t *p = (spfy_prosody_hint_t *)
            realloc(h->hints, nc * sizeof *p);
        if (!p) return SPFY_E_NOMEM;
        h->hints = p;
        h->cap   = nc;
    }
    h->hints[h->n_hints++] = hint;
    return SPFY_OK;
}

int spfy_prosody_emphasize(spfy_prosody_hints_t *h,
                            uint32_t byte_start, uint32_t byte_end,
                            spfy_emphasis_t level)
{
    spfy_prosody_hint_t hint = {0};
    hint.kind = SPFY_HINT_EMPHASIS;
    hint.byte_start = byte_start;
    hint.byte_end   = byte_end;
    hint.v.emphasis = level;
    return spfy_prosody_hints_add(h, hint);
}

int spfy_prosody_pitch_shift(spfy_prosody_hints_t *h,
                              uint32_t byte_start, uint32_t byte_end,
                              int8_t   semitones)
{
    spfy_prosody_hint_t hint = {0};
    hint.kind = SPFY_HINT_PITCH;
    hint.byte_start = byte_start;
    hint.byte_end   = byte_end;
    hint.v.pitch_st = semitones;
    return spfy_prosody_hints_add(h, hint);
}

int spfy_prosody_rate(spfy_prosody_hints_t *h,
                       uint32_t byte_start, uint32_t byte_end,
                       int16_t  rate_pct)
{
    spfy_prosody_hint_t hint = {0};
    hint.kind = SPFY_HINT_RATE;
    hint.byte_start = byte_start;
    hint.byte_end   = byte_end;
    hint.v.rate_pct = rate_pct;
    return spfy_prosody_hints_add(h, hint);
}

int spfy_prosody_break(spfy_prosody_hints_t *h,
                        uint32_t byte_pos,
                        uint16_t duration_ms)
{
    spfy_prosody_hint_t hint = {0};
    hint.kind = SPFY_HINT_BREAK;
    hint.byte_start = byte_pos;
    hint.byte_end   = byte_pos;
    hint.v.break_ms = duration_ms;
    return spfy_prosody_hints_add(h, hint);
}

/* SSML parsing.
 *
 * Single-pass walk over the input, copying non-tag chars to the output
 * buffer and tracking byte offsets within OUTPUT (not source). When we
 * hit `<`, parse the tag:
 *
 *   <emphasis level="strong|moderate|reduced|none">..</emphasis>
 *   <prosody pitch="+5st|x-low|low|medium|high" rate="+10%|slow|fast">..</prosody>
 *   <break time="500ms"/>     (self-closing, point-event)
 *   <phoneme ph="..." alphabet="SPR">..</phoneme>     (overrides pronunciation)
 *
 * Container tags push an "open record" onto a small stack; the
 * matching `</tag>` pops and emits the final hint with byte_end =
 * current output offset.
 *
 * Unknown tags are stripped silently (text inside them is kept).
 * Quoted attribute values support both `"..."` and `'...'`.
 */

#include <ctype.h>
#include <stdlib.h>

#define SSML_STACK_MAX 16

typedef struct {
    const char *tag_name;       /* "emphasis"/"prosody"/etc */
    uint8_t     tag_len;
    uint32_t    byte_start;     /* output byte offset when tag opened */
    spfy_prosody_hint_t pending; /* fields filled when </tag> is hit */
} ssml_open_t;

static int sn_streq_ci(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; ++i) {
        char ca = (char)tolower((unsigned char)a[i]);
        char cb = (char)tolower((unsigned char)b[i]);
        if (ca != cb) return 0;
    }
    return 1;
}

/* Parse `attr="value"` form starting at *p (just past '<tag '). On
 * success, *p advances past the closing quote. Returns 1 on found,
 * 0 if no attribute at this position. */
static int parse_attr(const char **p, const char *end,
                       char *name_out, size_t name_cap,
                       char *val_out,  size_t val_cap)
{
    const char *q = *p;
    while (q < end && (*q == ' ' || *q == '\t')) ++q;
    if (q >= end || *q == '>' || *q == '/') return 0;

    /* Read name. */
    const char *name_s = q;
    while (q < end && *q != '=' && *q != ' ' && *q != '/' && *q != '>') ++q;
    size_t nl = (size_t)(q - name_s);
    if (nl == 0 || nl + 1 >= name_cap) return 0;
    memcpy(name_out, name_s, nl); name_out[nl] = 0;

    /* Optional `=value`. */
    if (q < end && *q == '=') {
        ++q;
        char quote = (q < end) ? *q : 0;
        if (quote == '"' || quote == '\'') {
            ++q;
            const char *val_s = q;
            while (q < end && *q != quote) ++q;
            size_t vl = (size_t)(q - val_s);
            if (vl + 1 >= val_cap) return 0;
            memcpy(val_out, val_s, vl); val_out[vl] = 0;
            if (q < end) ++q;     /* skip closing quote */
        } else {
            /* Unquoted value -- read until space/>. */
            const char *val_s = q;
            while (q < end && *q != ' ' && *q != '/' && *q != '>') ++q;
            size_t vl = (size_t)(q - val_s);
            if (vl + 1 >= val_cap) return 0;
            memcpy(val_out, val_s, vl); val_out[vl] = 0;
        }
    } else {
        val_out[0] = 0;
    }
    *p = q;
    return 1;
}

/* Map emphasis level word to enum. Returns SPFY_EMPH_NONE for empty/
 * unknown. */
static spfy_emphasis_t emph_from_str(const char *s)
{
    if (sn_streq_ci(s, "strong",   strlen("strong"))   && strlen(s) == 6)
        return SPFY_EMPH_STRONG;
    if (sn_streq_ci(s, "moderate", strlen("moderate")) && strlen(s) == 8)
        return SPFY_EMPH_MODERATE;
    if (sn_streq_ci(s, "reduced",  strlen("reduced"))  && strlen(s) == 7)
        return SPFY_EMPH_REDUCED;
    return SPFY_EMPH_NONE;
}

/* Parse a pitch expression like "+5st" / "-3st" / "high" / "low". */
static int8_t pitch_from_str(const char *s)
{
    if (!s || !*s) return 0;
    if (sn_streq_ci(s, "x-low",  5) && strlen(s) == 5) return -8;
    if (sn_streq_ci(s, "low",    3) && strlen(s) == 3) return -4;
    if (sn_streq_ci(s, "medium", 6) && strlen(s) == 6) return  0;
    if (sn_streq_ci(s, "high",   4) && strlen(s) == 4) return  4;
    if (sn_streq_ci(s, "x-high", 6) && strlen(s) == 6) return  8;
    /* Numeric "+Nst" / "-Nst" / "Nst" */
    int sign = 1; int idx = 0;
    if (s[0] == '+') ++idx;
    else if (s[0] == '-') { sign = -1; ++idx; }
    int v = 0;
    while (s[idx] >= '0' && s[idx] <= '9') { v = v * 10 + (s[idx] - '0'); ++idx; }
    return (int8_t)(sign * v);
}

/* Parse a rate expression like "+10%" / "slow" / "fast" / "0.8". */
static int16_t rate_from_str(const char *s)
{
    if (!s || !*s) return 0;
    if (sn_streq_ci(s, "x-slow", 6) && strlen(s) == 6) return -50;
    if (sn_streq_ci(s, "slow",   4) && strlen(s) == 4) return -20;
    if (sn_streq_ci(s, "medium", 6) && strlen(s) == 6) return   0;
    if (sn_streq_ci(s, "fast",   4) && strlen(s) == 4) return  30;
    if (sn_streq_ci(s, "x-fast", 6) && strlen(s) == 6) return  60;
    int sign = 1; int idx = 0;
    if (s[0] == '+') ++idx;
    else if (s[0] == '-') { sign = -1; ++idx; }
    int v = 0;
    while (s[idx] >= '0' && s[idx] <= '9') { v = v * 10 + (s[idx] - '0'); ++idx; }
    return (int16_t)(sign * v);
}

/* Parse "500ms" -> 500. Plain numbers also accepted. */
static uint16_t break_ms_from_str(const char *s)
{
    if (!s) return 0;
    int v = 0; int idx = 0;
    while (s[idx] >= '0' && s[idx] <= '9') { v = v * 10 + (s[idx] - '0'); ++idx; }
    return (uint16_t)v;
}

int spfy_prosody_parse_ssml(const char            *ssml,
                             char                 **out_plain,
                             spfy_prosody_hints_t  *out_hints)
{
    spfy_prosody_hints_init(out_hints);
    *out_plain = NULL;
    if (!ssml) return SPFY_OK;

    size_t in_n  = strlen(ssml);
    char  *out   = (char *)malloc(in_n + 1);   /* output is <= input */
    if (!out) return SPFY_E_NOMEM;
    size_t out_n = 0;

    ssml_open_t stack[SSML_STACK_MAX];
    int top = 0;

    const char *p   = ssml;
    const char *end = ssml + in_n;

    while (p < end) {
        if (*p != '<') {
            out[out_n++] = *p++;
            continue;
        }
        /* Tag start. */
        const char *tag_p = p + 1;
        int closing = 0;
        if (tag_p < end && *tag_p == '/') { closing = 1; ++tag_p; }
        const char *name_s = tag_p;
        while (tag_p < end && *tag_p != ' ' && *tag_p != '>' && *tag_p != '/')
            ++tag_p;
        size_t name_l = (size_t)(tag_p - name_s);
        if (name_l == 0) { out[out_n++] = *p++; continue; }   /* not a tag */

        /* Skip to end of tag. */
        const char *attrs_s = tag_p;
        const char *gt = tag_p;
        while (gt < end && *gt != '>') ++gt;
        if (gt >= end) { out[out_n++] = *p++; continue; }     /* malformed */
        int self_closing = (gt > p && gt[-1] == '/');

        if (closing) {
            /* Find matching open tag on stack. */
            for (int i = top - 1; i >= 0; --i) {
                if (stack[i].tag_len == name_l &&
                    sn_streq_ci(stack[i].tag_name, name_s, name_l)) {
                    /* Emit hint. */
                    spfy_prosody_hint_t h = stack[i].pending;
                    h.byte_end = (uint32_t)out_n;
                    if (h.kind != (spfy_hint_kind_t)0xFFFF) {
                        spfy_prosody_hints_add(out_hints, h);
                    }
                    /* Pop everything from i upward (handles bad nesting). */
                    top = i;
                    break;
                }
            }
        } else {
            /* Opening or self-closing tag. Parse known names. */
            spfy_prosody_hint_t h = {0};
            h.kind = (spfy_hint_kind_t)0xFFFF;     /* "no hint" */
            h.byte_start = (uint32_t)out_n;

            const char *ap = attrs_s;
            char an[32], av[64];
            if (sn_streq_ci(name_s, "emphasis", name_l) && name_l == 8) {
                h.kind = SPFY_HINT_EMPHASIS;
                h.v.emphasis = SPFY_EMPH_MODERATE;
                while (ap < gt && parse_attr(&ap, gt, an, sizeof an,
                                              av, sizeof av)) {
                    if (sn_streq_ci(an, "level", 5))
                        h.v.emphasis = emph_from_str(av);
                }
            } else if (sn_streq_ci(name_s, "prosody", name_l) && name_l == 7) {
                /* Prosody is one tag carrying potentially MULTIPLE hints. */
                int8_t  pitch = 0; int8_t pitch_set = 0;
                int16_t rate  = 0; int8_t rate_set  = 0;
                while (ap < gt && parse_attr(&ap, gt, an, sizeof an,
                                              av, sizeof av)) {
                    if (sn_streq_ci(an, "pitch", 5)) {
                        pitch = pitch_from_str(av); pitch_set = 1;
                    } else if (sn_streq_ci(an, "rate", 4)) {
                        rate = rate_from_str(av); rate_set = 1;
                    }
                }
                /* For container <prosody>, defer pitch/rate emission
                 * until close. We split into 2 separate hints if both
                 * are set. */
                if (pitch_set || rate_set) {
                    h.kind = pitch_set ? SPFY_HINT_PITCH : SPFY_HINT_RATE;
                    if (pitch_set) h.v.pitch_st = pitch;
                    if (rate_set && !pitch_set) h.v.rate_pct = rate;
                    /* If both set, push pitch hint here and additionally
                     * stash the rate as a second simultaneous hint at
                     * close time. We use a side-stack (one extra hint
                     * when pitch+rate both present). */
                    if (pitch_set && rate_set) {
                        spfy_prosody_hint_t hr = {0};
                        hr.kind = SPFY_HINT_RATE;
                        hr.byte_start = (uint32_t)out_n;
                        hr.v.rate_pct = rate;
                        if (top < SSML_STACK_MAX) {
                            stack[top].tag_name   = "prosody";
                            stack[top].tag_len    = 7;
                            stack[top].byte_start = (uint32_t)out_n;
                            stack[top].pending    = hr;
                            ++top;
                        }
                    }
                }
            } else if (sn_streq_ci(name_s, "break", name_l) && name_l == 5) {
                /* Self-closing point-event regardless of /> form. */
                h.kind = SPFY_HINT_BREAK;
                h.v.break_ms = 0;
                while (ap < gt && parse_attr(&ap, gt, an, sizeof an,
                                              av, sizeof av)) {
                    if (sn_streq_ci(an, "time", 4))
                        h.v.break_ms = break_ms_from_str(av);
                }
                h.byte_end = h.byte_start;
                spfy_prosody_hints_add(out_hints, h);
                h.kind = (spfy_hint_kind_t)0xFFFF;     /* don't push */
                self_closing = 1;
            }

            if (!self_closing && h.kind != (spfy_hint_kind_t)0xFFFF) {
                if (top < SSML_STACK_MAX) {
                    stack[top].tag_name   = name_s;
                    stack[top].tag_len    = (uint8_t)name_l;
                    stack[top].byte_start = (uint32_t)out_n;
                    stack[top].pending    = h;
                    ++top;
                }
            }
        }
        p = gt + 1;
    }
    out[out_n] = 0;
    *out_plain = out;
    return SPFY_OK;
}
