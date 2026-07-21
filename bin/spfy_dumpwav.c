// cl spfy_dumpwav.c /Fe:spfy_dumpwav.exe
// usage: spfy_dumpwav.exe [options] "text to speak" out.wav
//
// Synthesis:
//   --phonemes    Enable phoneme/word mark output (.phn file alongside WAV)
//   --pron "..."  Synthesize raw SPR phonemes (see --help for symbol table)
//   --g2p         Print phoneme sequence for text (no audio output)
//   --16k         Use 16kHz output (default: 8kHz)
//   --dictionary F Apply a substitution dictionary (word/phrase -> SPR or text)
//
// Conversion (no server needed... just kidding, still needs server):
//   --bal2spr "..." Convert Balabolka/ARPAbet phonemes to SPR format
//   --spr2bal "..." Convert SPR phonemes to Balabolka/ARPAbet format
//
// Diagnostic:
//   --rawdump     Dump raw callback bytes to stderr

#define _CRT_SECURE_NO_WARNINGS
#include <winsock2.h>   /* must precede windows.h */
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "swi_min.h"

// crude wave helper
static void write_wav_header(FILE *f, uint32_t sampleRate, uint16_t bits, uint16_t chans, uint32_t dataBytes) {
    uint32_t riffSize = 36 + dataBytes;
    fwrite("RIFF",1,4,f); fwrite(&riffSize,4,1,f); fwrite("WAVE",1,4,f);
    fwrite("fmt ",1,4,f); uint32_t fmtSize=16; fwrite(&fmtSize,4,1,f);
    uint16_t audioFmt=1; fwrite(&audioFmt,2,1,f);
    fwrite(&chans,2,1,f);
    fwrite(&sampleRate,4,1,f);
    uint32_t byteRate = sampleRate * chans * (bits/8); fwrite(&byteRate,4,1,f);
    uint16_t blockAlign = chans * (bits/8); fwrite(&blockAlign,2,1,f);
    fwrite(&bits,2,1,f);
    fwrite("data",1,4,f); fwrite(&dataBytes,4,1,f);
}

// ARPAbet -> SPR conversion table
static const struct { const char *arpabet; const char *spr; } ARPA_TO_SPR[] = {
    {"aa","a"}, {"ae","A"}, {"ah","H"}, {"ao","c"}, {"aw","W"}, {"ax","x"},
    {"ay","Y"}, {"b","b"}, {"ch","C"}, {"d","d"}, {"dh","D"}, {"dx","F"},
    {"eh","E"}, {"el","l"}, {"en","N"}, {"er","R"}, {"ey","e"}, {"f","f"},
    {"g","g"}, {"hh","h"}, {"ih","I"}, {"ix","X"}, {"iy","i"}, {"jh","J"},
    {"k","k"}, {"l","l"}, {"m","m"}, {"n","n"}, {"ng","G"}, {"ow","o"},
    {"oy","O"}, {"p","p"}, {"pau","_"}, {"r","r"}, {"s","s"}, {"sh","S"},
    {"t","t"}, {"th","T"}, {"uh","U"}, {"uw","u"}, {"v","v"}, {"w","w"},
    {"xx","x"}, {"y","y"}, {"z","z"}, {"zh","Z"}, {NULL,NULL}
};

static const char *arpabet_to_spr(const char *arpa) {
    for (int i = 0; ARPA_TO_SPR[i].arpabet; i++) {
        if (strcmp(arpa, ARPA_TO_SPR[i].arpabet) == 0)
            return ARPA_TO_SPR[i].spr;
    }
    return "?";
}

// Check if an ARPAbet code is a vowel
static int is_vowel_arpabet(const char *arpa) {
    static const char *vowels[] = {
        "aa","ae","ah","ao","aw","ax","ay","eh","el","en","er","ey",
        "ih","ix","iy","ow","oy","uh","uw","xx",NULL
    };
    for (int i = 0; vowels[i]; i++)
        if (strcmp(arpa, vowels[i]) == 0) return 1;
    return 0;
}

// Check if an SPR symbol is a vowel
static int is_vowel_spr(char c) {
    return (c=='a'||c=='A'||c=='H'||c=='c'||c=='W'||c=='x'||c=='X'||
            c=='Y'||c=='O'||c=='i'||c=='I'||c=='e'||c=='E'||c=='R'||
            c=='u'||c=='U'||c=='o');
}

// Convert Balabolka/Balcon phoneme string to SPR format
// Balabolka format: stress marker (1/2) comes AFTER the vowel it modifies
// Input:  "p aa 1 t ax w aa t uw m iy" (Pottawattamie)
// Output: ".1pa.0tAx.0wa.0tu.0mi"
static void bal_to_spr(const char *input, char *output, int maxlen) {
    char buf[4096];
    strncpy(buf, input, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    // First pass: tokenize and collect into an array
    #define MAX_TOKS 256
    char *toks[MAX_TOKS];
    int ntoks = 0;
    char *t = strtok(buf, " ");
    while (t && ntoks < MAX_TOKS) {
        toks[ntoks++] = t;
        t = strtok(NULL, " ");
    }

    // Build syllable groups: each syllable = optional consonants + vowel
    // Stress in Balabolka follows the vowel: "p aa 1" = stressed "paa"
    char *out = output;
    char *end = output + maxlen - 1;
    int i = 0;

    while (i < ntoks && out < end) {
        // Skip special markers at this level
        if (strcmp(toks[i], "-") == 0 || strcmp(toks[i], "&") == 0 ||
            strcmp(toks[i], ".") == 0 || strcmp(toks[i], ",") == 0 ||
            strcmp(toks[i], "!") == 0 || strcmp(toks[i], "?") == 0 ||
            strcmp(toks[i], "_") == 0) {
            i++;
            continue;
        }
        // Skip bare stress markers (already consumed by lookahead below)
        if (strcmp(toks[i], "0") == 0 || strcmp(toks[i], "1") == 0 ||
            strcmp(toks[i], "2") == 0) {
            i++;
            continue;
        }

        // Collect consonants until we hit a vowel
        int syl_start = i;
        while (i < ntoks) {
            const char *lookup = toks[i];
            if (strcmp(lookup, "h") == 0) lookup = "hh";

            if (strcmp(toks[i],"0")==0 || strcmp(toks[i],"1")==0 ||
                strcmp(toks[i],"2")==0 || strcmp(toks[i],"-")==0 ||
                strcmp(toks[i],"&")==0 || strcmp(toks[i],".")==0 ||
                strcmp(toks[i],",")==0 || strcmp(toks[i],"!")==0 ||
                strcmp(toks[i],"?")==0 || strcmp(toks[i],"_")==0) {
                break;
            }

            i++;

            // If this was a vowel, check for following stress marker
            if (is_vowel_arpabet(lookup)) {
                break;
            }
        }

        // Check if next token is a stress marker (applies to this syllable)
        int stress = 0;
        if (i < ntoks && (strcmp(toks[i],"1")==0 || strcmp(toks[i],"2")==0)) {
            stress = toks[i][0] - '0';
            i++;
        }

        // Write syllable: .{stress}{consonants}{vowel}
        out += _snprintf(out, end - out, ".%d", stress);
        for (int j = syl_start; j < i; j++) {
            if (strcmp(toks[j],"0")==0 || strcmp(toks[j],"1")==0 ||
                strcmp(toks[j],"2")==0) continue;
            const char *lookup = toks[j];
            if (strcmp(lookup, "h") == 0) lookup = "hh";
            const char *spr = arpabet_to_spr(lookup);
            if (strcmp(spr, "?") != 0 && strcmp(spr, "_") != 0)
                out += _snprintf(out, end - out, "%s", spr);
        }
    }

    *out = '\0';
    #undef MAX_TOKS
}

// Case-insensitive substring search (used to skip a <pron> element body).
static const char *find_ci(const char *hay, const char *needle) {
    size_t nl = strlen(needle);
    if (nl == 0) return hay;
    for (; *hay; hay++) {
        size_t i = 0;
        while (i < nl && hay[i] &&
               tolower((unsigned char)hay[i]) == tolower((unsigned char)needle[i]))
            i++;
        if (i == nl) return hay;
    }
    return NULL;
}

// Case-insensitive equality of the first n characters of a and b.
static int ci_eqn(const char *a, const char *b, int n) {
    for (int i = 0; i < n; i++)
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i]))
            return 0;
    return 1;
}

