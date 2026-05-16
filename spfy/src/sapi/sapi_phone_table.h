/* SAPI 5.4 en-US phone-ID -> ARPAbet -> SPR conversion. Shared by both
 * the 32-bit and 64-bit SAPI shims; included as a header-only module
 * so each DLL gets its own static copy (no shared TU).
 *
 * SAPI's <phoneme> tag flows through to ISpTTSEngine::Speak as a
 * SPVTEXTFRAG with State.eAction = SPVA_Pronounce and pPhoneIds set to
 * a WCHAR* array of numeric phone IDs (NUL-terminated). The IDs are
 * documented in MSDN's "SAPI Phoneme Set Reference" — for en-US the
 * scheme runs from 0x0001 (silence) to 0x0033 (zh), with stress markers
 * (1/2/3) and syllable / sentence punctuation interspersed.
 *
 * Our hosted FE accepts inline SPR phoneme syntax: text wrapped in
 * `\![...]` is treated as raw SPR (e.g. "\![.1ha.0lo]" = "hello").
 * We convert SAPI IDs -> ARPAbet -> SPR using the existing table in
 * bin/spfy_dumpwav.c, then wrap and feed to the synth pipeline. */

#ifndef SPFY_SAPI_PHONE_TABLE_H
#define SPFY_SAPI_PHONE_TABLE_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

/* SAPI 5.4 en-US phone IDs.
 *
 * Derived empirically by logging pPhoneIds from System.Speech and
 * matching to the user-supplied phoneme names. The actual layout is:
 *   0x0001..0x0007  reserved / silence / punctuation markers
 *   0x0008..0x0009  primary / secondary stress
 *   0x000A onwards  phones in alphabetical order (aa, ae, ah, ...)
 *
 * Verified anchors: h=0x001A, eh=0x0015, l=0x001F, ow=0x0023,
 *   primary-stress=0x0008. ARPAbet names match the SPR-table
 *   convention (e.g. "hh", not "h"), so the second stage
 *   (sapi_arpa_to_spr) finds them. */
static const struct {
    uint16_t    id;
    const char *tok;
} SAPI_PHONE_TABLE[] = {
    { 0x0001, "_"  },     /* silence */
    { 0x0002, "_"  },     /* placeholder — exact meaning unknown */
    { 0x0003, "_"  },
    { 0x0004, "-"  },     /* assumed syllable boundary */
    { 0x0005, "!"  },     /* sentence terminator (guess) */
    { 0x0006, "&"  },     /* word boundary (guess) */
    { 0x0007, ","  },     /* comma intonation (guess) */
    { 0x0008, "1"  },     /* primary stress (verified) */
    { 0x0009, "2"  },     /* secondary stress */
    { 0x000A, "aa" },
    { 0x000B, "ae" },
    { 0x000C, "ah" },
    { 0x000D, "ao" },
    { 0x000E, "aw" },
    { 0x000F, "ax" },
    { 0x0010, "ay" },
    { 0x0011, "b"  },
    { 0x0012, "ch" },
    { 0x0013, "d"  },
    { 0x0014, "dh" },
    { 0x0015, "eh" },     /* verified */
    { 0x0016, "er" },
    { 0x0017, "ey" },
    { 0x0018, "f"  },
    { 0x0019, "g"  },
    { 0x001A, "hh" },     /* verified — SAPI's "h" => ARPAbet "hh" */
    { 0x001B, "ih" },
    { 0x001C, "iy" },
    { 0x001D, "jh" },
    { 0x001E, "k"  },
    { 0x001F, "l"  },     /* verified */
    { 0x0020, "m"  },
    { 0x0021, "n"  },
    { 0x0022, "ng" },
    { 0x0023, "ow" },     /* verified */
    { 0x0024, "oy" },
    { 0x0025, "p"  },
    { 0x0026, "r"  },
    { 0x0027, "s"  },
    { 0x0028, "sh" },
    { 0x0029, "t"  },
    { 0x002A, "th" },
    { 0x002B, "uh" },
    { 0x002C, "uw" },
    { 0x002D, "v"  },
    { 0x002E, "w"  },
    { 0x002F, "y"  },
    { 0x0030, "z"  },
    { 0x0031, "zh" },
};

static const struct { const char *arpa; const char *spr; } SAPI_ARPA_TO_SPR[] = {
    {"aa","a"}, {"ae","A"}, {"ah","H"}, {"ao","c"}, {"aw","W"}, {"ax","x"},
    {"ay","Y"}, {"b","b"},  {"ch","C"}, {"d","d"},  {"dh","D"}, {"dx","F"},
    {"eh","E"}, {"el","l"}, {"en","N"}, {"er","R"}, {"ey","e"}, {"f","f"},
    {"g","g"},  {"hh","h"}, {"ih","I"}, {"ix","X"}, {"iy","i"}, {"jh","J"},
    {"k","k"},  {"l","l"},  {"m","m"},  {"n","n"},  {"ng","G"}, {"ow","o"},
    {"oy","O"}, {"p","p"},  {"r","r"},  {"s","s"},  {"sh","S"}, {"t","t"},
    {"th","T"}, {"uh","U"}, {"uw","u"}, {"v","v"},  {"w","w"},  {"y","y"},
    {"z","z"},  {"zh","Z"},
};
static const size_t SAPI_ARPA_TO_SPR_N =
    sizeof(SAPI_ARPA_TO_SPR) / sizeof(SAPI_ARPA_TO_SPR[0]);
