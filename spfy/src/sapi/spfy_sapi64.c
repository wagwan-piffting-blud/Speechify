/* spfy_sapi64.dll — 64-bit SAPI shim for the Speechify engine.
 *
 * The full spfy synth stack is 32-bit (the FE host needs the 32-bit PE
 * loader). To make Speechify voices usable from 64-bit SAPI clients
 * (Windows Narrator, modern System.Speech, 64-bit apps) we ship a
 * second DLL that:
 *
 *   - Implements ISpTTSEngine + ISpObjectWithToken natively as a
 *     64-bit in-proc COM server (so 64-bit clients load it directly,
 *     no cross-bitness COM marshaling required).
 *
 *   - Inside Speak(), shells out to the 32-bit `spfy_synth.exe` —
 *     same binary the CLI uses — to do the actual synthesis. We pass
 *     SPFY_WORD_EVENTS_FILE=<tmp.tsv> so the 32-bit child emits a
 *     per-word audio-offset sidecar; we use it to fire
 *     SPEI_WORD_BOUNDARY / SPEI_SENTENCE_BOUNDARY events back to the
 *     SAPI site as we stream the WAV chunk-by-chunk.
 *
 *   - Voice discovery uses the same auto-scan layout as the 32-bit
 *     DLL (%USERPROFILE%\Documents\Speechify\en-US\<name>\). The
 *     32-bit DLL's DllRegisterServer continues to own the CLSID and
 *     token registration; this 64-bit DLL only needs to be loadable
 *     when the 64-bit registry's InprocServer32 points at it. */

#define INITGUID 1

#include <windows.h>
#include <objbase.h>
#include <olectl.h>
#include <shlobj.h>
#include <sapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <wchar.h>

#include "sapiddk_min.h"
#include "sapi_phone_table.h"

/* Module handle (set in DllMain) — used to derive paths. */
static HMODULE g_hModule = NULL;
static LONG    g_dll_refs = 0;

/* ----------------------------------------------------------------- */
/* COM object                                                          */
/* ----------------------------------------------------------------- */

typedef struct {
    ISpTTSEngine       tts_iface;
    ISpObjectWithToken token_iface;
    LONG               refcount;
    ISpObjectToken    *pToken;
    WCHAR              voice_name[64];   /* read once from token */
    int                voice_resolved;
} SpfyEngine64;

#define IMPL_FROM_TTS(p)   ((SpfyEngine64 *)(((char *)(p)) - offsetof(SpfyEngine64, tts_iface)))
#define IMPL_FROM_TOKEN(p) ((SpfyEngine64 *)(((char *)(p)) - offsetof(SpfyEngine64, token_iface)))

static HRESULT impl_query(SpfyEngine64 *impl, REFIID riid, void **ppv)
{
    if (!ppv) return E_POINTER;
    *ppv = NULL;
    if (IsEqualIID(riid, &IID_IUnknown)
        || IsEqualIID(riid, &IID_ISpTTSEngine)) {
        *ppv = &impl->tts_iface;
    } else if (IsEqualIID(riid, &IID_ISpObjectWithToken)) {
        *ppv = &impl->token_iface;
    } else {
        return E_NOINTERFACE;
    }
    InterlockedIncrement(&impl->refcount);
    return S_OK;
}
static ULONG impl_addref(SpfyEngine64 *impl)
{
    return (ULONG)InterlockedIncrement(&impl->refcount);
}
static ULONG impl_release(SpfyEngine64 *impl)
{
    LONG r = InterlockedDecrement(&impl->refcount);
    if (r == 0) {
        if (impl->pToken) ISpObjectToken_Release(impl->pToken);
        free(impl);
        InterlockedDecrement(&g_dll_refs);
    }
    return (ULONG)r;
}

/* ----------------------------------------------------------------- */
/* Paths + voice name                                                  */
/* ----------------------------------------------------------------- */

static int get_documents_speechify(WCHAR *out, size_t out_n)
{
    WCHAR docs[MAX_PATH] = {0};
    if (FAILED(SHGetFolderPathW(NULL, CSIDL_PERSONAL, NULL,
                                 SHGFP_TYPE_CURRENT, docs))) return 0;
    return _snwprintf(out, out_n - 1, L"%ls\\Speechify", docs) > 0;
}

static int read_voice_name_w(ISpObjectToken *tok, WCHAR *out, size_t out_n)
{
    LPWSTR pw = NULL;
    HRESULT hr = ISpObjectToken_GetStringValue(tok, L"VoiceName", &pw);
    if (FAILED(hr) || !pw) {
        if (pw) CoTaskMemFree(pw);
        hr = ISpObjectToken_GetStringValue(tok, NULL, &pw);
        if (FAILED(hr) || !pw) return 0;
    }
    wcsncpy(out, pw, out_n - 1);
    out[out_n - 1] = 0;
    CoTaskMemFree(pw);
    return 1;
}

/* ----------------------------------------------------------------- */
/* Word/sentence scanning of the input UTF-16 text                     */
/* ----------------------------------------------------------------- */

typedef struct {
    ULONG offset;     /* UTF-16 char index */
    ULONG length;
    int   sentence_start;
} word_pos_t;