// Extract attribute `name` (case-insensitive) from an open tag's attribute
// range [ts, te). Copies the quoted value into val (NUL-terminated). Returns 1
// on success. Requires a whitespace/tag-start boundary before the name so a
// substring of another attribute's value cannot match.
static int get_attr(const char *ts, const char *te, const char *name,
                    char *val, int vlen) {
    int nl = (int)strlen(name);
    for (const char *a = ts; a + nl <= te; a++) {
        if (a != ts && !isspace((unsigned char)a[-1])) continue;
        if (!ci_eqn(a, name, nl)) continue;
        const char *e = a + nl;
        while (e < te && isspace((unsigned char)*e)) e++;
        if (e >= te || *e != '=') continue;
        e++;
        while (e < te && isspace((unsigned char)*e)) e++;
        if (e >= te || (*e != '"' && *e != '\'')) continue;
        char q = *e++;
        int i = 0;
        while (e < te && *e != q && i < vlen - 1) val[i++] = *e++;
        val[i] = '\0';
        return 1;
    }
    return 0;
}

static int clamp_int(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

// SSML break "time" ("500ms", "0.75s", "500") -> milliseconds (1..32767).
static int break_to_ms(const char *t) {
    while (isspace((unsigned char)*t)) t++;
    double v = atof(t);
    const char *u = t;
    while (*u == '+' || *u == '-' || *u == '.' || isdigit((unsigned char)*u)) u++;
    while (isspace((unsigned char)*u)) u++;
    if ((*u == 's' || *u == 'S') && (u[1] != 'm' && u[1] != 'M'))
        v *= 1000.0;                       // seconds (not "ms")
    return clamp_int((int)(v + 0.5), 1, 32767);
}

// SAPI rate absspeed/speed (-10..10) -> Speechify rate percent (33..300):
// round(100 * 3^(S/10)), which lines the SAPI slider up with the guide's
// documented "1/3 to 3x" rate range. Tabled to avoid pulling in <math.h>.
static int sapi_rate_to_pct(int s) {
    static const int tbl[21] = {
        33, 37, 42, 47, 52, 58, 65, 72, 80, 90, 100,
        112, 125, 139, 155, 173, 192, 215, 240, 268, 300
    };
    return tbl[clamp_int(s, -10, 10) + 10];
}

// Expand Balabolka / SAPI 5 / SSML control tags into Speechify inline \! codes,
// which the engine speaks directly (the \! notation already works; this just
// accepts the friendlier markup). Applied to CLI text and each --batch line.
// Recognized case-insensitively, anywhere in the text; the element body is
// kept and spoken normally except for <pron> (whose body is a display
// fallback and is dropped):
//   <pron sym="ph"/> | <pron sym="ph">word</pron>   -> \![SPR]   (body dropped)
//   <silence msec="N"/>                              -> \!pN      (pause)
//   <break time="500ms"|"0.5s"/>                     -> \!pN
//   <bookmark mark="X"/> | <mark name="X"/>          -> \!bmX
//   <volume level="N"> .. </volume>                  -> \!vdN .. \!vdr
//   <rate absspeed|speed="S"> .. </rate>  (S -10..10)-> \!rdPCT .. \!rdr
//   <prosody rate="N%" volume="M%"> .. </prosody>    -> \!rdN\!vdM .. resets
//   <spell> .. </spell> ,
//   <say-as interpret-as="characters"> .. </say-as>  -> \!tsc .. \!ts0
//   <emph>/<emphasis>/<pitch ...>                      stripped (no engine tag)
// Unrecognized "<...>" is copied through verbatim. The \!eos / \!ny / \!di
// tags have no common Balabolka markup; use their \! forms directly.
static void expand_tags(const char *in, char *out, size_t maxlen) {
    const char *p = in;
    char *o = out, *oend = out + maxlen - 1;
    int pros_rate = 0, pros_vol = 0;   // attrs the most recent <prosody> set
    int sayas_spell = 0;               // most recent <say-as> was spell-ish
    char val[1024];
    // Emit a \! code padded with spaces. The engine requires a tag be preceded
    // by whitespace and NOT followed by an alphanumeric, so a code glued to
    // adjacent text (e.g. <volume level="80">loud) would be treated as one
    // unknown tag and swallow the word. Extra/leading spaces collapse harmlessly.
    #define EMIT(...) do {                                          \
            if (o < oend) *o++ = ' ';                               \
            o += _snprintf(o, (size_t)(oend - o), __VA_ARGS__);     \
            if (o < oend) *o++ = ' ';                               \
        } while (0)
    while (*p && o < oend) {
        if (*p != '<') { *o++ = *p++; continue; }
        const char *gt = strchr(p, '>');
        if (!gt) { *o++ = *p++; continue; }
        const char *nm = p + 1;
        int closing = 0;
        if (*nm == '/') { closing = 1; nm++; }
        const char *ne = nm;
        while (isalnum((unsigned char)*ne) || *ne == '-') ne++;
        int nl = (int)(ne - nm);
        if (nl == 0) { *o++ = *p++; continue; }   // "<" not starting a tag name
        const char *bb = gt - 1;
        while (bb > nm && isspace((unsigned char)*bb)) bb--;
        int selfclose = (*bb == '/');
        const char *ts = ne, *te = gt;            // attribute range
        #define TAG(s) (nl == (int)(sizeof(s) - 1) && ci_eqn(nm, s, nl))
        int handled = 1;
        if (TAG("pron")) {
            if (!closing) {
                if (get_attr(ts, te, "sym", val, sizeof val) && val[0]) {
                    char spr[4096];
                    bal_to_spr(val, spr, sizeof spr);
                    EMIT("\\![%s]", spr);
                }
                if (!selfclose) {                 // drop the display-fallback body
                    const char *c = find_ci(gt + 1, "</pron>");
                    p = c ? c + 7 : gt + 1;
                    continue;
                }
            }
        } else if (TAG("silence")) {
            if (!closing && get_attr(ts, te, "msec", val, sizeof val))
                EMIT("\\!p%d", clamp_int(atoi(val), 1, 32767));
        } else if (TAG("break")) {
            if (!closing && get_attr(ts, te, "time", val, sizeof val))
                EMIT("\\!p%d", break_to_ms(val));
        } else if (TAG("bookmark") || TAG("mark")) {
            const char *an = TAG("bookmark") ? "mark" : "name";
            if (!closing && get_attr(ts, te, an, val, sizeof val) && val[0])
                EMIT("\\!bm%s", val);
        } else if (TAG("volume")) {
            if (closing)
                EMIT("\\!vdr");
            else if (get_attr(ts, te, "level", val, sizeof val))
                EMIT("\\!vd%d", clamp_int(atoi(val), 0, 500));
        } else if (TAG("rate")) {
            if (closing)
                EMIT("\\!rdr");
            else {
                int s = 0;
                if (get_attr(ts, te, "absspeed", val, sizeof val) ||
                    get_attr(ts, te, "speed", val, sizeof val))
                    s = atoi(val);
                EMIT("\\!rd%d", sapi_rate_to_pct(s));
            }
        } else if (TAG("prosody")) {
            if (closing) {
                if (pros_rate) EMIT("\\!rdr");
                if (pros_vol)  EMIT("\\!vdr");
                pros_rate = pros_vol = 0;
            } else {
                if (get_attr(ts, te, "rate", val, sizeof val) &&
                    isdigit((unsigned char)val[0])) {
                    EMIT("\\!rd%d", clamp_int(atoi(val), 33, 300));
                    pros_rate = 1;
                }
                if (get_attr(ts, te, "volume", val, sizeof val) &&
                    isdigit((unsigned char)val[0])) {
                    EMIT("\\!vd%d", clamp_int(atoi(val), 0, 500));
                    pros_vol = 1;
                }
            }
        } else if (TAG("spell")) {
            EMIT("%s", closing ? "\\!ts0" : "\\!tsc");
        } else if (TAG("say-as")) {
            if (closing) {
                if (sayas_spell) EMIT("\\!ts0");
                sayas_spell = 0;
            } else {
                int spell = 0;
                if (get_attr(ts, te, "interpret-as", val, sizeof val))
                    spell = (find_ci(val, "char") != NULL) ||
                            (find_ci(val, "spell") != NULL);
                if (spell) {
                    EMIT("\\!tsc");
                    sayas_spell = 1;
                }
                // other interpret-as: strip the tag, keep the body
            }
        } else if (TAG("emph") || TAG("emphasis") || TAG("pitch")) {
            // No Speechify \! equivalent -> strip the tag, keep any body text.
        } else {
            handled = 0;
        }
        #undef TAG
        if (handled) { p = gt + 1; continue; }
        *o++ = *p++;   // unrecognized tag: copy '<' literally and keep scanning
    }
    #undef EMIT
    *o = '\0';
}