static const size_t SAPI_PHONE_TABLE_N =
    sizeof(SAPI_PHONE_TABLE) / sizeof(SAPI_PHONE_TABLE[0]);

static const char *sapi_phone_id_to_token(uint16_t id)
{
    for (size_t i = 0; i < SAPI_PHONE_TABLE_N; ++i)
        if (SAPI_PHONE_TABLE[i].id == id) return SAPI_PHONE_TABLE[i].tok;
    return NULL;
}

static const char *sapi_arpa_to_spr(const char *arpa)
{
    for (size_t i = 0; i < SAPI_ARPA_TO_SPR_N; ++i)
        if (strcmp(SAPI_ARPA_TO_SPR[i].arpa, arpa) == 0)
            return SAPI_ARPA_TO_SPR[i].spr;
    return NULL;
}

static int sapi_arpa_is_vowel(const char *arpa)
{
    static const char *vowels[] = {
        "aa","ae","ah","ao","aw","ax","ay","eh","el","en","er","ey",
        "ih","ix","iy","ow","oy","uh","uw","xx", NULL
    };
    for (int i = 0; vowels[i]; ++i)
        if (strcmp(arpa, vowels[i]) == 0) return 1;
    return 0;
}

/* Convert a SAPI pPhoneIds WCHAR* stream to an SPR string suitable for
 * wrapping in `\![...]`. Returns the number of SPR chars written (excl.
 * trailing NUL), or 0 on failure / empty input.
 *
 * Algorithm mirrors bin/spfy_dumpwav.c::bal_to_spr: collect consonants
 * until a vowel, then peek the next token for a stress marker (1/2),
 * emit `.{stress}{consonants}{vowel}` for the syllable. SAPI's IDs use
 * the same convention: stress comes AFTER its vowel (or after the
 * syllable). Unrecognised tokens are dropped. */
static size_t sapi_phones_to_spr(const wchar_t *pids, char *out, size_t out_n)
{
    if (!pids || !out || out_n < 2) return 0;
    /* Debug: log the raw pPhoneIds bytes when SPFY_SAPI_PHONE_DEBUG is
     * set. Useful when adding a new phone-table entry (the table above
     * was derived empirically by logging IDs from System.Speech). */
    if (getenv("SPFY_SAPI_PHONE_DEBUG")) {
        FILE *fp = fopen("C:/tmp/_sapi_phone_log.txt", "a");
        if (fp) {
            fprintf(fp, "pPhoneIds (%lu chars): ",
                    (unsigned long)wcslen(pids));
            for (const wchar_t *p = pids; *p && (p - pids) < 64; ++p) {
                fprintf(fp, "%04X ", (unsigned)*p);
            }
            fprintf(fp, "\n");
            fclose(fp);
        }
    }
    /* Pass 1: gather tokens into an array, mapping IDs -> ARPAbet/markers. */
    enum { MAX_TOKS = 256 };
    const char *toks[MAX_TOKS];
    int ntoks = 0;
    for (const wchar_t *p = pids; *p && ntoks < MAX_TOKS; ++p) {
        uint16_t id = (uint16_t)*p;
        const char *t = sapi_phone_id_to_token(id);
        if (!t) continue;
        if (t[0] == '_' || t[0] == '!' || t[0] == ',' || t[0] == '.'
            || t[0] == '?' || t[0] == '&') {
            /* punctuation/markers: ignored for phoneme synth */
            continue;
        }
        toks[ntoks++] = t;
    }
    if (ntoks == 0) return 0;

    char *o = out;
    char *end = out + out_n - 1;
    int i = 0;
    while (i < ntoks && o < end) {
        if (strcmp(toks[i], "1") == 0 || strcmp(toks[i], "2") == 0
            || strcmp(toks[i], "-") == 0) {
            /* bare stress/boundary outside a syllable — skip */
            i++;
            continue;
        }
        /* Collect consonants up to (and including) a vowel. */
        int syl_start = i;
        while (i < ntoks) {
            const char *t = toks[i];
            if (strcmp(t, "1") == 0 || strcmp(t, "2") == 0
                || strcmp(t, "-") == 0) break;
            int is_vowel = sapi_arpa_is_vowel(t);
            i++;
            if (is_vowel) break;
        }
        /* Stress marker after the vowel? */
        int stress = 0;
        if (i < ntoks && (strcmp(toks[i], "1") == 0
                          || strcmp(toks[i], "2") == 0)) {
            stress = toks[i][0] - '0';
            i++;
        }
        /* Consume a trailing syllable boundary. */
        if (i < ntoks && strcmp(toks[i], "-") == 0) i++;

        /* Emit ".{stress}{consonants+vowel}". */
        int n = _snprintf(o, (size_t)(end - o), ".%d", stress);
        if (n < 0) break;
        o += n;
        for (int j = syl_start; j < i && o < end; ++j) {
            const char *t = toks[j];
            if (strcmp(t, "1") == 0 || strcmp(t, "2") == 0
                || strcmp(t, "-") == 0) continue;
            const char *spr = sapi_arpa_to_spr(t);
            if (!spr) continue;
            size_t slen = strlen(spr);
            if (o + slen >= end) break;
            memcpy(o, spr, slen);
            o += slen;
        }
    }
    *o = '\0';
    return (size_t)(o - out);
}

#endif /* SPFY_SAPI_PHONE_TABLE_H */