static word_pos_t *scan_words(const WCHAR *w, int wlen, ULONG *out_n,
                              ULONG **out_sent_starts, ULONG *out_sent_n)
{
    ULONG nw = 0, ns = 0;
    int in_w = 0, sent_armed = 1;
    for (int i = 0; i < wlen; ++i) {
        WCHAR c = w[i];
        int ws = (c == L' ' || c == L'\t' || c == L'\n' || c == L'\r');
        if (ws) { in_w = 0; continue; }
        if (!in_w) { in_w = 1; ++nw; if (sent_armed) { ++ns; sent_armed = 0; } }
        if (c == L'.' || c == L'!' || c == L'?') sent_armed = 1;
    }
    word_pos_t *out = (word_pos_t *)malloc(sizeof *out * (nw + 1));
    ULONG       *ss = (ULONG       *)malloc(sizeof *ss * (ns + 1));
    if (!out || !ss) { free(out); free(ss); return NULL; }
    ULONG wi = 0, si = 0;
    in_w = 0; sent_armed = 1;
    int start = 0;
    for (int i = 0; i < wlen; ++i) {
        WCHAR c = w[i];
        int ws = (c == L' ' || c == L'\t' || c == L'\n' || c == L'\r');
        if (ws) {
            if (in_w) out[wi - 1].length = (ULONG)(i - start);
            in_w = 0;
            continue;
        }
        if (!in_w) {
            in_w = 1; start = i;
            out[wi].offset = (ULONG)i;
            out[wi].sentence_start = sent_armed ? 1 : 0;
            if (sent_armed) { ss[si++] = wi; sent_armed = 0; }
            ++wi;
        }
        if (c == L'.' || c == L'!' || c == L'?') sent_armed = 1;
    }
    if (in_w) out[wi - 1].length = (ULONG)(wlen - start);
    *out_n = wi;
    *out_sent_starts = ss;
    *out_sent_n      = si;
    return out;
}

/* ----------------------------------------------------------------- */
/* Subprocess: invoke 32-bit spfy_synth.exe                            */
/* ----------------------------------------------------------------- */

/* Look for spfy_synth.exe in the same directory as this 64-bit DLL.
 * The 32-bit DLL + 32-bit synth.exe + 64-bit DLL all sit in the same
 * install dir post-Inno. Falls back to %USERPROFILE%\Documents\
 * Speechify\spfy_build32\src\cli\spfy_synth.exe for dev. */
static int locate_synth_exe(WCHAR *out, size_t out_n)
{
    WCHAR mod[MAX_PATH] = {0};
    if (GetModuleFileNameW(g_hModule, mod, MAX_PATH)) {
        WCHAR *slash = wcsrchr(mod, L'\\');
        if (slash) {
            *slash = 0;
            _snwprintf(out, out_n - 1, L"%ls\\spfy_synth.exe", mod);
            if (GetFileAttributesW(out) != INVALID_FILE_ATTRIBUTES) return 1;
        }
    }
    /* Dev fallback: hard-coded build dir. */
    _snwprintf(out, out_n - 1,
        L"C:\\tmp\\spfy_build32\\src\\cli\\spfy_synth.exe");
    if (GetFileAttributesW(out) != INVALID_FILE_ATTRIBUTES) return 1;
    return 0;
}