// --------------------------------------------------------------------------
// Substitution dictionary (Speechify/NWS "vip_dicta.txt" style)
//
// Each non-comment line is  key<sep>value  where <sep> is the first comma or
// tab.  '#' starts a comment (whole-line or inline) and is stripped.  Keys may
// be single words, multi-word phrases, or digit patterns where '@' matches one
// decimal digit.  '@' in the value is back-filled with the captured digits, in
// order (e.g.  "@@@ AM" -> "@:@@ Ay em"  turns "830 AM" into "8:30 Ay em").
// On duplicate keys an SPR value ( \![...] ) is preferred over a plain respell.
// --------------------------------------------------------------------------
#define DICT_MAX     2048
#define DICT_KEYLEN  96
#define DICT_VALLEN  256
#define DICT_MAXCAPS 64

typedef struct {
    char key[DICT_KEYLEN];
    char val[DICT_VALLEN];
} DictEntry;

static DictEntry g_dict[DICT_MAX];
static int g_dictCount = 0;

static int dict_val_is_spr(const char *v) {
    return strstr(v, "\\![") != NULL;
}

static char *dict_trim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    size_t n = strlen(s);
    while (n > 0 && (s[n-1]==' '||s[n-1]=='\t'||s[n-1]=='\r'||s[n-1]=='\n'))
        s[--n] = '\0';
    return s;
}

// Returns number of entries loaded, or -1 if the file could not be opened.
static int dict_load(const wchar_t *path) {
    FILE *fp = _wfopen(path, L"r");
    if (!fp) return -1;

    char line[1024];
    while (fgets(line, sizeof(line), fp)) {
        // Strip '#' comments (whole-line or trailing).
        char *hash = strchr(line, '#');
        if (hash) *hash = '\0';

        // Separator = first comma or tab (handles the lone tab-delimited line).
        char *sep = NULL;
        for (char *p = line; *p; p++) {
            if (*p == ',' || *p == '\t') { sep = p; break; }
        }
        if (!sep) continue;
        *sep = '\0';

        char *k = dict_trim(line);
        char *v = dict_trim(sep + 1);
        if (!*k || !*v) continue;

        int newSpr = dict_val_is_spr(v);
        int found = -1;
        for (int i = 0; i < g_dictCount; i++) {
            if (_stricmp(g_dict[i].key, k) == 0) { found = i; break; }
        }
        if (found >= 0) {
            // SPR always wins; a later SPR beats an earlier one; a plain
            // respell only replaces another plain respell.
            if (newSpr || !dict_val_is_spr(g_dict[found].val)) {
                strncpy(g_dict[found].val, v, DICT_VALLEN - 1);
                g_dict[found].val[DICT_VALLEN - 1] = '\0';
            }
        } else if (g_dictCount < DICT_MAX) {
            strncpy(g_dict[g_dictCount].key, k, DICT_KEYLEN - 1);
            g_dict[g_dictCount].key[DICT_KEYLEN - 1] = '\0';
            strncpy(g_dict[g_dictCount].val, v, DICT_VALLEN - 1);
            g_dict[g_dictCount].val[DICT_VALLEN - 1] = '\0';
            g_dictCount++;
        }
    }
    fclose(fp);
    return g_dictCount;
}

// Match key against text[0..]; '@' matches a single digit (recorded in caps[]).
// Returns matched length in text, or -1 on mismatch.
static int dict_match(const char *key, const char *text, char *caps, int *ncaps) {
    int ti = 0, nc = 0;
    for (const char *k = key; *k; k++) {
        char tc = text[ti];
        if (tc == '\0') return -1;
        if (*k == '@') {
            if (tc < '0' || tc > '9') return -1;
            if (nc < DICT_MAXCAPS) caps[nc] = tc;
            nc++;
        } else if (tolower((unsigned char)tc) != tolower((unsigned char)*k)) {
            return -1;
        }
        ti++;
    }
    *ncaps = nc;
    return ti;
}

// Apply the loaded dictionary to 'in', writing to 'out'.  Single pass,
// longest-match-first, token boundaries required on both sides.  Output is
// never re-scanned, so replacements cannot chain.  Returns substitution count.
static int dict_apply(const char *in, char *out, size_t outsz) {
    int n = (int)strlen(in);
    int pos = 0;
    size_t oi = 0;
    int subs = 0;

    while (pos < n) {
        int bestLen = 0, bestIdx = -1, bestNcaps = 0;
        char bestCaps[DICT_MAXCAPS] = {0};

        // Left token boundary: previous char must not be alphanumeric.
        int leftOk = (pos == 0) || !isalnum((unsigned char)in[pos-1]);
        if (leftOk) {
            for (int i = 0; i < g_dictCount; i++) {
                char caps[DICT_MAXCAPS];
                int nc = 0;
                int m = dict_match(g_dict[i].key, in + pos, caps, &nc);
                if (m <= 0) continue;
                // Right token boundary: next char must not be alphanumeric.
                if (isalnum((unsigned char)in[pos + m])) continue;
                if (m > bestLen) {
                    bestLen = m; bestIdx = i; bestNcaps = nc;
                    memcpy(bestCaps, caps, sizeof(caps));
                }
            }
        }

        if (bestIdx >= 0) {
            const char *v = g_dict[bestIdx].val;
            int ci = 0;
            for (const char *p = v; *p; p++) {
                char c = (*p == '@') ? ((ci < bestNcaps) ? bestCaps[ci++] : '@') : *p;
                if (oi + 1 < outsz) out[oi++] = c;
            }
            pos += bestLen;
            subs++;
        } else {
            if (oi + 1 < outsz) out[oi++] = in[pos];
            pos++;
        }
    }
    out[oi] = '\0';
    return subs;
}

