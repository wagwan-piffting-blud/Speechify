/* Voice phoneset parser.
 *
 * Scans the decrypted VCF XML for the `tts.voiceCfg.phones` param and
 * builds an ordered ARPAbet-name <-> phone_id table. The position of
 * each <namedValue> in the list IS its phone_id.
 */

#include "phoneset.h"

#include <spfy/spfy.h>
#include "../common/log.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Bounded substring search: find `needle` in [s, s+n). */
static const char *bfind(const char *s, size_t n, const char *needle)
{
    size_t k = strlen(needle);
    if (k == 0 || k > n) return NULL;
    for (size_t i = 0; i + k <= n; ++i) {
        if (memcmp(s + i, needle, k) == 0) return s + i;
    }
    return NULL;
}

/* Skip whitespace forward. */
static const char *skip_ws(const char *p, const char *end)
{
    while (p < end && (unsigned char)*p <= ' ') ++p;
    return p;
}

/* Parse one <namedValue name="X"> ... </namedValue> entry starting at *p.
 * Returns 1 on success (and advances *p past the closing tag), 0 if the
 * next thing is not a namedValue (e.g. the param's closing tag), -1
 * on malformed input. */
static int parse_one_namedvalue(const char **pp, const char *end,
                                 spfy_phone_entry_t *out)
{
    const char *p = *pp;
    p = skip_ws(p, end);
    if (p >= end) return 0;

    /* Must start with "<namedValue". */
    static const char tag_open[] = "<namedValue";
    size_t to_n = sizeof tag_open - 1;
    if ((size_t)(end - p) < to_n || memcmp(p, tag_open, to_n) != 0) {
        return 0;
    }
    p += to_n;

    /* Find name=" attribute. */
    const char *gt = bfind(p, (size_t)(end - p), ">");
    if (!gt) return -1;

    const char *name_attr = bfind(p, (size_t)(gt - p), "name=");
    if (!name_attr) return -1;
    name_attr += 5;
    if (name_attr >= gt || (*name_attr != '"' && *name_attr != '\''))
        return -1;
    char quote = *name_attr++;
    const char *name_end = name_attr;
    while (name_end < gt && *name_end != quote) ++name_end;
    if (name_end >= gt) return -1;

    size_t name_len = (size_t)(name_end - name_attr);
    if (name_len == 0 || name_len + 1 > SPFY_PHONESET_NAME_MAX) return -1;
    memcpy(out->name, name_attr, name_len);
    out->name[name_len] = 0;

    /* Body between '>' and the closing tag holds feature words like
     * "voiced vowel". */
    const char *body = gt + 1;
    static const char tag_close[] = "</namedValue>";
    size_t tc_n = sizeof tag_close - 1;
    const char *body_end = bfind(body, (size_t)(end - body), tag_close);
    if (!body_end) return -1;

    /* Set features based on body keywords. */
    out->is_voiced = 0;
    out->is_vowel  = 0;
    if (bfind(body, (size_t)(body_end - body), "voiced")) out->is_voiced = 1;
    if (bfind(body, (size_t)(body_end - body), "vowel"))  out->is_vowel  = 1;
    /* Vowels are always voiced in the engine's encoding. */
    if (out->is_vowel) out->is_voiced = 1;

    *pp = body_end + tc_n;
    return 1;
}

int spfy_phoneset_load_from_vcf(const spfy_vcf_t *vcf,
                                 spfy_phoneset_t  *out)
{
    if (!vcf || !out) return SPFY_E_INVAL;
    memset(out, 0, sizeof *out);
    out->silence_phone_id = 0xFF;
    if (!vcf->xml_bytes || vcf->xml_n == 0) {
        spfy_log_err("phoneset: empty xml in VCF (n=%zu)", vcf->xml_n);
        return SPFY_E_FORMAT;
    }

    const char *xml = (const char *)vcf->xml_bytes;
    const char *end = xml + vcf->xml_n;

    /* Locate <param name="tts.voiceCfg.phones">. The closing quote
     * disambiguates from "tts.voiceCfg.phoneset" which is a strict
     * prefix match. */
    const char *anchor = bfind(xml, vcf->xml_n,
                                "tts.voiceCfg.phones\"");
    if (!anchor) {
        anchor = bfind(xml, vcf->xml_n, "tts.voiceCfg.phones'");
    }
    if (!anchor) {
        spfy_log_err("phoneset: 'tts.voiceCfg.phones' param not found");
        return SPFY_E_FORMAT;
    }
    /* Step forward to the closing '>' of <param ...>. */
    const char *gt = bfind(anchor, (size_t)(end - anchor), ">");
    if (!gt) return SPFY_E_FORMAT;
    const char *p = gt + 1;

    /* End boundary: </param>. */
    const char *param_end = bfind(p, (size_t)(end - p), "</param>");
    if (!param_end) return SPFY_E_FORMAT;

    /* Walk namedValues. */
    uint32_t pid = 0;
    while (p < param_end && pid < SPFY_PHONESET_MAX) {
        spfy_phone_entry_t entry = {0};
        int rc = parse_one_namedvalue(&p, param_end, &entry);
        if (rc <= 0) break;
        entry.phone_id = (uint8_t)pid;
        out->entries[pid] = entry;
        if (strcmp(entry.name, "pau") == 0) {
            out->silence_phone_id = entry.phone_id;
        }
        ++pid;
    }
    out->n_phones = pid;
    if (pid == 0) return SPFY_E_FORMAT;
    return SPFY_OK;
}

void spfy_phoneset_free(spfy_phoneset_t *ps)
{
    if (!ps) return;
    memset(ps, 0, sizeof *ps);
}

uint8_t spfy_phoneset_lookup(const spfy_phoneset_t *ps, const char *name)
{
    if (!ps || !name) return 0xFF;
    for (uint32_t i = 0; i < ps->n_phones; ++i) {
        if (strcmp(ps->entries[i].name, name) == 0)
            return ps->entries[i].phone_id;
    }
    return 0xFF;
}

const char *spfy_phoneset_name_of(const spfy_phoneset_t *ps, uint8_t phone_id)
{
    if (!ps || phone_id >= ps->n_phones) return NULL;
    return ps->entries[phone_id].name;
}