/* Run spfy_synth.exe synchronously. Returns 0 on success. */
static int run_synth_subprocess(const WCHAR *exe, const WCHAR *voice_name,
                                const char  *utf8_text,
                                const WCHAR *out_wav,
                                const WCHAR *out_events)
{
    WCHAR project[MAX_PATH];
    if (!get_documents_speechify(project, MAX_PATH)) return -1;

    WCHAR vin[MAX_PATH], vdb[MAX_PATH], vcf[MAX_PATH];
    WCHAR hpc[MAX_PATH], vocab[MAX_PATH], tab_a[MAX_PATH], tab_b[MAX_PATH];
    _snwprintf(vin,   MAX_PATH, L"%ls\\en-US\\%ls\\%ls.vin",  project, voice_name, voice_name);
    _snwprintf(vdb,   MAX_PATH, L"%ls\\en-US\\%ls\\%ls8.vdb", project, voice_name, voice_name);
    _snwprintf(vcf,   MAX_PATH, L"%ls\\en-US\\%ls\\%ls.vcf",  project, voice_name, voice_name);
    _snwprintf(hpc,   MAX_PATH, L"%ls\\spfy\\data\\tom_hpclass.bin", project);
    _snwprintf(vocab, MAX_PATH, L"%ls\\spfy\\build\\fe_symbol_table.json", project);
    _snwprintf(tab_a, MAX_PATH, L"%ls\\spfy\\data\\fe_tables_a", project);
    _snwprintf(tab_b, MAX_PATH, L"%ls\\spfy\\data\\fe_tables",   project);

    /* Convert UTF-8 text -> UTF-16 for the cmdline (CreateProcessW). */
    int wn = MultiByteToWideChar(CP_UTF8, 0, utf8_text, -1, NULL, 0);
    if (wn <= 0) return -1;
    WCHAR *wtxt = (WCHAR *)malloc((size_t)wn * sizeof(WCHAR));
    if (!wtxt) return -1;
    MultiByteToWideChar(CP_UTF8, 0, utf8_text, -1, wtxt, wn);

    /* Build cmdline. */
    size_t cmd_cap = (size_t)wn + MAX_PATH * 9 + 64;
    WCHAR *cmd = (WCHAR *)malloc(cmd_cap * sizeof(WCHAR));
    if (!cmd) { free(wtxt); return -1; }
    _snwprintf(cmd, cmd_cap,
        L"\"%ls\" \"%ls\" \"%ls\" \"%ls\" \"%ls\" \"%ls\" \"%ls\" \"%ls\" \"%ls\" \"%ls\"",
        exe, vin, vdb, vcf, hpc, vocab, tab_a, tab_b, wtxt, out_wav);
    free(wtxt);

    /* Pass SPFY_WORD_EVENTS_FILE=<out_events> via environment. */
    LPWCH base_env = GetEnvironmentStringsW();
    size_t base_n = 0;
    {
        const WCHAR *p = base_env;
        while (*p) { size_t l = wcslen(p) + 1; base_n += l; p += l; }
        base_n += 1;
    }
    WCHAR wev_var[MAX_PATH + 64];
    int wev_len = _snwprintf(wev_var, MAX_PATH + 64,
                              L"SPFY_WORD_EVENTS_FILE=%ls", out_events);
    size_t total = base_n + (size_t)wev_len + 1 + 1;
    WCHAR *env = (WCHAR *)malloc(total * sizeof(WCHAR));
    if (!env) { FreeEnvironmentStringsW(base_env); free(cmd); return -1; }
    {
        WCHAR *dst = env;
        const WCHAR *src = base_env;
        while (*src) {
            size_t l = wcslen(src) + 1;
            memcpy(dst, src, l * sizeof(WCHAR));
            dst += l; src += l;
        }
        memcpy(dst, wev_var, ((size_t)wev_len + 1) * sizeof(WCHAR));
        dst += wev_len + 1;
        *dst = 0;
    }
    FreeEnvironmentStringsW(base_env);

    STARTUPINFOW si = {0};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {0};
    BOOL ok = CreateProcessW(NULL, cmd, NULL, NULL, FALSE,
                              CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
                              env, NULL, &si, &pi);
    free(cmd);
    free(env);
    if (!ok) return -1;
    WaitForSingleObject(pi.hProcess, 30000);
    DWORD exit_code = 1;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return (int)exit_code;
}

/* ----------------------------------------------------------------- */
/* Stream the rendered WAV + emit boundary events                       */
/* ----------------------------------------------------------------- */

/* Stream a fragment's rendered WAV to the SAPI site with optional gain
 * + word/sentence boundary events.
 *
 *   audio_base    cum sample count at start of this fragment; added to
 *                 the per-event audio offset so events line up globally
 *                 across all fragments in the utterance.
 *   text_base     SPVTEXTFRAG.ulTextSrcOffset of this fragment; added
 *                 to per-word UTF-16 offsets so lParam values reference
 *                 the original SSML text (not just the local frag).
 *   volume_gain   linear scalar, 1.0 = passthrough.
 *
 * On return *cum_samples_out is incremented by samples streamed. */