#define MAX_G2P_PHONES 256

typedef struct {
    FILE *out;
    FILE *phonemeOut;           // .phn file (NULL if disabled)
    volatile LONG gotAudio;
    volatile LONG done;
    HANDLE doneEvent;           // signaled when synthesis completes
    uint32_t bytesWritten;
    int enablePhonemes;
    int rawDump;
    int g2pMode;
    uint32_t sampleRate;
    // G2P collection
    struct { char name[8]; uint32_t stress; } g2pPhones[MAX_G2P_PHONES];
    int g2pCount;
} Ctx;

static SWIttsResult SWIAPI cb(SWIttsPort port, int status, void *data, void *user) {
    Ctx *ctx = (Ctx*)user;

    // Debug/log message from engine -- only surfaced in --rawdump mode, since
    // it is noise (and often uninitialized garbage) on a failed connect.
    if (port == (SWIttsPort)-1) {
        if (data && ctx && ctx->rawDump) {
            fwprintf(stderr, L"DEBUG (status=%d): %hs\n", status, (char*)data);
        }
        return 0;
    }

    // Raw byte dump for diagnostics
    if (ctx->rawDump && data && (status == SWITTS_CB_PHONEMEMARK || status == SWITTS_CB_WORDMARK)) {
        uint8_t *bytes = (uint8_t*)data;
        fprintf(stderr, "MARK status=%d data=%p\n", status, data);
        fprintf(stderr, "  hex: ");
        for (int i = 0; i < 48; i++) fprintf(stderr, "%02x ", bytes[i]);
        fprintf(stderr, "\n");
        // Also try interpreting as u32 + string
        fprintf(stderr, "  u32[0]=%u u32[1]=%u\n",
            *(uint32_t*)(bytes), *(uint32_t*)(bytes+4));
        // Try to print as string starting at various offsets
        for (int off = 4; off <= 12; off += 4) {
            fprintf(stderr, "  str@%d: \"%.8s\"\n", off, bytes+off);
        }
    }

    // Phoneme mark callback (official struct: SWIttsPhonemeMark from SWItts.h)
    if (status == SWITTS_CB_PHONEMEMARK && data) {
        SWIttsPhonemeMark *pm = (SWIttsPhonemeMark*)data;

        // Collect for G2P mode
        if (ctx->g2pMode && ctx->g2pCount < MAX_G2P_PHONES) {
            strncpy(ctx->g2pPhones[ctx->g2pCount].name, pm->name, 7);
            ctx->g2pPhones[ctx->g2pCount].name[7] = '\0';
            ctx->g2pPhones[ctx->g2pCount].stress = pm->stress;
            ctx->g2pCount++;
        }

        // Write to .phn file
        if (ctx->phonemeOut) {
            uint32_t endSample = pm->sampleNumber + pm->duration;
            fprintf(ctx->phonemeOut, "%u\t%u\t%s\t%u\n",
                    pm->sampleNumber, endSample, pm->name, pm->stress);
            fflush(ctx->phonemeOut);
        }
        return 0;
    }

    // Word mark callback (official struct: SWIttsWordMark from SWItts.h)
    if (status == SWITTS_CB_WORDMARK && data) {
        if (ctx->phonemeOut) {
            SWIttsWordMark *wm = (SWIttsWordMark*)data;

            fprintf(ctx->phonemeOut, "# word\t%u\ttext_off=%u\ttext_len=%u\n",
                    wm->sampleNumber, wm->offset, wm->length);
            fflush(ctx->phonemeOut);
        }
        return 0;
    }

    // Audio packet
    if (data) {
        SWIttsAudioPacket *p = (SWIttsAudioPacket*)data;
        if (p->samples && p->numBytes && p->numBytes < (1u<<26)) {
            // network byte order -> little endian for 16-bit PCM
            uint8_t *buf = (uint8_t*)p->samples;
            for (unsigned i=0;i+1<p->numBytes;i+=2) {
                uint8_t t=buf[i]; buf[i]=buf[i+1]; buf[i+1]=t;
            }
            fwrite(buf,1,p->numBytes,ctx->out);
            ctx->bytesWritten += p->numBytes;
            InterlockedExchange(&ctx->gotAudio, 1);
            return 0;
        } else {
            fwprintf(stderr, L"INFO: Non-audio packet on port %d (status=%d)\n", (int)port, status);
        }
    } else {
        // NULL data: start/end/stopped/portclosed/errors
        if (ctx->gotAudio) {
            InterlockedExchange(&ctx->done, 1);
            SetEvent(ctx->doneEvent);
        } else {
            fwprintf(stderr, L"INFO: NULL data on port %d (status=%d) done=%d\n",
                (int)port, status, (int)ctx->done);
            if (status != 0) {
                InterlockedExchange(&ctx->done, 1);
                SetEvent(ctx->doneEvent);
            }
        }
    }
    return 0;
}

// Quick TCP reachability probe against the local Speechify server. Returns 1
// if 127.0.0.1:port accepts a connection within timeout_ms, else 0. Used to
// fail fast with a clear message instead of loading the client DLL and
// leaving a header-only WAV plus engine debug noise on stderr. If winsock
// itself is unavailable we return 1 (don't block on a probe we can't run).
static int server_reachable(unsigned short port, int timeout_ms) {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return 1;
    int ok = 0;
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s != INVALID_SOCKET) {
        u_long nb = 1;
        ioctlsocket(s, FIONBIO, &nb);              // non-blocking connect
        struct sockaddr_in a;
        memset(&a, 0, sizeof a);
        a.sin_family = AF_INET;
        a.sin_port = htons(port);
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        connect(s, (struct sockaddr *)&a, sizeof a);   // returns WSAEWOULDBLOCK
        fd_set wf;
        FD_ZERO(&wf);
        FD_SET(s, &wf);
        struct timeval tv;
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        if (select(0, NULL, &wf, NULL, &tv) > 0) {
            int err = 0, len = (int)sizeof err;
            getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&err, &len);
            ok = (err == 0);
        }
        closesocket(s);
    }
    WSACleanup();
    return ok;
}

