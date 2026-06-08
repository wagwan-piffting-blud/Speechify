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

    // Debug/log message from engine
    if (port == (SWIttsPort)-1) {
        if (data) {
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

static void print_usage(const wchar_t *exe) {
    fwprintf(stderr,
        L"usage: %ls [options] \"text to speak\" out.wav\n"
        L"\n"
        L"Options:\n"
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
        exe);
}

int wmain(int argc, wchar_t **wargv) {
    // Parse options
    int enablePhonemes = 0;
    int rawDump = 0;
    int g2pMode = 0;
    uint32_t sampleRate = 8000;
    const wchar_t *pronPhonemes = NULL;
    const wchar_t *wtext = NULL;
    const wchar_t *woutPath = NULL;
    const wchar_t *dictPath = NULL;

    for (int i = 1; i < argc; i++) {
        if (wcscmp(wargv[i], L"--phonemes") == 0) {
            enablePhonemes = 1;
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

    // --pron doesn't need separate text argument; first positional arg is the output file
    if (pronPhonemes && !woutPath) {
        if (wtext) {
            woutPath = wtext;  // the positional arg is actually the output path
            wtext = L"";
        }
    }
    if (pronPhonemes && !wtext) {
        wtext = L"";
    }
    if (!wtext || (!woutPath && !g2pMode)) {
        print_usage(wargv[0]);
        return 2;
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

    // Build the text to send
    char utf8buf[8192];
    if (pronPhonemes) {
        // Wrap phonemes in SPR inline tag: \![phonemes]
        // SPR uses single-char symbols (not ARPAbet), see Language Supplement
        // Syllable boundaries: period (.), stress: 1=primary 0=unstressed
        // Example: \![.1Sa.0kIG] = "shocking"
        char pronUtf8[4096];
        WideCharToMultiByte(CP_UTF8, 0, pronPhonemes, -1, pronUtf8, sizeof(pronUtf8), NULL, NULL);
        _snprintf(utf8buf, sizeof(utf8buf), "\\![%s]", pronUtf8);
    } else {
        WideCharToMultiByte(CP_UTF8, 0, wtext, -1, utf8buf, sizeof(utf8buf), NULL, NULL);
    }

    // Apply the substitution dictionary to plain text (not to raw --pron phonemes).
    // Replacements can expand the text, so use a separate, generously sized buffer.
    char *speakBuf = utf8buf;
    if (!pronPhonemes && g_dictCount > 0) {
        size_t cap = strlen(utf8buf) * 32 + 4096;
        char *db = (char*)malloc(cap);
        if (db) {
            int subs = dict_apply(utf8buf, db, cap);
            fwprintf(stderr, L"INFO: Dictionary applied (%d substitution%hs)\n",
                     subs, subs == 1 ? "" : "s");
            if (subs > 0)
                fprintf(stderr, "INFO: Rewritten text: %s\n", db);
            speakBuf = db;
        } else {
            fwprintf(stderr, L"WARNING: dictionary buffer alloc failed; using original text\n");
        }
    }

    // Open output WAV (skip for g2p-only mode)
    FILE *f = NULL;
    if (woutPath) {
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
            fprintf(phnFile, "# Phoneme timing for: %s\n", utf8buf);
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
        fprintf(stderr, "SWIttsOpenPortEx failed\n");
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

    // Wait for completion (event-based, no polling delay)
    WaitForSingleObject(ctx.doneEvent, 60000);
    CloseHandle(ctx.doneEvent);

    fwprintf(stderr, L"INFO: Done. Closing port.\n");

    api.ClosePort(port);
    api.Term(cb, &ctx);

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

    if (!g2pMode) {
        fprintf(stderr, "Wrote %u bytes of audio (%u samples at %u Hz)\n",
                dataBytes, dataBytes / 2, sampleRate);
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

    if (speakBuf != utf8buf) free(speakBuf);

    return 0;
}