static HRESULT stream_wav_with_events(ISpTTSEngineSite *site,
                                      const WCHAR *wav_path,
                                      const WCHAR *evt_path,
                                      const word_pos_t *words,
                                      ULONG word_n,
                                      uint64_t audio_base,
                                      ULONG    text_base,
                                      float    volume_gain,
                                      uint64_t *cum_samples_out)
{
    HANDLE eh = CreateFileW(evt_path, GENERIC_READ, FILE_SHARE_READ, NULL,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    ULONG *evt_sample_off = NULL;
    ULONG  evt_n = 0;
    if (eh != INVALID_HANDLE_VALUE) {
        DWORD sz = GetFileSize(eh, NULL);
        char *buf = (char *)malloc((size_t)sz + 1);
        DWORD got = 0;
        if (buf && ReadFile(eh, buf, sz, &got, NULL)) {
            buf[got] = 0;
            ULONG lines = 0;
            for (char *p = buf; *p; ++p) if (*p == '\n') ++lines;
            evt_sample_off = (ULONG *)malloc(sizeof(ULONG) * (lines + 1));
            if (evt_sample_off) {
                char *p = buf;
                while (*p) {
                    char *eol = strchr(p, '\n');
                    if (!eol) eol = p + strlen(p);
                    char save = *eol; *eol = 0;
                    ULONG s = (ULONG)strtoul(p, NULL, 10);
                    evt_sample_off[evt_n++] = s;
                    *eol = save;
                    if (!*eol) break;
                    p = eol + 1;
                }
            }
        }
        free(buf);
        CloseHandle(eh);
    }

    HANDLE wh = CreateFileW(wav_path, GENERIC_READ, FILE_SHARE_READ, NULL,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (wh == INVALID_HANDLE_VALUE) { free(evt_sample_off); return E_FAIL; }
    BYTE hdr[44];
    DWORD got = 0;
    ReadFile(wh, hdr, 44, &got, NULL);

    HRESULT hr = S_OK;
    ULONG samples_written = 0;
    ULONG ei = 0;
    int   apply_gain = (volume_gain < 0.999f || volume_gain > 1.001f);
    BYTE  chunk[4096];
    BYTE  scaled[4096];
    /* Heartbeat phoneme events every 100 ms keep Balabolka & other
     * SAPI consumers from cutting the audio buffer mid-stream when no
     * natural word events fire (e.g. inside a long <pron>). */
    const ULONG HEARTBEAT_SAMPLES = 800u;   /* 100 ms @ 8 kHz */
    ULONG next_phone_evt = HEARTBEAT_SAMPLES;
    for (;;) {
        if (FAILED(hr)) break;
        DWORD acts = ISpTTSEngineSite_GetActions(site);
        if (acts & SPVES_ABORT) break;
        while (ei < evt_n && evt_sample_off[ei] <= samples_written
               && ei < word_n) {
            ULONGLONG byte_off = (audio_base + samples_written) * 2u;
            if (words[ei].sentence_start) {
                SPEVENT sev = {0};
                sev.eEventId            = SPEI_SENTENCE_BOUNDARY;
                sev.elParamType         = SPET_LPARAM_IS_UNDEFINED;
                sev.ullAudioStreamOffset = byte_off;
                sev.wParam = (WPARAM)words[ei].length;
                sev.lParam = (LPARAM)(text_base + words[ei].offset);
                site->lpVtbl->AddEvents(site, &sev, 1);
            }
            SPEVENT wev = {0};
            wev.eEventId            = SPEI_WORD_BOUNDARY;
            wev.elParamType         = SPET_LPARAM_IS_UNDEFINED;
            wev.ullAudioStreamOffset = byte_off;
            wev.wParam = (WPARAM)words[ei].length;
            wev.lParam = (LPARAM)(text_base + words[ei].offset);
            site->lpVtbl->AddEvents(site, &wev, 1);
            ei++;
        }
        DWORD r = 0;
        if (!ReadFile(wh, chunk, sizeof chunk, &r, NULL) || r == 0) break;
        const void *out_buf = chunk;
        if (apply_gain) {
            const int16_t *in = (const int16_t *)chunk;
            int16_t       *out = (int16_t *)scaled;
            DWORD nsamp = r / 2u;
            for (DWORD i = 0; i < nsamp; ++i) {
                float v = (float)in[i] * volume_gain;
                if (v >  32767.0f) v =  32767.0f;
                if (v < -32768.0f) v = -32768.0f;
                out[i] = (int16_t)v;
            }
            out_buf = scaled;
        }
        ULONG written = 0;
        hr = site->lpVtbl->Write(site, out_buf, r, &written);
        samples_written += r / 2u;
        while (samples_written >= next_phone_evt) {
            SPEVENT pev = {0};
            pev.eEventId            = SPEI_PHONEME;
            pev.elParamType         = SPET_LPARAM_IS_UNDEFINED;
            pev.ullAudioStreamOffset = (audio_base + next_phone_evt) * 2u;
            pev.wParam = (WPARAM)100;
            pev.lParam = 0;
            site->lpVtbl->AddEvents(site, &pev, 1);
            next_phone_evt += HEARTBEAT_SAMPLES;
        }
    }
    CloseHandle(wh);
    free(evt_sample_off);
    if (cum_samples_out) *cum_samples_out += samples_written;
    return hr;
}

/* Emit `n_samples` of zeros to the SAPI site — used for SilenceMSecs +
 * SPVA_Silence. Updates *cum_samples. */
static HRESULT emit_silence64(ISpTTSEngineSite *site, ULONG n_samples,
                              uint64_t *cum_samples)
{
    static const int16_t zeros[1024] = {0};
    HRESULT hr = S_OK;
    while (n_samples > 0 && SUCCEEDED(hr)) {
        DWORD acts = ISpTTSEngineSite_GetActions(site);
        if (acts & SPVES_ABORT) return E_ABORT;
        ULONG k = n_samples > 1024 ? 1024 : n_samples;
        ULONG written = 0;
        hr = site->lpVtbl->Write(site, zeros, k * (ULONG)sizeof(int16_t),
                                  &written);
        if (cum_samples) *cum_samples += k;
        n_samples -= k;
    }
    return hr;
}

/* Emit SPEI_TTS_BOOKMARK for <mark name="..."> tags. */
static void emit_bookmark64(ISpTTSEngineSite *site, uint64_t cum_samples,
                            const SPVTEXTFRAG *f)
{
    SPEVENT ev = {0};
    ev.eEventId             = SPEI_TTS_BOOKMARK;
    ev.elParamType          = SPET_LPARAM_IS_STRING;
    ev.ullAudioStreamOffset = cum_samples * 2u;
    static WCHAR name_buf[256];
    ULONG nlen = f->ulTextLen;
    if (nlen >= 255) nlen = 255;
    if (f->pTextStart) memcpy(name_buf, f->pTextStart, nlen * sizeof(WCHAR));
    name_buf[nlen] = 0;
    ev.lParam = (LPARAM)name_buf;
    ev.wParam = (WPARAM)_wtoi(name_buf);
    site->lpVtbl->AddEvents(site, &ev, 1);
}

/* ----------------------------------------------------------------- */
/* ISpTTSEngine methods                                                 */
/* ----------------------------------------------------------------- */

static HRESULT STDMETHODCALLTYPE
tts_QueryInterface(ISpTTSEngine *This, REFIID riid, void **ppv)
{ return impl_query(IMPL_FROM_TTS(This), riid, ppv); }
static ULONG STDMETHODCALLTYPE tts_AddRef(ISpTTSEngine *This)
{ return impl_addref(IMPL_FROM_TTS(This)); }
static ULONG STDMETHODCALLTYPE tts_Release(ISpTTSEngine *This)
{ return impl_release(IMPL_FROM_TTS(This)); }

/* Concatenate the SPVTEXTFRAG list into UTF-16 + matching UTF-8. */
static char *frags_to_utf8(const SPVTEXTFRAG *frag, WCHAR **out_w, int *out_wlen)
{
    int total = 0;
    for (const SPVTEXTFRAG *f = frag; f; f = f->pNext) {
        if (f->State.eAction == SPVA_Speak
            || f->State.eAction == SPVA_Pronounce
            || f->State.eAction == SPVA_SpellOut) {
            total += (int)f->ulTextLen;
        }
        total += 1;
    }
    if (total <= 0) return NULL;
    WCHAR *w = (WCHAR *)malloc((size_t)total * sizeof(WCHAR) + sizeof(WCHAR));
    if (!w) return NULL;
    int wp = 0;
    for (const SPVTEXTFRAG *f = frag; f; f = f->pNext) {
        if (f->State.eAction == SPVA_Speak
            || f->State.eAction == SPVA_Pronounce
            || f->State.eAction == SPVA_SpellOut) {
            if (f->pTextStart && f->ulTextLen > 0) {
                memcpy(w + wp, f->pTextStart, f->ulTextLen * sizeof(WCHAR));
                wp += (int)f->ulTextLen;
            }
            w[wp++] = L' ';
        }
    }
    w[wp] = 0;
    int u8n = WideCharToMultiByte(CP_UTF8, 0, w, wp, NULL, 0, NULL, NULL);
    if (u8n <= 0) { free(w); return NULL; }
    char *u8 = (char *)malloc((size_t)u8n + 1);
    if (!u8) { free(w); return NULL; }
    WideCharToMultiByte(CP_UTF8, 0, w, wp, u8, u8n, NULL, NULL);
    u8[u8n] = 0;
    *out_w    = w;
    *out_wlen = wp;
    return u8;
}

/* Synth one fragment's text via the 32-bit spfy_synth.exe subprocess,
 * then stream its WAV (with volume gain + boundary events) to the SAPI
 * site. Advances *cum_samples by the streamed sample count. */
static HRESULT speak_one_frag_text64(SpfyEngine64 *impl,
                                     const SPVTEXTFRAG *f,
                                     ISpTTSEngineSite *site,
                                     float volume_gain,
                                     uint64_t *cum_samples)
{
    if ((!f->pTextStart || f->ulTextLen == 0)
        && (f->State.eAction != SPVA_Pronounce
            || f->State.pPhoneIds == NULL)) return S_OK;

    WCHAR *w = (WCHAR *)malloc((size_t)(f->ulTextLen + 1) * sizeof(WCHAR));
    if (!w) return E_OUTOFMEMORY;
    if (f->ulTextLen > 0 && f->pTextStart)
        memcpy(w, f->pTextStart, f->ulTextLen * sizeof(WCHAR));
    w[f->ulTextLen] = 0;

    /* For <phoneme> tags, build SPR `\![...]` from pPhoneIds; otherwise
     * convert the verbatim UTF-16 frag text to UTF-8. The 32-bit
     * spfy_synth.exe subprocess accepts both as command-line text. */
    char *u8 = NULL;
    int   u8n = 0;
    if (f->State.eAction == SPVA_Pronounce && f->State.pPhoneIds) {
        char spr[1024];
        size_t spr_n = sapi_phones_to_spr(f->State.pPhoneIds,
                                          spr, sizeof spr);
        if (spr_n == 0) { free(w); return S_OK; }
        u8n = (int)spr_n + 3;
        u8 = (char *)malloc((size_t)u8n + 1);
        if (!u8) { free(w); return E_OUTOFMEMORY; }
        _snprintf(u8, (size_t)u8n + 1, "\\![%s]", spr);
    } else {
        u8n = WideCharToMultiByte(CP_UTF8, 0, w, (int)f->ulTextLen,
                                  NULL, 0, NULL, NULL);
        if (u8n <= 0) { free(w); return S_OK; }
        u8 = (char *)malloc((size_t)u8n + 1);
        if (!u8) { free(w); return E_OUTOFMEMORY; }
        WideCharToMultiByte(CP_UTF8, 0, w, (int)f->ulTextLen, u8, u8n,
                            NULL, NULL);
        u8[u8n] = 0;
    }

    ULONG word_n = 0, sent_n = 0;
    ULONG *sent_starts = NULL;
    word_pos_t *words = scan_words(w, (int)f->ulTextLen,
                                    &word_n, &sent_starts, &sent_n);
    free(sent_starts);

    WCHAR temp_dir[MAX_PATH];
    GetTempPathW(MAX_PATH, temp_dir);
    WCHAR wav_path[MAX_PATH], evt_path[MAX_PATH];
    _snwprintf(wav_path, MAX_PATH, L"%ls\\spfy_sapi64_%lu_%llu.wav",
               temp_dir, GetCurrentProcessId(),
               (unsigned long long)*cum_samples);
    _snwprintf(evt_path, MAX_PATH, L"%ls\\spfy_sapi64_%lu_%llu.tsv",
               temp_dir, GetCurrentProcessId(),
               (unsigned long long)*cum_samples);

    WCHAR exe[MAX_PATH];
    HRESULT hr = E_FAIL;
    if (locate_synth_exe(exe, MAX_PATH)) {
        int rc = run_synth_subprocess(exe, impl->voice_name,
                                      u8, wav_path, evt_path);
        if (rc == 0) {
            hr = stream_wav_with_events(site, wav_path, evt_path,
                                        words, word_n,
                                        *cum_samples,
                                        f->ulTextSrcOffset,
                                        volume_gain,
                                        cum_samples);
        }
    }
    DeleteFileW(wav_path);
    DeleteFileW(evt_path);
    free(words);
    free(u8); free(w);
    return hr;
}

static HRESULT STDMETHODCALLTYPE
tts_Speak(ISpTTSEngine *This, DWORD dwSpeakFlags, REFGUID rguidFormatId,
          const WAVEFORMATEX *pWaveFormatEx,
          const SPVTEXTFRAG *pTextFragList,
          ISpTTSEngineSite *pOutputSite)
{
    (void)dwSpeakFlags; (void)rguidFormatId; (void)pWaveFormatEx;
    SpfyEngine64 *impl = IMPL_FROM_TTS(This);
    if (!pTextFragList || !pOutputSite) return E_POINTER;
    if (!impl->voice_resolved) return SPERR_UNINITIALIZED;

    const ULONG sr = 8000u;   /* native rate of all spfy voices */
    uint64_t cum_samples = 0;
    HRESULT hr = S_OK;
    int abort_flag = 0;

    /* SPFY_SAPI_DEBUG diagnostic — see spfy_sapi.c::tts_Speak for the
     * full description. Logs GetEventInterest, frag list, and per-frag
     * Speak progression so we can see exactly what a problematic SAPI
     * consumer is doing. */
    FILE *dbg = NULL;
    if (getenv("SPFY_SAPI_DEBUG")) {
        dbg = fopen("C:/tmp/_sapi_dbg.log", "a");
        if (dbg) {
            ULONGLONG interest = 0;
            HRESULT ihr = pOutputSite->lpVtbl->GetEventInterest(
                pOutputSite, &interest);
            fprintf(dbg, "\n=== tts_Speak (64-bit) entry pid=%lu ===\n",
                    (unsigned long)GetCurrentProcessId());
            fprintf(dbg, "GetEventInterest: hr=0x%08lX mask=0x%016llX\n",
                    (unsigned long)ihr,
                    (unsigned long long)interest);
            static const struct { int bit; const char *name; } NAMES[] = {
                { 1, "START_INPUT_STREAM" }, { 2, "END_INPUT_STREAM" },
                { 3, "VOICE_CHANGE" },       { 4, "TTS_BOOKMARK" },
                { 5, "WORD_BOUNDARY" },      { 6, "PHONEME" },
                { 7, "SENTENCE_BOUNDARY" },  { 8, "VISEME" },
                { 9, "TTS_AUDIO_LEVEL" },
            };
            for (size_t i = 0; i < sizeof NAMES / sizeof NAMES[0]; ++i) {
                if (interest & (1ULL << NAMES[i].bit))
                    fprintf(dbg, "  + SPEI_%s\n", NAMES[i].name);
            }
            fprintf(dbg, "dwSpeakFlags=0x%08lX\n", (unsigned long)dwSpeakFlags);
            int fi = 0;
            for (const SPVTEXTFRAG *f = pTextFragList; f && fi < 32;
                 f = f->pNext, ++fi) {
                fprintf(dbg, "frag[%d] eAction=%d ulTextLen=%lu "
                            "ulTextSrcOffset=%lu Volume=%lu "
                            "SilenceMSecs=%ld pPhoneIds=%s",
                        fi, (int)f->State.eAction,
                        (unsigned long)f->ulTextLen,
                        (unsigned long)f->ulTextSrcOffset,
                        (unsigned long)f->State.Volume,
                        (long)f->State.SilenceMSecs,
                        f->State.pPhoneIds ? "yes" : "no");
                if (f->pTextStart && f->ulTextLen > 0) {
                    char u8[256];
                    int u8n = WideCharToMultiByte(CP_UTF8, 0,
                        f->pTextStart, (int)f->ulTextLen,
                        u8, (int)(sizeof u8 - 1), NULL, NULL);
                    if (u8n > 0) { u8[u8n] = 0; fprintf(dbg, " text=\"%s\"", u8); }
                }
                fprintf(dbg, "\n");
            }
            fflush(dbg);
        }
    }

    /* Per-fragment iteration mirroring the 32-bit DLL — see
     * spfy_sapi.c::tts_Speak for the rationale. */
    int frag_idx = 0;
    for (const SPVTEXTFRAG *f = pTextFragList; f && !abort_flag;
         f = f->pNext, ++frag_idx) {
        if (dbg) {
            DWORD a0 = ISpTTSEngineSite_GetActions(pOutputSite);
            fprintf(dbg, "[frag %d start] cum_samples=%llu acts=0x%lX\n",
                    frag_idx, (unsigned long long)cum_samples,
                    (unsigned long)a0);
            fflush(dbg);
        }
        DWORD acts = ISpTTSEngineSite_GetActions(pOutputSite);
        if (acts & SPVES_ABORT) { abort_flag = 1; break; }

        ULONG vol = f->State.Volume;
        if (vol > 100u) vol = 100u;
        float gain = (float)vol / 100.0f;

        if (f->State.SilenceMSecs > 0) {
            HRESULT shr = emit_silence64(pOutputSite,
                (ULONG)(((uint64_t)f->State.SilenceMSecs * sr) / 1000u),
                &cum_samples);
            if (FAILED(shr)) { hr = shr; break; }
        }

        switch (f->State.eAction) {
        case SPVA_Speak:
        case SPVA_Pronounce:    /* <phoneme> / <pron> — handled inside */
        case SPVA_SpellOut: {
            /* See spfy_sapi.c::tts_Speak for rationale: a synthetic
             * SPEI_WORD_BOUNDARY at the start of an empty-text Pronounce
             * frag keeps consumers like Balabolka engaged through the
             * subsequent phoneme audio. */
            if (f->State.eAction == SPVA_Pronounce
                && (f->ulTextLen == 0 || !f->pTextStart)) {
                SPEVENT wev = {0};
                wev.eEventId             = SPEI_WORD_BOUNDARY;
                wev.elParamType          = SPET_LPARAM_IS_UNDEFINED;
                wev.ullAudioStreamOffset = cum_samples * 2u;
                wev.wParam = 0;
                wev.lParam = (LPARAM)f->ulTextSrcOffset;
                pOutputSite->lpVtbl->AddEvents(pOutputSite, &wev, 1);
            }
            HRESULT fhr = speak_one_frag_text64(impl, f, pOutputSite,
                                                gain, &cum_samples);
            if (FAILED(fhr)) hr = fhr;
            break;
        }
        case SPVA_Silence:
            emit_silence64(pOutputSite,
                (ULONG)(((uint64_t)f->State.SilenceMSecs * sr) / 1000u),
                &cum_samples);
            break;
        case SPVA_Bookmark:
            emit_bookmark64(pOutputSite, cum_samples, f);
            break;
        case SPVA_Section:
        case SPVA_ParseUnknownTag:
        default:
            break;
        }
    }
    /* See spfy_sapi.c::tts_Speak — don't propagate per-frag failure
     * if any audio was written, otherwise consumers (Balabolka) drop
     * the entire playback queue. */
    if (FAILED(hr) && cum_samples > 0 && !abort_flag) {
        hr = S_OK;
    }
    if (dbg) {
        fprintf(dbg,
                "[tts_Speak (64-bit) return] hr=0x%08lX cum_samples=%llu "
                "abort=%d acts=0x%lX\n",
                (unsigned long)hr,
                (unsigned long long)cum_samples, abort_flag,
                (unsigned long)ISpTTSEngineSite_GetActions(pOutputSite));
        fclose(dbg);
    }
    return hr;
}

static HRESULT STDMETHODCALLTYPE
tts_GetOutputFormat(ISpTTSEngine *This,
                    const GUID *pTargetFormatId,
                    const WAVEFORMATEX *pTargetWaveFormatEx,
                    GUID *pDesiredFormatId,
                    WAVEFORMATEX **ppCoMemDesiredWaveFormatEx)
{
    (void)This; (void)pTargetFormatId; (void)pTargetWaveFormatEx;
    if (!pDesiredFormatId || !ppCoMemDesiredWaveFormatEx) return E_POINTER;
    WAVEFORMATEX *wfx = (WAVEFORMATEX *)CoTaskMemAlloc(sizeof(WAVEFORMATEX));
    if (!wfx) return E_OUTOFMEMORY;
    wfx->wFormatTag      = WAVE_FORMAT_PCM;
    wfx->nChannels       = 1;
    wfx->nSamplesPerSec  = 8000;
    wfx->wBitsPerSample  = 16;
    wfx->nBlockAlign     = 2;
    wfx->nAvgBytesPerSec = 16000;
    wfx->cbSize          = 0;
    *pDesiredFormatId        = SPFY_SPDFID_WaveFormatEx;
    *ppCoMemDesiredWaveFormatEx = wfx;
    return S_OK;
}

static const ISpTTSEngineVtbl g_tts_vtbl = {
    tts_QueryInterface, tts_AddRef, tts_Release,
    tts_Speak, tts_GetOutputFormat,
};

/* ----------------------------------------------------------------- */
/* ISpObjectWithToken                                                   */
/* ----------------------------------------------------------------- */

static HRESULT STDMETHODCALLTYPE
tok_QueryInterface(ISpObjectWithToken *This, REFIID riid, void **ppv)
{ return impl_query(IMPL_FROM_TOKEN(This), riid, ppv); }
static ULONG STDMETHODCALLTYPE tok_AddRef(ISpObjectWithToken *This)
{ return impl_addref(IMPL_FROM_TOKEN(This)); }
static ULONG STDMETHODCALLTYPE tok_Release(ISpObjectWithToken *This)
{ return impl_release(IMPL_FROM_TOKEN(This)); }

static HRESULT STDMETHODCALLTYPE
tok_SetObjectToken(ISpObjectWithToken *This, ISpObjectToken *pToken)
{
    SpfyEngine64 *impl = IMPL_FROM_TOKEN(This);
    if (!pToken) return E_INVALIDARG;
    if (impl->pToken) ISpObjectToken_Release(impl->pToken);
    ISpObjectToken_AddRef(pToken);
    impl->pToken = pToken;
    if (!read_voice_name_w(pToken, impl->voice_name,
                           sizeof impl->voice_name / sizeof(WCHAR)))
        return E_FAIL;
    impl->voice_resolved = 1;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE
tok_GetObjectToken(ISpObjectWithToken *This, ISpObjectToken **ppToken)
{
    SpfyEngine64 *impl = IMPL_FROM_TOKEN(This);
    if (!ppToken) return E_POINTER;
    *ppToken = impl->pToken;
    if (impl->pToken) ISpObjectToken_AddRef(impl->pToken);
    return S_OK;
}

static const ISpObjectWithTokenVtbl g_tok_vtbl = {
    tok_QueryInterface, tok_AddRef, tok_Release,
    tok_SetObjectToken, tok_GetObjectToken,
};

/* ----------------------------------------------------------------- */
/* Class factory + DLL entry points                                     */
/* ----------------------------------------------------------------- */

typedef struct { IClassFactory iface; LONG refcount; } SpfyFactory64;

static HRESULT STDMETHODCALLTYPE
factory_QueryInterface(IClassFactory *This, REFIID riid, void **ppv)
{
    if (!ppv) return E_POINTER;
    *ppv = NULL;
    if (IsEqualIID(riid, &IID_IUnknown)
        || IsEqualIID(riid, &IID_IClassFactory)) {
        *ppv = This;
        InterlockedIncrement(&((SpfyFactory64 *)This)->refcount);
        return S_OK;
    }
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE factory_AddRef(IClassFactory *This)
{ return (ULONG)InterlockedIncrement(&((SpfyFactory64 *)This)->refcount); }
static ULONG STDMETHODCALLTYPE factory_Release(IClassFactory *This)
{ return (ULONG)InterlockedDecrement(&((SpfyFactory64 *)This)->refcount); }

static HRESULT STDMETHODCALLTYPE
factory_CreateInstance(IClassFactory *This, IUnknown *pUnkOuter,
                       REFIID riid, void **ppv)
{
    (void)This;
    if (pUnkOuter) return CLASS_E_NOAGGREGATION;
    if (!ppv) return E_POINTER;
    *ppv = NULL;
    SpfyEngine64 *impl = (SpfyEngine64 *)calloc(1, sizeof *impl);
    if (!impl) return E_OUTOFMEMORY;
    impl->tts_iface.lpVtbl   = (CONST_VTBL ISpTTSEngineVtbl       *)&g_tts_vtbl;
    impl->token_iface.lpVtbl = (CONST_VTBL ISpObjectWithTokenVtbl *)&g_tok_vtbl;
    impl->refcount = 1;
    InterlockedIncrement(&g_dll_refs);
    HRESULT hr = impl_query(impl, riid, ppv);
    impl_release(impl);
    return hr;
}
static HRESULT STDMETHODCALLTYPE
factory_LockServer(IClassFactory *This, BOOL fLock)
{
    (void)This;
    if (fLock) InterlockedIncrement(&g_dll_refs);
    else       InterlockedDecrement(&g_dll_refs);
    return S_OK;
}

static const IClassFactoryVtbl g_factory_vtbl = {
    factory_QueryInterface, factory_AddRef, factory_Release,
    factory_CreateInstance, factory_LockServer,
};
static SpfyFactory64 g_factory = {
    { (CONST_VTBL IClassFactoryVtbl *)&g_factory_vtbl }, 1
};

BOOL WINAPI DllMain(HINSTANCE hInst, DWORD reason, LPVOID rsrv);
BOOL WINAPI DllMain(HINSTANCE hInst, DWORD reason, LPVOID rsrv)
{
    (void)rsrv;
    if (reason == DLL_PROCESS_ATTACH) {
        g_hModule = hInst;
        DisableThreadLibraryCalls(hInst);
    }
    return TRUE;
}

HRESULT WINAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, LPVOID *ppv)
{
    if (!ppv) return E_POINTER;
    *ppv = NULL;
    if (!IsEqualCLSID(rclsid, &CLSID_SpfyTTSEngine))
        return CLASS_E_CLASSNOTAVAILABLE;
    return g_factory.iface.lpVtbl->QueryInterface(
        (IClassFactory *)&g_factory, riid, ppv);
}

HRESULT WINAPI DllCanUnloadNow(void)
{
    return g_dll_refs == 0 ? S_OK : S_FALSE;
}

/* Registration is owned by the 32-bit DLL — but we still need
 * DllRegisterServer / DllUnregisterServer for regsvr32 to succeed. They
 * just succeed without doing anything beyond what the 32-bit DLL did. */
HRESULT WINAPI DllRegisterServer(void) { return S_OK; }
HRESULT WINAPI DllUnregisterServer(void) { return S_OK; }