// Read an entire text file into a malloc'd, NUL-terminated UTF-8 buffer (no
// length cap beyond available memory). Accepts UTF-8 (a leading BOM is
// stripped) and UTF-16 LE (converted). Returns NULL on error; caller frees.
static char *read_file_utf8(const wchar_t *path) {
    FILE *f = _wfopen(path, L"rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return NULL; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[rd] = '\0';
    // UTF-16 LE (Windows "Unicode" .txt) -> UTF-8.
    if (rd >= 2 && (unsigned char)buf[0] == 0xFF && (unsigned char)buf[1] == 0xFE) {
        const wchar_t *w = (const wchar_t *)(buf + 2);
        int wn = (int)((rd - 2) / 2);
        int un = WideCharToMultiByte(CP_UTF8, 0, w, wn, NULL, 0, NULL, NULL);
        char *u8 = (char *)malloc((size_t)(un > 0 ? un : 1) + 1);
        if (!u8) { free(buf); return NULL; }
        WideCharToMultiByte(CP_UTF8, 0, w, wn, u8, un, NULL, NULL);
        u8[un > 0 ? un : 0] = '\0';
        free(buf);
        return u8;
    }
    // Strip a UTF-8 BOM if present.
    if (rd >= 3 && (unsigned char)buf[0] == 0xEF &&
        (unsigned char)buf[1] == 0xBB && (unsigned char)buf[2] == 0xBF) {
        memmove(buf, buf + 3, rd - 3 + 1);
    }
    return buf;
}

static void print_usage(const wchar_t *exe) {
    fwprintf(stderr,
        L"usage: %ls [options] \"text to speak\" out.wav\n"
        L"       %ls [options] -f input.txt out.wav   (read text from a file)\n"
        L"\n"
        L"Text may be any length; -f/--file reads a whole file (UTF-8 or\n"
        L"UTF-16 LE) with no size cap. CLI arg text is limited only by the OS\n"
        L"command line.\n"
        L"\n"
        L"The input text (CLI arg or a --batch line) may contain inline\n"
        L"Balabolka/SAPI/SSML control tags, expanded to Speechify \\! codes:\n"
        L"  <pron sym=\"f r ay 1 d ey 2\"/> or <pron sym=..>friday</pron>  \\![SPR]\n"
        L"  <silence msec=\"300\"/>  <break time=\"0.5s\"/>                  pause \\!p\n"
        L"  <bookmark mark=\"X\"/>  <mark name=\"X\"/>                       \\!bmX\n"
        L"  <volume level=\"80\">text</volume>                            \\!vd80..\\!vdr\n"
        L"  <rate absspeed=\"5\">text</rate>  (SAPI -10..10)              \\!rd..\\!rdr\n"
        L"  <prosody rate=\"150%%\" volume=\"80%%\">text</prosody>           \\!rd/\\!vd\n"
        L"  <spell>abc</spell>  <say-as interpret-as=\"characters\">..     \\!tsc..\\!ts0\n"
        L"pron sym uses Balabolka/ARPAbet (stress digit AFTER the vowel).\n"
        L"Non-tag text is spoken normally, so a mix works. \\!eos/\\!ny/\\!di\n"
        L"have no common markup -- use their \\! forms directly.\n"
        L"\n"
        L"Options:\n"
        L"  --batch DIR     Read \"<id>\\t<text>\" lines from stdin; write\n"
        L"                  DIR\\<id>.wav per line and print \"DONE <id>\".\n"
        L"                  ~50x faster than re-invoking per phrase. Each\n"
        L"                  utterance gets a fresh port so engine session\n"
        L"                  state cannot leak between them.\n"
        L"  --batch-shared-port\n"
        L"                  Keep ONE port for the whole batch. Faster, but\n"
        L"                  the engine then carries state across utterances\n"
        L"                  and Frida traces differ. Audio only.\n"
        L"  --graceful-term Call SWIttsTerm() on exit (adds ~4.6 s; only\n"
        L"                  needed to debug a suspected resource leak)\n"
        L"  --phonemes      Write phoneme timing to .phn file alongside WAV\n"
        L"  --pron \"...\"    Synthesize raw SPR phonemes (see symbol table below)\n"
        L"  --g2p           Print phoneme sequence for text (ARPAbet + SPR)\n"
        L"  --bal2spr \"...\" Convert Balabolka phonemes to SPR format\n"
        L"  --spr2bal \"...\" Convert SPR phonemes to Balabolka/ARPAbet format\n"
        L"  --dictionary F  Apply a substitution dictionary (e.g. vip_dicta.txt)\n"
        L"  --rawdump       Dump raw callback bytes to stderr (diagnostic)\n"
        L"  --16k           Use 16kHz output (default: 8kHz)\n"
        L"\n"
        L"\n"
        L"SPR phoneme symbols (case-sensitive):\n"
        L"  Vowels:  a=aa A=ae H=ah c=ao W=aw x=ax Y=ay i=iy I=ih\n"
        L"           e=ey E=eh R=er u=uw U=uh o=ow X=ix O=oy\n"
        L"  Cons:    p b t d k g f v s z h m n l r w y (same as ARPAbet)\n"
        L"           C=ch J=jh T=th D=dh S=sh Z=zh G=ng N=en F=dx\n"
        L"  Stress:  1=primary 2=secondary 0=none  Syllable: . (period)\n"
        L"\n"
        L"Example: --pron \".0Dx.1wE.0DR\" synthesizes \"the weather\"\n",
        exe, exe);
}

// Exit without running DLL_PROCESS_DETACH.
//
// Skipping SWIttsTerm() is not enough on its own: SWItts.dll does the
// same ~4.6 s of cleanup from its detach handler, so a normal return from
// wmain() (or exit()/_exit(), both of which route through ExitProcess and
// therefore still notify loaded DLLs) pays the cost anyway. Only
// TerminateProcess bypasses detach entirely.
//
// Safe here because every file this tool owns is fclose'd before the
// call, the WAV is fully finalized on disk, and the server-side port was
// already released by SWIttsClosePort. Use --graceful-term to take the
// slow, fully-unwound path instead.
static void fast_exit(int code) {
    fflush(stdout);
    fflush(stderr);
    TerminateProcess(GetCurrentProcess(), (UINT)code);
}

// Synthesize one utterance on an already-open port and write it to
// outPath as a complete WAV. Resets the per-utterance callback state so
// the same Ctx/port can be reused across a whole batch. Returns 0 on ok.
static int synth_one(SWIttsAPI *api, SWIttsPort port, Ctx *ctx,
                     const char *speakText, const wchar_t *outPath,
                     uint32_t sampleRate) {
    FILE *f = _wfopen(outPath, L"wb+");
    if (!f) {
        fwprintf(stderr, L"ERROR: cannot open %ls\n", outPath);
        return -1;
    }
    write_wav_header(f, sampleRate, 16, 1, 0);

    ctx->out          = f;
    ctx->bytesWritten = 0;
    ctx->g2pCount     = 0;
    InterlockedExchange(&ctx->gotAudio, 0);
    InterlockedExchange(&ctx->done, 0);
    ResetEvent(ctx->doneEvent);

    const char *contentType = "text/plain;charset=utf-8";
    if (strncmp(speakText, "<speak", 6) == 0 || strncmp(speakText, "<?xml", 5) == 0)
        contentType = "application/synthesis+ssml";

    int rc = 0;
    if (api->Speak(port, (const unsigned char *)speakText,
                   (unsigned)strlen(speakText), contentType) != 0) {
        fprintf(stderr, "SWIttsSpeak failed\n");
        rc = -1;
    } else {
        // Scale the wait with the utterance length (60 s floor + ~10 ms/char,
        // capped at 24 h) so long lines are not truncated.
        unsigned long long wl = 60000ULL + (unsigned long long)strlen(speakText) * 10ULL;
        if (wl > 86400000ULL) wl = 86400000ULL;
        WaitForSingleObject(ctx->doneEvent, (DWORD)wl);
    }

    // Patch the RIFF/data sizes now that the length is known.
    fflush(f);
    uint32_t dataBytes = ctx->bytesWritten;
    uint32_t riffSize  = 36 + dataBytes;
    fseek(f, 4,  SEEK_SET); fwrite(&riffSize,  4, 1, f);
    fseek(f, 40, SEEK_SET); fwrite(&dataBytes, 4, 1, f);
    fclose(f);
    ctx->out = NULL;
    return rc;
}

// Batch driver: read "<id>\t<text>" lines from stdin, synthesize each to
// <outDir>/<id>.wav on ONE port, and print "DONE <id>" to stdout after
// each. Reading a line at a time makes it lock-step, so a Frida capture
// can drain its hook ring between utterances.
//
// This exists because SWIttsTerm() costs ~4.6 s while the synthesis
// itself takes ~90 ms: spawning one process per phrase spent 98% of the
// wall clock on library teardown.
//
// By default each utterance gets a FRESH port (close + reopen between
// lines). That is not paranoia: reusing one port across utterances lets
// the engine carry session state, and a Frida capture of "text_002"
// then yields 259 trace events instead of the 276 it emits on a virgin
// port -- the audio is byte-identical, but the trace the oracle audit
// consumes is not. Reopening costs ~60 ms and restores exact
// one-process-per-phrase semantics; the ~4.6 s we are avoiding was
// SWIttsTerm/DLL-detach, which still happens only once.
//
// --batch-shared-port opts into a single port for the whole run. Use it
// only when you want audio and nothing else.
// `portInOut` is updated as ports are cycled so the caller always closes
// the live one.
static int run_batch(SWIttsAPI *api, SWIttsPort *portInOut, Ctx *ctx,
                     const wchar_t *outDir, uint32_t sampleRate,
                     int freshPort, int enablePhonemes) {
    char line[65536];
    int n = 0, failed = 0;
    SWIttsPort port = *portInOut;
    const char *openParams = "hostname=127.0.0.1;hostport=5555";

    while (fgets(line, sizeof line, stdin)) {
        // Cycle the port before every utterance except the first, which
        // already has the virgin port opened by main().
        if (freshPort && n > 0) {
            api->ClosePort(port);
            port = SWITTS_INVALID_PORT;
            if (api->OpenPortEx(&port, openParams, NULL, cb, ctx) != 0
                || port == SWITTS_INVALID_PORT) {
                fprintf(stderr, "ERROR: batch could not reopen port\n");
                *portInOut = port;
                return 1;
            }
            *portInOut = port;
            char mimetype[64];
            _snprintf(mimetype, sizeof mimetype, "audio/L16;rate=%u", sampleRate);
            api->SetParameter(port, "tts.audioformat.mimetype", mimetype);
            if (enablePhonemes) {
                api->SetParameter(port, "tts.marks.phoneme", "true");
                api->SetParameter(port, "tts.marks.word", "true");
            }
        }

        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';
        if (len == 0) continue;

        char *tab = strchr(line, '\t');
        if (!tab) {
            fprintf(stderr, "ERROR: batch line %d has no TAB separator\n", n);
            failed++;
            continue;
        }
        *tab = '\0';
        const char *id   = line;
        const char *text = tab + 1;

        wchar_t wid[256], outPath[MAX_PATH];
        MultiByteToWideChar(CP_UTF8, 0, id, -1, wid, 256);
        _snwprintf(outPath, MAX_PATH, L"%ls\\%ls.wav", outDir, wid);

        // Expand any Balabolka/SAPI/SSML control tags in the line's text to
        // inline \! codes before synthesis (same as the CLI path).
        char expanded[65536];
        expand_tags(text, expanded, sizeof(expanded));

        if (synth_one(api, port, ctx, expanded, outPath, sampleRate) != 0)
            failed++;
        n++;

        // The capture driver blocks on this line, so it must be flushed.
        printf("DONE %s\n", id);
        fflush(stdout);
    }

    fwprintf(stderr, L"INFO: batch complete: %d utterance%hs, %d failed "
                     L"(%hs port)\n",
             n, n == 1 ? "" : "s", failed,
             freshPort ? "fresh-per-utterance" : "shared");
    *portInOut = port;   // caller closes it
    return failed ? 1 : 0;
}

int wmain(int argc, wchar_t **wargv) {
    // Parse options
    int enablePhonemes = 0;
    int rawDump = 0;
    int g2pMode = 0;
    int batchMode = 0;
    int batchSharedPort = 0;
    int gracefulTerm = 0;
    uint32_t sampleRate = 8000;
    const wchar_t *pronPhonemes = NULL;
    const wchar_t *wtext = NULL;
    const wchar_t *woutPath = NULL;
    const wchar_t *dictPath = NULL;
    const wchar_t *batchOutDir = NULL;
    const wchar_t *inputFile = NULL;

    for (int i = 1; i < argc; i++) {
        if (wcscmp(wargv[i], L"--phonemes") == 0) {
            enablePhonemes = 1;
        } else if (wcscmp(wargv[i], L"--batch") == 0) {
            // --batch <outdir>: read "<id>\t<text>" lines from stdin.
            if (i + 1 < argc) {
                batchMode   = 1;
                batchOutDir = wargv[++i];
            } else {
                fwprintf(stderr, L"ERROR: --batch requires an output directory\n");
                return 2;
            }
        } else if (wcscmp(wargv[i], L"--batch-shared-port") == 0) {
            batchSharedPort = 1;
        } else if (wcscmp(wargv[i], L"--graceful-term") == 0) {
            gracefulTerm = 1;
        } else if (wcscmp(wargv[i], L"--g2p") == 0) {
            g2pMode = 1;
            enablePhonemes = 1;
        } else if (wcscmp(wargv[i], L"--bal2spr") == 0) {
            if (i + 1 < argc) {
                // Convert Balabolka phonemes to SPR and print
                char balUtf8[4096];
                WideCharToMultiByte(CP_UTF8, 0, wargv[++i], -1, balUtf8, sizeof(balUtf8), NULL, NULL);
                char sprOut[4096];
                bal_to_spr(balUtf8, sprOut, sizeof(sprOut));
                printf("SPR: %s\n", sprOut);
                printf("Use: spfy_dumpwav.exe --pron \"%s\" output.wav\n", sprOut);
                return 0;
            } else {
                fwprintf(stderr, L"ERROR: --bal2spr requires a phoneme string argument\n");
                return 2;
            }
        } else if (wcscmp(wargv[i], L"--spr2bal") == 0) {
            if (i + 1 < argc) {
                // Convert SPR to Balabolka/ARPAbet phonemes
                // Stress in Balabolka goes AFTER the vowel, not before syllable
                char sprUtf8[4096];
                WideCharToMultiByte(CP_UTF8, 0, wargv[++i], -1, sprUtf8, sizeof(sprUtf8), NULL, NULL);
                printf("BAL: ");
                int first = 1;
                int pending_stress = 0;
                for (const char *p = sprUtf8; *p; ) {
                    if (*p == '.') { p++; continue; }
                    if (*p == '0' || *p == '1' || *p == '2') {
                        pending_stress = *p - '0';
                        p++;
                        continue;
                    }
                    // Find matching SPR symbol
                    int found = 0;
                    for (int j = 0; ARPA_TO_SPR[j].arpabet; j++) {
                        int slen = (int)strlen(ARPA_TO_SPR[j].spr);
                        if (slen > 0 && strncmp(p, ARPA_TO_SPR[j].spr, slen) == 0 &&
                            strcmp(ARPA_TO_SPR[j].spr, "_") != 0 &&
                            strcmp(ARPA_TO_SPR[j].spr, "?") != 0) {
                            if (!first) printf(" ");
                            // Balabolka uses "h" not "hh"
                            if (strcmp(ARPA_TO_SPR[j].arpabet, "hh") == 0)
                                printf("h");
                            else
                                printf("%s", ARPA_TO_SPR[j].arpabet);
                            first = 0;
                            // If this is a vowel, output pending stress AFTER it
                            if (is_vowel_spr(*p) && pending_stress > 0) {
                                printf(" %d", pending_stress);
                            }
                            pending_stress = 0;
                            p += slen;
                            found = 1;
                            break;
                        }
                    }
                    if (!found) p++;
                }
                printf("\n");
                return 0;
            } else {
                fwprintf(stderr, L"ERROR: --spr2bal requires an SPR string argument\n");
                return 2;
            }
        } else if (wcscmp(wargv[i], L"--pron") == 0) {
            if (i + 1 < argc) {
                pronPhonemes = wargv[++i];
            } else {
                fwprintf(stderr, L"ERROR: --pron requires a phoneme string argument\n");
                return 2;
            }
        } else if (wcscmp(wargv[i], L"--dictionary") == 0 || wcscmp(wargv[i], L"--dict") == 0) {
            if (i + 1 < argc) {
                dictPath = wargv[++i];
            } else {
                fwprintf(stderr, L"ERROR: --dictionary requires a file path argument\n");
                return 2;
            }
        } else if (wcscmp(wargv[i], L"-f") == 0 || wcscmp(wargv[i], L"--file") == 0) {
            // Read the text to speak from a file (no length cap). The single
            // positional argument is then the output WAV path.
            if (i + 1 < argc) {
                inputFile = wargv[++i];
            } else {
                fwprintf(stderr, L"ERROR: -f/--file requires a file path\n");
                return 2;
            }
        } else if (wcscmp(wargv[i], L"--rawdump") == 0) {
            rawDump = 1;
            enablePhonemes = 1;  // need to enable marks to get callbacks
        } else if (wcscmp(wargv[i], L"--16k") == 0) {
            sampleRate = 16000;
        } else if (wcscmp(wargv[i], L"--help") == 0 || wcscmp(wargv[i], L"-h") == 0) {
            print_usage(wargv[0]);
            return 0;
        } else if (!wtext) {
            wtext = wargv[i];
        } else if (!woutPath) {
            woutPath = wargv[i];
        }
    }

    // --pron and -f/--file get their text from elsewhere, so the sole
    // positional argument is the output WAV path (not the text).
    if ((pronPhonemes || inputFile) && !woutPath && wtext) {
        woutPath = wtext;
        wtext = L"";
    }
    // Batch mode takes its work from stdin, so it needs neither text nor an
    // output positional. Otherwise a text source (positional text, -f, or
    // --pron) and an output (WAV path, unless --g2p) are required.
    int haveText = pronPhonemes || inputFile || (wtext && wtext[0]);
    if (!batchMode && (!haveText || (!woutPath && !g2pMode))) {
        print_usage(wargv[0]);
        return 2;
    }

    // Fail fast if the Speechify server is not up. Without this the client DLL
    // still loads, the OpenPortEx fails deep inside it (spraying "DEBUG
    // (status=...)" garbage), and a header-only WAV is left behind.
    if (!server_reachable(5555, 1500)) {
        fwprintf(stderr,
                 L"ERROR: Speechify server not reachable at 127.0.0.1:5555 -- "
                 L"is it running?  (start it with bin\\Speechify.exe)\n");
        return 9;
    }

    // Load substitution dictionary if requested
    if (dictPath) {
        int nd = dict_load(dictPath);
        if (nd < 0) {
            fwprintf(stderr, L"ERROR: could not open dictionary file %ls\n", dictPath);
            return 7;
        }
        fwprintf(stderr, L"INFO: Loaded %d dictionary entries from %ls\n", nd, dictPath);
    }

    // Build the text to send. Non-batch input is heap-allocated to the exact
    // size needed, so there is no length cap -- the text may be a whole file
    // (-f, bounded only by memory) or the positional CLI argument. Batch mode
    // gets its text per line from stdin inside run_batch.
    char *speakBuf = NULL;   // owned; the final UTF-8 text handed to Speak()
    if (batchMode) {
        // nothing to build up front
    } else if (pronPhonemes) {
        // Wrap raw SPR phonemes in the inline tag: \![phonemes]. Example:
        // \![.1Sa.0kIG] = "shocking".
        int pn = WideCharToMultiByte(CP_UTF8, 0, pronPhonemes, -1, NULL, 0, NULL, NULL);
        char *pronUtf8 = (char *)malloc((size_t)(pn > 0 ? pn : 1));
        if (pronUtf8) {
            WideCharToMultiByte(CP_UTF8, 0, pronPhonemes, -1, pronUtf8, pn, NULL, NULL);
            size_t cap = strlen(pronUtf8) + 8;
            speakBuf = (char *)malloc(cap);
            if (speakBuf) _snprintf(speakBuf, cap, "\\![%s]", pronUtf8);
            free(pronUtf8);
        }
    } else {
        // Get the raw UTF-8 input -- from a file (unlimited) or the CLI arg --
        // then expand any Balabolka/SAPI/SSML control tags (<pron>, <silence>,
        // <volume>, <rate>, <bookmark>, <spell>, ...) to inline \! codes.
        char *rawtext = NULL;
        if (inputFile) {
            rawtext = read_file_utf8(inputFile);
            if (!rawtext) {
                fwprintf(stderr, L"ERROR: could not read input file %ls\n", inputFile);
                return 8;
            }
        } else {
            int rn = WideCharToMultiByte(CP_UTF8, 0, wtext, -1, NULL, 0, NULL, NULL);
            rawtext = (char *)malloc((size_t)(rn > 0 ? rn : 1));
            if (rawtext) WideCharToMultiByte(CP_UTF8, 0, wtext, -1, rawtext, rn, NULL, NULL);
        }
        if (rawtext) {
            // Tag expansion stays well under 2x the input plus a fixed slack.
            size_t ecap = strlen(rawtext) * 2 + 4096;
            speakBuf = (char *)malloc(ecap);
            if (speakBuf) expand_tags(rawtext, speakBuf, ecap);
            free(rawtext);
        }
    }
    if (!batchMode && !speakBuf) {
        fprintf(stderr, "ERROR: out of memory building the speak text\n");
        return 8;
    }

    // Apply the substitution dictionary to plain text (not to raw --pron
    // phonemes). Replacements can expand the text, so use a separate buffer.
    if (!pronPhonemes && speakBuf && g_dictCount > 0) {
        size_t cap = strlen(speakBuf) * 32 + 4096;
        char *db = (char *)malloc(cap);
        if (db) {
            int subs = dict_apply(speakBuf, db, cap);
            fwprintf(stderr, L"INFO: Dictionary applied (%d substitution%hs)\n",
                     subs, subs == 1 ? "" : "s");
            if (subs > 0)
                fprintf(stderr, "INFO: Rewritten text: %s\n", db);
            speakBuf = db;   // old buffer intentionally leaked; process is short-lived
        } else {
            fwprintf(stderr, L"WARNING: dictionary buffer alloc failed; using original text\n");
        }
    }

    // Open output WAV (skip for g2p-only and batch modes; batch opens one
    // per utterance inside synth_one)
    FILE *f = NULL;
    if (batchMode) {
        f = NULL;
    } else if (woutPath) {
        f = _wfopen(woutPath, L"wb+");
        if (!f) { fprintf(stderr, "failed to open output\n"); return 3; }
        write_wav_header(f, sampleRate, 16, 1, 0);
    } else if (g2pMode) {
        // G2P mode with no output file -- use NUL to discard audio
        f = fopen("NUL", "wb+");
    }

    // Open .phn file if phonemes enabled (skip for g2p-only mode)
    FILE *phnFile = NULL;
    if (enablePhonemes && woutPath) {
        // Replace .wav extension with .phn
        wchar_t phnPath[MAX_PATH];
        wcscpy(phnPath, woutPath);
        size_t len = wcslen(phnPath);
        if (len > 4 && _wcsicmp(phnPath + len - 4, L".wav") == 0) {
            wcscpy(phnPath + len - 4, L".phn");
        } else {
            wcscat(phnPath, L".phn");
        }
        phnFile = _wfopen(phnPath, L"w");
        if (!phnFile) {
            fwprintf(stderr, L"WARNING: Could not open %ls for phoneme output\n", phnPath);
        } else {
            fprintf(phnFile, "# Phoneme timing for: %s\n", speakBuf ? speakBuf : "");
            fprintf(phnFile, "# Format: start_sample\\tend_sample\\tphoneme\\tstress\n");
            fprintf(phnFile, "# Sample rate: %u Hz  (values are in samples, not bytes)\n", sampleRate);
            fwprintf(stderr, L"Phoneme output: %ls\n", phnPath);
        }
    }

    // Load the client DLL
    SWIttsAPI api;
    if (!LoadSWItts(&api, L".\\SWItts.dll")) {
        fprintf(stderr, "Failed to load SWItts.dll from .\n");
        return 4;
    }

    Ctx ctx = {0};
    ctx.out = f;
    ctx.phonemeOut = phnFile;
    ctx.doneEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    ctx.enablePhonemes = enablePhonemes;
    ctx.rawDump = rawDump;
    ctx.g2pMode = g2pMode;
    ctx.sampleRate = sampleRate;
    ctx.g2pCount = 0;

    if (api.Init(cb, &ctx) != 0) { fprintf(stderr, "SWIttsInit failed\n"); return 5; }

    SWIttsPort port = SWITTS_INVALID_PORT;
    const char *params = "hostname=127.0.0.1;hostport=5555";

    if (api.OpenPortEx(&port, params, NULL, cb, &ctx) != 0 || port == SWITTS_INVALID_PORT) {
        fwprintf(stderr, L"ERROR: could not open a Speechify port on "
                         L"127.0.0.1:5555 (server unreachable or busy).\n");
        api.Term(cb, &ctx);
        return 6;
    }

    fwprintf(stderr, L"INFO: Port opened: %d\n", (int)port);

    // Set audio format
    char mimetype[64];
    _snprintf(mimetype, sizeof(mimetype), "audio/L16;rate=%u", sampleRate);
    if (api.SetParameter(port, "tts.audioformat.mimetype", mimetype) != 0) {
        fprintf(stderr, "SetParameter(mimetype) failed\n");
    }

    // Enable phoneme/word marks if requested
    if (enablePhonemes) {
        api.SetParameter(port, "tts.marks.phoneme", "true");
        api.SetParameter(port, "tts.marks.word", "true");
    }


    // Batch mode: drive every utterance on THIS port, then fall through
    // to the shared teardown below.
    if (batchMode) {
        int brc = run_batch(&api, &port, &ctx, batchOutDir, sampleRate,
                            !batchSharedPort, enablePhonemes);
        CloseHandle(ctx.doneEvent);
        api.ClosePort(port);
        // See the teardown note below: SWIttsTerm costs ~4.6 s and is
        // pure client-side cleanup, so it is skipped by default.
        if (gracefulTerm) api.Term(cb, &ctx);
        if (phnFile) fclose(phnFile);
        if (!gracefulTerm) fast_exit(brc);
        return brc;
    }

    // Speak
    const unsigned char *bytes = (const unsigned char*)speakBuf;
    unsigned len = (unsigned)strlen(speakBuf);
    // Auto-detect SSML: if input starts with '<speak' or '<?xml', use SSML content type
    const char *contentType = "text/plain;charset=utf-8";
    if (strncmp(speakBuf, "<speak", 6) == 0 || strncmp(speakBuf, "<?xml", 5) == 0) {
        contentType = "application/synthesis+ssml";
    }

    fwprintf(stderr, L"INFO: Speaking (%hs)...\n", contentType);

    if (api.Speak(port, bytes, len, contentType) != 0) {
        fprintf(stderr, "SWIttsSpeak failed\n");
    }

    // Wait for completion (event-based, no polling delay). Scale the timeout
    // with the text length so a large -f file is not truncated: a 60 s floor
    // plus ~10 ms per input character (a generous ceiling on synthesis time --
    // it only fires if the engine actually stalls), capped at 24 hours.
    unsigned long long wl = 60000ULL + (unsigned long long)len * 10ULL;
    if (wl > 86400000ULL) wl = 86400000ULL;
    WaitForSingleObject(ctx.doneEvent, (DWORD)wl);
    CloseHandle(ctx.doneEvent);

    fwprintf(stderr, L"INFO: Done. Closing port.\n");

    api.ClosePort(port);

    // SWIttsTerm() costs ~4.6 SECONDS while ClosePort is instant and the
    // synthesis itself takes ~90 ms -- it dominated 98% of every
    // invocation. ClosePort has already released the server-side port, and
    // the TCP connection is torn down when this process exits, so Term is
    // purely client-side library cleanup that the OS is about to do for
    // us anyway. Skipped by default; --graceful-term restores it if a
    // leak is ever suspected.
    if (gracefulTerm) api.Term(cb, &ctx);

    // Finalize WAV
    fflush(f);
    long end = ftell(f);
    uint32_t dataBytes = ctx.bytesWritten;
    fseek(f, 4, SEEK_SET);  uint32_t riffSize = 36 + dataBytes; fwrite(&riffSize,4,1,f);
    fseek(f, 40, SEEK_SET); fwrite(&dataBytes,4,1,f);
    fclose(f);

    // Close phoneme file
    if (phnFile) {
        fclose(phnFile);
    }

    int rc = 0;
    if (!g2pMode) {
        if (dataBytes == 0) {
            // Port opened but nothing came back -- server up but not producing
            // audio (voice not loaded / unhealthy, or returned junk).
            fwprintf(stderr, L"ERROR: no audio produced -- the port opened but "
                             L"synthesis returned nothing (is the voice loaded "
                             L"and the server healthy?).\n");
            rc = 10;
        } else {
            fprintf(stderr, "Wrote %u bytes of audio (%u samples at %u Hz)\n",
                    dataBytes, dataBytes / 2, sampleRate);
        }
    }

    // G2P output: print phoneme sequences
    if (g2pMode && ctx.g2pCount > 0) {
        // ARPAbet output (space-separated)
        printf("ARPAbet: ");
        for (int i = 0; i < ctx.g2pCount; i++) {
            if (i > 0) printf(" ");
            printf("%s", ctx.g2pPhones[i].name);
            if (ctx.g2pPhones[i].stress) printf("(%u)", ctx.g2pPhones[i].stress);
        }
        printf("\n");

        // SPR output (ready to paste into --pron)
        printf("SPR:     ");
        for (int i = 0; i < ctx.g2pCount; i++) {
            const char *spr = arpabet_to_spr(ctx.g2pPhones[i].name);
            if (strcmp(spr, "_") == 0) continue;  // skip pau for SPR
            if (ctx.g2pPhones[i].stress)
                printf(".%u%s", ctx.g2pPhones[i].stress, spr);
            else
                printf(".0%s", spr);
        }
        printf("\n");
    }

    free(speakBuf);   // always heap-allocated now (NULL only in batch mode)

    if (!gracefulTerm) fast_exit(rc);   // skips the ~4.6 s DLL detach
    return rc;
}
