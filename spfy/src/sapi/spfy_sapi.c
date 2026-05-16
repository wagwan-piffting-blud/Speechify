/* spfy_sapi.dll — SAPI 5 voice DLL that wraps the spfy synth library.
 *
 * Layout:
 *   - One COM CLSID (CLSID_SpfyTTSEngine) is registered as in-proc
 *     server. SAPI registers multiple voice tokens, all pointing at this
 *     CLSID; each token's `Name` attribute selects the voice directory.
 *   - ISpObjectWithToken::SetObjectToken is called once per Speak object;
 *     we read the token's Name and load spfy_voice_t for that voice.
 *   - ISpTTSEngine::Speak iterates SPVTEXTFRAG, concatenates UTF-16 text
 *     into UTF-8, and calls spfy_synth_to_sink with a sink callback that
 *     forwards int16 PCM to the SAPI site->Write().
 *   - ISpTTSEngine::GetOutputFormat advertises 8 kHz 16-bit mono PCM.
 *   - DllRegisterServer auto-scans %USERPROFILE%\Documents\Speechify\
 *     en-US\* for voice directories and creates one SAPI token per voice
 *     it finds. */

/* INITGUID must precede objbase.h/sapi.h so DEFINE_GUID emits the symbol
 * bodies into this TU. Otherwise IID_ISpObjectWithToken / IID_ISpTTSEngine
 * / CLSID_SpfyTTSEngine remain undefined at link time. */
#define INITGUID 1
#include "sapiddk_min.h"
#include "sapi_phone_table.h"
#include "spfy_synth_lib.h"
#include "pitch_shift.h"
#include "time_stretch.h"

#include <math.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <shlobj.h>      /* SHGetFolderPathW */
#include <objbase.h>
#include <olectl.h>      /* SELFREG_E_CLASS */

/* CLSID_SpfyTTSEngine is declared in sapiddk_min.h via DEFINE_GUID;
 * because INITGUID is defined at the top of this TU the symbol body is
 * emitted here (and only here) at link time. */

/* Module handle (set at DllMain for the DLL build; manually by WinMain
 * for the LocalServer32 EXE build). Not static so spfy_sapi_host.c can
 * initialise it. */
HMODULE g_hModule = NULL;
LONG    g_dll_refs = 0;

/* ----------------------------------------------------------------- */
/* COM object                                                          */
/* ----------------------------------------------------------------- */

typedef struct {
    /* Two interfaces — ISpTTSEngine is primary, ISpObjectWithToken
     * surfaced via QueryInterface using offsetof. We embed both vtbl
     * pointers so QI can return either without allocating. */
    ISpTTSEngine       tts_iface;
    ISpObjectWithToken token_iface;
    LONG               refcount;
    ISpObjectToken    *pToken;        /* AddRef'd at SetObjectToken */
    spfy_voice_t       voice;
    int                voice_loaded;
} SpfyEngineImpl;

/* Get container struct from an interface pointer. */
#define IMPL_FROM_TTS(p)   ((SpfyEngineImpl *)(((char *)(p)) - offsetof(SpfyEngineImpl, tts_iface)))
#define IMPL_FROM_TOKEN(p) ((SpfyEngineImpl *)(((char *)(p)) - offsetof(SpfyEngineImpl, token_iface)))

/* Forward decls. */
static HRESULT load_voice_from_token(SpfyEngineImpl *impl,
                                     ISpObjectToken *pToken);

/* ----------------------------------------------------------------- */
/* IUnknown / QI shared between both interfaces                       */
/* ----------------------------------------------------------------- */

static HRESULT impl_query(SpfyEngineImpl *impl, REFIID riid, void **ppv)
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

static ULONG impl_addref(SpfyEngineImpl *impl)
{
    return (ULONG)InterlockedIncrement(&impl->refcount);
}

static ULONG impl_release(SpfyEngineImpl *impl)
{
    LONG r = InterlockedDecrement(&impl->refcount);
    if (r == 0) {
        if (impl->voice_loaded) spfy_voice_free(&impl->voice);
        if (impl->pToken) ISpObjectToken_Release(impl->pToken);
        free(impl);
        InterlockedDecrement(&g_dll_refs);
    }
    return (ULONG)r;
}

/* ----------------------------------------------------------------- */
/* ISpTTSEngine methods                                                */
/* ----------------------------------------------------------------- */

static HRESULT STDMETHODCALLTYPE
tts_QueryInterface(ISpTTSEngine *This, REFIID riid, void **ppv)
{
    return impl_query(IMPL_FROM_TTS(This), riid, ppv);
}
static ULONG STDMETHODCALLTYPE tts_AddRef(ISpTTSEngine *This)
{
    return impl_addref(IMPL_FROM_TTS(This));
}
static ULONG STDMETHODCALLTYPE tts_Release(ISpTTSEngine *This)
{
    return impl_release(IMPL_FROM_TTS(This));
}

/* Sink ctx: forward int16 PCM bursts from WSOLA to SAPI site->Write.
 *
 * volume_gain — applied per-sample before forwarding to site->Write
 *   (SSML <prosody volume=...> maps to SPVSTATE.Volume in [0,100];
 *   we scale by Volume/100.0). 1.0 means pass-through.
 *
 * cum_samples — global running sample count across all fragments;
 *   bookmark/word/sentence events reference this so their audio
 *   offsets line up across the whole utterance.
 *
 * next_phoneme_evt_samples — heartbeat threshold. Some SAPI consumers
 *   (notably Balabolka) abort/cut off audio when they don't see engine
 *   events for ~1 second. Long phoneme fragments (e.g. SSML <phoneme>
 *   or SAPI XML <pron>) can produce a single word event followed by a
 *   silent multi-second stretch. To keep consumers engaged we emit a
 *   SPEI_PHONEME event roughly every 100 ms with dummy phone IDs;
 *   highlight-by-word apps ignore these but the steady stream looks
 *   like "engine still working" to the SAPI runtime. */
typedef struct {
    ISpTTSEngineSite *site;
    HRESULT           last_hr;
    int               abort;
    float             volume_gain;
    uint64_t          cum_samples;
    uint64_t          next_phoneme_evt_samples;
    FILE             *dbg_fp;           /* nullable; SPFY_SAPI_DEBUG */
    DWORD             last_acts;        /* tracks GetActions transitions */
    /* PSOLA buffer for the residual portion of <prosody pitch> that
     * exceeds the corpus selection-natural range. When psola_residual_st
     * is non-zero OR rate_factor != 1.0, sapi_sink_write accumulates
     * into psola_buf instead of writing through; the frag-end flush
     * runs TD-PSOLA then WSOLA time-stretch and forwards. */
    float             psola_residual_st;
    float             rate_factor;       /* 1.0 = no time-stretch */
    int16_t          *psola_buf;
    size_t            psola_n;
    size_t            psola_cap;
    int               psola_sr;
} sapi_sink_ctx_t;

/* Either DSP pass needs the per-frag buffer. */
#define SAPI_NEED_BUFFER(s) ((s)->psola_residual_st != 0.0f \
                             || (s)->rate_factor   != 1.0f)

#define SAPI_PHONEME_HEARTBEAT_SAMPLES  800u   /* 100 ms @ 8 kHz */

/* Word-boundary ctx — pre-scanned per-word (text_offset, text_len)
 * positions from the UTF-16 input of ONE fragment. word_offsets are
 * frag-local UTF-16 indices; we add frag_text_base (= frag's
 * SPVTEXTFRAG.ulTextSrcOffset) when emitting so events reference the
 * original SSML text. frag_audio_base is the cum sample count at the
 * start of this fragment — added to sample_offset before computing
 * ullAudioStreamOffset. */
typedef struct {
    ISpTTSEngineSite *site;
    const ULONG      *word_offsets;
    const ULONG      *word_lens;
    ULONG             word_count;
    ULONG             fired;
    const ULONG      *sentence_starts;
    ULONG             sentence_count;
    ULONG             sentence_fired;
    ULONG             frag_text_base;     /* added to lParam */
    uint64_t          frag_audio_base;    /* added to sample_offset */
} sapi_word_ctx_t;

/* Append `samples` to the PSOLA accumulator. Word/sentence events have
 * already been emitted by the synth-time callback at the correct
 * sample_offset; SAPI's event delivery is byte-offset-based so events
 * sit in the consumer queue until the eventually-written audio reaches
 * their ullAudioStreamOffset (TD-PSOLA preserves duration so offsets
 * stay accurate). */
static int sapi_psola_buffer(sapi_sink_ctx_t *s, const int16_t *samples,
                             size_t n)
{
    if (s->psola_n + n > s->psola_cap) {
        size_t cap = s->psola_cap ? s->psola_cap * 2 : 16384;
        while (cap < s->psola_n + n) cap *= 2;
        int16_t *nb = (int16_t *)realloc(s->psola_buf, cap * sizeof(int16_t));
        if (!nb) { s->abort = 1; s->last_hr = E_OUTOFMEMORY; return SPFY_E_NOMEM; }
        s->psola_buf = nb;
        s->psola_cap = cap;
    }
    memcpy(s->psola_buf + s->psola_n, samples, n * sizeof(int16_t));
    s->psola_n += n;
    s->cum_samples += n;
    return SPFY_OK;
}

static int sapi_sink_write(void *ctx, const int16_t *samples, size_t n)
{
    sapi_sink_ctx_t *s = (sapi_sink_ctx_t *)ctx;
    if (s->abort) return SPFY_E_IO;
    /* DSP path: buffer raw samples; the frag-end flush does the
     * pitch-shift and/or time-stretch then forwards to the site. */
    if (SAPI_NEED_BUFFER(s)) return sapi_psola_buffer(s, samples, n);
    DWORD acts = ISpTTSEngineSite_GetActions(s->site);
    if (s->dbg_fp && acts != s->last_acts) {
        fprintf(s->dbg_fp,
                "[sink write] cum=%llu n=%zu acts 0x%lX -> 0x%lX\n",
                (unsigned long long)s->cum_samples, n,
                (unsigned long)s->last_acts, (unsigned long)acts);
        fflush(s->dbg_fp);
        s->last_acts = acts;
    }
    if (acts & SPVES_ABORT) {
        if (s->dbg_fp) {
            fprintf(s->dbg_fp, "[sink write] ABORT signalled by consumer\n");
            fflush(s->dbg_fp);
        }
        s->abort = 1; return SPFY_E_IO;
    }
    ULONG cb = (ULONG)(n * sizeof(int16_t));
    ULONG written = 0;
    HRESULT hr;
    /* Fast path: pass through when volume_gain == 1.0 (avoid copy). */
    if (s->volume_gain >= 0.999f && s->volume_gain <= 1.001f) {
        hr = ISpTTSEngineSite_Write(s->site, samples, cb, &written);
    } else {
        /* Scale into a stack buffer in chunks to avoid heap allocation. */
        int16_t buf[1024];
        size_t off = 0;
        hr = S_OK;
        while (off < n && SUCCEEDED(hr)) {
            size_t k = n - off;
            if (k > sizeof buf / sizeof buf[0]) k = sizeof buf / sizeof buf[0];
            for (size_t i = 0; i < k; ++i) {
                float v = (float)samples[off + i] * s->volume_gain;
                if (v >  32767.0f) v =  32767.0f;
                if (v < -32768.0f) v = -32768.0f;
                buf[i] = (int16_t)v;
            }
            ULONG sub_written = 0;
            hr = ISpTTSEngineSite_Write(s->site,
                                        buf, (ULONG)(k * sizeof(int16_t)),
                                        &sub_written);
            off += k;
        }
    }
    if (FAILED(hr)) { s->last_hr = hr; s->abort = 1; return SPFY_E_IO; }
    s->cum_samples += n;

    /* Heartbeat: fire a SPEI_PHONEME ~every 100 ms of output. Dummy
     * phone IDs (silence/silence); the goal is just a steady event
     * stream so SAPI consumers don't conclude the engine has stalled. */
    while (s->cum_samples >= s->next_phoneme_evt_samples) {
        SPEVENT pev = {0};
        pev.eEventId            = SPEI_PHONEME;
        pev.elParamType         = SPET_LPARAM_IS_UNDEFINED;
        pev.ullAudioStreamOffset = s->next_phoneme_evt_samples * 2u;
        pev.wParam = (WPARAM)100;        /* nominal duration in ms */
        pev.lParam = 0;                   /* prev/next phone = silence */
        ISpTTSEngineSite_AddEvents(s->site, &pev, 1);
        s->next_phoneme_evt_samples += SAPI_PHONEME_HEARTBEAT_SAMPLES;
    }
    return SPFY_OK;
}

/* Apply pitch shift and/or time-stretch to the buffered samples and
 * stream the result to the SAPI site. Pitch first (preserves duration),
 * then rate (scales duration). Re-uses the volume/heartbeat machinery
 * by temporarily disabling the DSP gate around the recursive write. */
static int sapi_psola_flush(sapi_sink_ctx_t *s)
{
    if (s->psola_n == 0) return SPFY_OK;
    int16_t *cur = s->psola_buf;
    size_t   cur_n = s->psola_n;
    int16_t *owned = NULL;     /* tracks malloc'd buffer to free later */

    /* Pitch pass — same length out. */
    if (s->psola_residual_st != 0.0f) {
        int16_t *shifted = (int16_t *)malloc(cur_n * sizeof(int16_t));
        if (!shifted) return SPFY_E_NOMEM;
        int rc = spfy_pitch_shift_block(cur, cur_n, shifted,
                                         s->psola_residual_st, s->psola_sr);
        if (rc != 0) { free(shifted); return SPFY_E_IO; }
        cur = shifted; owned = shifted;
    }
    /* Rate pass — variable length out. */
    if (s->rate_factor != 1.0f) {
        int16_t *stretched = NULL;
        size_t   stretched_n = 0;
        int rc = spfy_time_stretch_block(cur, cur_n, &stretched, &stretched_n,
                                          s->rate_factor, s->psola_sr);
        if (rc != 0) { free(owned); return SPFY_E_IO; }
        if (owned) free(owned);
        cur = stretched; owned = stretched; cur_n = stretched_n;
    }

    /* Temporarily switch DSP off so sapi_sink_write writes through to
     * the site instead of re-buffering. Drop the pre-DSP sample count
     * from cum_samples so the write path's bookkeeping doesn't double-
     * count; the post-DSP write re-increments by cur_n. */
    float saved_res  = s->psola_residual_st;
    float saved_rate = s->rate_factor;
    s->psola_residual_st = 0.0f;
    s->rate_factor       = 1.0f;
    s->cum_samples      -= s->psola_n;
    int wr = sapi_sink_write(s, cur, cur_n);
    s->psola_residual_st = saved_res;
    s->rate_factor       = saved_rate;
    s->psola_n = 0;
    free(owned);
    return wr;
}

/* Emit `n` zero samples — used for SSML <break>/SPVA_Silence and per-
 * fragment SilenceMSecs. */
static HRESULT sapi_emit_silence(sapi_sink_ctx_t *s, ULONG n)
{
    if (s->abort) return E_ABORT;
    if (n == 0) return S_OK;
    static const int16_t zeros[512] = {0};
    while (n > 0) {
        ULONG k = n > 512 ? 512 : n;
        ULONG cb = k * (ULONG)sizeof(int16_t);
        ULONG written = 0;
        HRESULT hr = ISpTTSEngineSite_Write(s->site, zeros, cb, &written);
        if (FAILED(hr)) { s->last_hr = hr; s->abort = 1; return hr; }
        s->cum_samples += k;
        n -= k;
    }
    return S_OK;
}

/* Emit SPEI_TTS_BOOKMARK for a <mark name="..."> tag (SAPI delivers
 * the bookmark name as the fragment's pTextStart/ulTextLen). */
static void sapi_emit_bookmark(ISpTTSEngineSite *site,
                               uint64_t cum_samples,
                               const SPVTEXTFRAG *f)
{
    SPEVENT ev = {0};
    ev.eEventId             = SPEI_TTS_BOOKMARK;
    ev.elParamType          = SPET_LPARAM_IS_STRING;
    ev.ullAudioStreamOffset = cum_samples * 2u;
    /* lParam: pointer to bookmark name (UTF-16, NUL-terminated copy).
     * SAPI ABI guarantees we can pass a transient pointer because the
     * runtime copies the string before our return. */
    static WCHAR name_buf[256];
    ULONG nlen = f->ulTextLen;
    if (nlen >= 255) nlen = 255;
    if (f->pTextStart) memcpy(name_buf, f->pTextStart, nlen * sizeof(WCHAR));
    name_buf[nlen] = 0;
    ev.lParam = (LPARAM)name_buf;
    /* wParam: bookmark numeric value if parseable (else 0). */
    ev.wParam = (WPARAM)_wtoi(name_buf);
    site->lpVtbl->AddEvents(site, &ev, 1);
}

/* Walk a UTF-16 buffer and record per-word + per-sentence positions.
 * A "word" is a maximal run of non-whitespace; a sentence boundary is
 * (. ! ?) followed by whitespace. Returns word count; writes
 * sentence count via out_sent_n. All output arrays are caller-freed. */
static ULONG scan_word_positions(const WCHAR *w, int wlen,
                                 ULONG **out_off, ULONG **out_len,
                                 ULONG **out_sent_starts,
                                 ULONG  *out_sent_n)
{
    ULONG nwords = 0, nsent = 0;
    int in_word = 0;
    int sentence_armed = 1;
    for (int i = 0; i < wlen; ++i) {
        WCHAR c = w[i];
        int ws = (c == L' ' || c == L'\t' || c == L'\n' || c == L'\r');
        if (ws) { in_word = 0; continue; }
        if (!in_word) {
            in_word = 1;
            ++nwords;
            if (sentence_armed) { ++nsent; sentence_armed = 0; }
        }
        if (c == L'.' || c == L'!' || c == L'?') sentence_armed = 1;
    }
    *out_off         = (ULONG *)malloc(sizeof(ULONG) * (nwords + 1));
    *out_len         = (ULONG *)malloc(sizeof(ULONG) * (nwords + 1));
    *out_sent_starts = (ULONG *)malloc(sizeof(ULONG) * (nsent + 1));
    if (!*out_off || !*out_len || !*out_sent_starts) {
        free(*out_off); free(*out_len); free(*out_sent_starts);
        *out_off = *out_len = *out_sent_starts = NULL;
        *out_sent_n = 0;
        return 0;
    }
    ULONG wi = 0, si = 0;
    in_word = 0;
    sentence_armed = 1;
    int start = 0;
    for (int i = 0; i < wlen; ++i) {
        WCHAR c = w[i];
        int ws = (c == L' ' || c == L'\t' || c == L'\n' || c == L'\r');
        if (ws) {
            if (in_word) (*out_len)[wi - 1] = (ULONG)(i - start);
            in_word = 0;
            continue;
        }
        if (!in_word) {
            in_word = 1;
            start = i;
            (*out_off)[wi] = (ULONG)i;
            if (sentence_armed) {
                (*out_sent_starts)[si++] = wi;
                sentence_armed = 0;
            }
            ++wi;
        }
        if (c == L'.' || c == L'!' || c == L'?') sentence_armed = 1;
    }
    if (in_word) (*out_len)[wi - 1] = (ULONG)(wlen - start);
    *out_sent_n = si;
    return wi;
}

/* Called by spfy_synth_to_sink each time a new word begins. Emits a
 * SPEI_SENTENCE_BOUNDARY first (if this is the first word of a new
 * sentence), then SPEI_WORD_BOUNDARY. */
static void sapi_word_event(void *ctx, uint32_t sample_offset)
{
    sapi_word_ctx_t *wc = (sapi_word_ctx_t *)ctx;
    if (wc->fired >= wc->word_count) return;
    ULONGLONG abs_samples = wc->frag_audio_base + (uint64_t)sample_offset;
    ULONGLONG byte_offset = abs_samples * (ULONGLONG)2;
    while (wc->sentence_fired < wc->sentence_count
           && wc->sentence_starts[wc->sentence_fired] == wc->fired) {
        SPEVENT sev = {0};
        sev.eEventId            = SPEI_SENTENCE_BOUNDARY;
        sev.elParamType         = SPET_LPARAM_IS_UNDEFINED;
        sev.ullAudioStreamOffset = byte_offset;
        sev.wParam = (WPARAM)wc->word_lens[wc->fired];
        sev.lParam = (LPARAM)(wc->frag_text_base + wc->word_offsets[wc->fired]);
        wc->site->lpVtbl->AddEvents(wc->site, &sev, 1);
        wc->sentence_fired++;
    }
    SPEVENT wev = {0};
    wev.eEventId            = SPEI_WORD_BOUNDARY;
    wev.elParamType         = SPET_LPARAM_IS_UNDEFINED;
    wev.ullAudioStreamOffset = byte_offset;
    wev.wParam = (WPARAM)wc->word_lens[wc->fired];
    wev.lParam = (LPARAM)(wc->frag_text_base + wc->word_offsets[wc->fired]);
    wc->site->lpVtbl->AddEvents(wc->site, &wev, 1);
    wc->fired++;
}

/* Synth one fragment's text through the FE+USel+WSOLA pipeline. Returns
 * SPFY_OK or SPFY_E_*. Fires per-word boundary events through `wc`.
 *
 * For SPVA_Pronounce fragments that carry pPhoneIds (SSML <phoneme>
 * tags), we convert SAPI phone IDs to an SPR string wrapped in
 * `\![...]` and pass it as the synth text — the hosted FE recognises
 * the inline phoneme syntax and emits the exact phonemes verbatim
 * (same code path as `spfy_dumpwav --pron`). */
static int speak_one_frag_text(SpfyEngineImpl *impl,
                               const SPVTEXTFRAG *f,
                               sapi_sink_ctx_t   *sink_ctx,
                               sapi_word_ctx_t   *wc)
{
    if ((!f->pTextStart || f->ulTextLen == 0)
        && (f->State.eAction != SPVA_Pronounce
            || f->State.pPhoneIds == NULL)) return SPFY_OK;

    /* Word-boundary scan uses the SOURCE text (not the phoneme stream)
     * so SAPI events reference the user's original SSML chars. Both
     * Pronounce and Speak frags carry pTextStart pointing at the inner
     * word(s) being pronounced. */
    WCHAR *w = (WCHAR *)malloc((size_t)(f->ulTextLen + 1) * sizeof(WCHAR));
    if (!w) return SPFY_E_NOMEM;
    if (f->ulTextLen > 0 && f->pTextStart)
        memcpy(w, f->pTextStart, f->ulTextLen * sizeof(WCHAR));
    w[f->ulTextLen] = 0;

    /* Build the UTF-8 text actually handed to the synth — either the
     * verbatim source text, or an inline SPR-phoneme escape. */
    char *u8 = NULL;
    int   u8n = 0;
    if (f->State.eAction == SPVA_Pronounce && f->State.pPhoneIds) {
        char spr[1024];
        size_t spr_n = sapi_phones_to_spr(f->State.pPhoneIds,
                                          spr, sizeof spr);
        if (spr_n == 0) { free(w); return SPFY_OK; }
        u8n = (int)spr_n + 3;          /* "\![" + spr + "]" */
        u8 = (char *)malloc((size_t)u8n + 1);
        if (!u8) { free(w); return SPFY_E_NOMEM; }
        _snprintf(u8, (size_t)u8n + 1, "\\![%s]", spr);
    } else {
        u8n = WideCharToMultiByte(CP_UTF8, 0, w, (int)f->ulTextLen,
                                  NULL, 0, NULL, NULL);
        if (u8n <= 0) { free(w); return SPFY_OK; }
        u8 = (char *)malloc((size_t)u8n + 1);
        if (!u8) { free(w); return SPFY_E_NOMEM; }
        WideCharToMultiByte(CP_UTF8, 0, w, (int)f->ulTextLen, u8, u8n,
                            NULL, NULL);
        u8[u8n] = 0;
    }

    /* Per-frag word/sentence scan (local UTF-16 indices). */
    ULONG *off = NULL, *len = NULL, *ss = NULL, sn = 0;
    ULONG wn = scan_word_positions(w, (int)f->ulTextLen, &off, &len, &ss, &sn);
    free(w);

    wc->word_offsets    = off;
    wc->word_lens       = len;
    wc->word_count      = wn;
    wc->fired           = 0;
    wc->sentence_starts = ss;
    wc->sentence_count  = sn;
    wc->sentence_fired  = 0;
    wc->frag_text_base  = f->ulTextSrcOffset;
    wc->frag_audio_base = sink_ctx->cum_samples;

    spfy_synth_callbacks_t cb = {0};
    cb.word_cb = sapi_word_event;
    cb.ctx     = wc;

    spfy_wav_writer_t sink = {0};
    int rc = spfy_wav_open_callback(&sink, sapi_sink_write, sink_ctx,
                                    impl->voice.vdb.sample_rate);
    if (rc == SPFY_OK) {
        spfy_synth_stats_t stats = {0};
        rc = spfy_synth_to_sink(&impl->voice, u8, &sink, &cb, &stats);
    }
    spfy_wav_close(&sink);
    /* If this frag was synthesised with any DSP pass active, drain the
     * accumulator now. Done per-frag so each frag's pitch/rate is
     * honoured independently. */
    if (SAPI_NEED_BUFFER(sink_ctx) && sink_ctx->psola_n > 0) {
        sapi_psola_flush(sink_ctx);
    }
    free(u8);
    free(off); free(len); free(ss);
    return rc;
}

static HRESULT STDMETHODCALLTYPE
tts_Speak(ISpTTSEngine *This, DWORD dwSpeakFlags, REFGUID rguidFormatId,
          const WAVEFORMATEX *pWaveFormatEx,
          const SPVTEXTFRAG *pTextFragList,
          ISpTTSEngineSite *pOutputSite)
{
    (void)dwSpeakFlags; (void)rguidFormatId; (void)pWaveFormatEx;
    SpfyEngineImpl *impl = IMPL_FROM_TTS(This);
    if (!pTextFragList || !pOutputSite) return E_POINTER;
    if (!impl->voice_loaded) return SPERR_UNINITIALIZED;

    uint32_t sr = impl->voice.vdb.sample_rate;
    sapi_sink_ctx_t sink_ctx = {0};
    sink_ctx.site        = pOutputSite;
    sink_ctx.volume_gain = 1.0f;
    sink_ctx.next_phoneme_evt_samples = SAPI_PHONEME_HEARTBEAT_SAMPLES;
    sink_ctx.rate_factor              = 1.0f;

    /* Pull the SAPI-level baseline rate set via ISpVoice::SetRate(). The
     * per-fragment SPVSTATE.RateAdj is only the SSML <rate> adjustment
     * ON TOP of this baseline, so a host like Balabolka that uses its
     * rate slider (-> SetRate) would otherwise pass us frags with
     * RateAdj=0 and the slider would silently no-op. Range is the same
     * as RateAdj ([-10, +10]); we sum the two below. */
    long site_base_rate = 0;
    ISpTTSEngineSite_GetRate(pOutputSite, &site_base_rate);

    /* Diagnostic: when SPFY_SAPI_DEBUG is set, log Speak entry context
     * + GetEventInterest bitmask + each fragment we receive to
     * C:/tmp/_sapi_dbg.log. Useful when a SAPI consumer (Balabolka,
     * NVDA, etc.) cuts speech off and we need to see what events it
     * subscribes to and what frag list SAPI handed us. */
    FILE *dbg = NULL;
    if (getenv("SPFY_SAPI_DEBUG")) {
        dbg = fopen("C:/tmp/_sapi_dbg.log", "a");
        if (dbg) {
            ULONGLONG interest = 0;
            HRESULT ihr = pOutputSite->lpVtbl->GetEventInterest(
                pOutputSite, &interest);
            fprintf(dbg, "\n=== tts_Speak entry pid=%lu ===\n",
                    (unsigned long)GetCurrentProcessId());
            fprintf(dbg, "GetEventInterest: hr=0x%08lX mask=0x%016llX\n",
                    (unsigned long)ihr,
                    (unsigned long long)interest);
            /* Decode well-known bits (SPEI_X = 1 << X). */
            static const struct { int bit; const char *name; } NAMES[] = {
                { 1, "START_INPUT_STREAM" },
                { 2, "END_INPUT_STREAM" },
                { 3, "VOICE_CHANGE" },
                { 4, "TTS_BOOKMARK" },
                { 5, "WORD_BOUNDARY" },
                { 6, "PHONEME" },
                { 7, "SENTENCE_BOUNDARY" },
                { 8, "VISEME" },
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
                            "RateAdj=%ld PitchMid=%ld "
                            "SilenceMSecs=%ld pPhoneIds=%s",
                        fi, (int)f->State.eAction,
                        (unsigned long)f->ulTextLen,
                        (unsigned long)f->ulTextSrcOffset,
                        (unsigned long)f->State.Volume,
                        (long)f->State.RateAdj,
                        (long)f->State.PitchAdj.MiddleAdj,
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
    sapi_word_ctx_t word_ctx = {0};
    word_ctx.site = pOutputSite;
    sink_ctx.dbg_fp = dbg;
    HRESULT hr = S_OK;

    /* Walk the fragment list. Each fragment carries its own SPVSTATE
     * (rate / pitch / volume / silence / phoneme IDs / bookmark name).
     * SSML elements roll up into these SPVSTATE fields via SAPI's XML
     * parser before we get here. */
    int frag_idx = 0;
    for (const SPVTEXTFRAG *f = pTextFragList;
         f && !sink_ctx.abort; f = f->pNext, ++frag_idx) {
        if (dbg) {
            fprintf(dbg, "[frag %d start] cum_samples=%llu acts=0x%lX\n",
                    frag_idx, (unsigned long long)sink_ctx.cum_samples,
                    (unsigned long)ISpTTSEngineSite_GetActions(pOutputSite));
            fflush(dbg);
        }

        /* Per-fragment volume (SPVSTATE.Volume in [0,100]; 100=default). */
        ULONG vol = f->State.Volume;
        if (vol > 100u) vol = 100u;
        sink_ctx.volume_gain = (float)vol / 100.0f;

        /* Per-fragment pitch — SPVSTATE.PitchAdj.MiddleAdj is the SAPI
         * convention "approximately one semitone per unit", range [-10,
         * +10]. Split into a corpus-natural selection portion
         * (spfy_synth_set_pitch_semitones) and a residual that goes
         * through TD-PSOLA at sink flush. */
        {
            float target_st = (float)f->State.PitchAdj.MiddleAdj;
            float sel_st = 0.0f, psola_st = 0.0f;
            spfy_synth_split_pitch(target_st, &sel_st, &psola_st);
            spfy_synth_set_pitch_semitones(&impl->voice, sel_st);
            sink_ctx.psola_residual_st = psola_st;
            sink_ctx.psola_sr = (int)sr;
        }
        /* Per-fragment rate — SPVSTATE.RateAdj in [-10, +10]. Common
         * SAPI mapping: factor = pow(1.2, RateAdj/2), which gives a
         * roughly 2.5x range at the extremes. Applied as a post-process
         * WSOLA time-stretch in the same sink flush pass that handles
         * the PSOLA residual. */
        {
            long ra = f->State.RateAdj + site_base_rate;
            if (ra > 10) ra = 10;
            if (ra < -10) ra = -10;
            sink_ctx.rate_factor = powf(1.2f, (float)ra / 2.0f);
        }

        /* SilenceMSecs: a hard prefix-pause before the fragment's audio
         * (SSML <break time="..."/> emits this on the following frag's
         * State). */
        if (f->State.SilenceMSecs > 0) {
            sapi_emit_silence(&sink_ctx,
                (ULONG)(((uint64_t)f->State.SilenceMSecs * sr) / 1000u));
            if (sink_ctx.abort) break;
        }

        switch (f->State.eAction) {
        case SPVA_Speak:
        case SPVA_Pronounce:    /* <phoneme> / <pron> — handled inside */
        case SPVA_SpellOut: {
            /* Pronounce frags with no inner text (e.g. SAPI XML's
             * self-closing `<pron sym="..."/>`) produce no natural word
             * events. Some consumers (Balabolka) treat that as
             * end-of-utterance and cut off the audio. Fire a synthetic
             * SPEI_WORD_BOUNDARY at the frag's source position so the
             * consumer sees activity before the phoneme synth starts. */
            if (f->State.eAction == SPVA_Pronounce
                && (f->ulTextLen == 0 || !f->pTextStart)) {
                SPEVENT wev = {0};
                wev.eEventId             = SPEI_WORD_BOUNDARY;
                wev.elParamType          = SPET_LPARAM_IS_UNDEFINED;
                wev.ullAudioStreamOffset = sink_ctx.cum_samples * 2u;
                wev.wParam = 0;
                wev.lParam = (LPARAM)f->ulTextSrcOffset;
                ISpTTSEngineSite_AddEvents(pOutputSite, &wev, 1);
            }
            int rc = speak_one_frag_text(impl, f, &sink_ctx, &word_ctx);
            if (rc != SPFY_OK && !sink_ctx.abort) {
                hr = E_FAIL;
            }
            break;
        }
        case SPVA_Silence: {
            /* Inline silence (SSML <silence msec="..."/> uses this too,
             * delivered as a frag with eAction=SPVA_Silence). */
            sapi_emit_silence(&sink_ctx,
                (ULONG)(((uint64_t)f->State.SilenceMSecs * sr) / 1000u));
            break;
        }
        case SPVA_Bookmark: {
            sapi_emit_bookmark(pOutputSite, sink_ctx.cum_samples, f);
            break;
        }
        case SPVA_Section:
        case SPVA_ParseUnknownTag:
        default:
            /* No-op: structural / unknown tags don't produce audio. */
            break;
        }
    }

    if (sink_ctx.abort && FAILED(sink_ctx.last_hr)) hr = sink_ctx.last_hr;
    else if (sink_ctx.abort) hr = S_OK;
    /* Never propagate per-frag failure to the SAPI consumer if we
     * already wrote audio in this utterance. Some SAPI consumers
     * (Balabolka) abort the entire audio playback queue when Speak
     * returns a failure HRESULT, dropping previously-queued audio from
     * earlier in the same logical utterance. As long as at least some
     * audio reached the site, report success. */
    if (FAILED(hr) && sink_ctx.cum_samples > 0 && !sink_ctx.abort) {
        hr = S_OK;
    }
    if (dbg) {
        fprintf(dbg,
                "[tts_Speak return] hr=0x%08lX cum_samples=%llu "
                "abort=%d last_hr=0x%08lX\n",
                (unsigned long)hr,
                (unsigned long long)sink_ctx.cum_samples,
                sink_ctx.abort,
                (unsigned long)sink_ctx.last_hr);
        fclose(dbg);
    }
    free(sink_ctx.psola_buf);
    /* Reset pitch on the voice handle so the next Speak with no
     * <prosody pitch> doesn't inherit this Speak's bias. */
    spfy_synth_set_pitch_semitones(&impl->voice, 0.0f);
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
    if (!pDesiredFormatId || !ppCoMemDesiredWaveFormatEx)
        return E_POINTER;
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
/* ISpObjectWithToken methods                                          */
/* ----------------------------------------------------------------- */

static HRESULT STDMETHODCALLTYPE
tok_QueryInterface(ISpObjectWithToken *This, REFIID riid, void **ppv)
{
    return impl_query(IMPL_FROM_TOKEN(This), riid, ppv);
}
static ULONG STDMETHODCALLTYPE tok_AddRef(ISpObjectWithToken *This)
{
    return impl_addref(IMPL_FROM_TOKEN(This));
}
static ULONG STDMETHODCALLTYPE tok_Release(ISpObjectWithToken *This)
{
    return impl_release(IMPL_FROM_TOKEN(This));
}

static HRESULT STDMETHODCALLTYPE
tok_SetObjectToken(ISpObjectWithToken *This, ISpObjectToken *pToken)
{
    SpfyEngineImpl *impl = IMPL_FROM_TOKEN(This);
    if (!pToken) return E_INVALIDARG;
    if (impl->pToken) ISpObjectToken_Release(impl->pToken);
    ISpObjectToken_AddRef(pToken);
    impl->pToken = pToken;
    return load_voice_from_token(impl, pToken);
}

static HRESULT STDMETHODCALLTYPE
tok_GetObjectToken(ISpObjectWithToken *This, ISpObjectToken **ppToken)
{
    SpfyEngineImpl *impl = IMPL_FROM_TOKEN(This);
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
/* Path resolution + voice load                                        */
/* ----------------------------------------------------------------- */

/* %USERPROFILE%\Documents\Speechify\ — the project root in dev. After
 * Inno install we'll likely move global tables out of here, but for the
 * v1 SAPI build this is where everything lives. */
static int get_project_root(char *buf, size_t buf_n)
{
    WCHAR docs[MAX_PATH] = {0};
    if (FAILED(SHGetFolderPathW(NULL, CSIDL_PERSONAL, NULL,
                                 SHGFP_TYPE_CURRENT, docs)))
        return 0;
    WCHAR full[MAX_PATH] = {0};
    _snwprintf(full, MAX_PATH - 1, L"%ls\\Speechify", docs);
    int n = WideCharToMultiByte(CP_UTF8, 0, full, -1, buf,
                                (int)buf_n, NULL, NULL);
    return n > 0;
}

/* Read voice name from the SAPI token. The token's `Name` registry value
 * (or our private "VoiceName" attribute) is the en-US/<voice>/ dirname. */
static int read_voice_name(ISpObjectToken *pToken, char *out, size_t out_n)
{
    LPWSTR pwName = NULL;
    HRESULT hr = ISpObjectToken_GetStringValue(pToken, L"VoiceName", &pwName);
    if (FAILED(hr) || !pwName) {
        /* Fall back to default Name attribute. */
        if (pwName) CoTaskMemFree(pwName);
        hr = ISpObjectToken_GetStringValue(pToken, NULL, &pwName);
        if (FAILED(hr) || !pwName) return 0;
    }
    int n = WideCharToMultiByte(CP_UTF8, 0, pwName, -1, out,
                                (int)out_n, NULL, NULL);
    CoTaskMemFree(pwName);
    return n > 0;
}

static HRESULT load_voice_from_token(SpfyEngineImpl *impl,
                                     ISpObjectToken *pToken)
{
    char root[MAX_PATH];
    if (!get_project_root(root, sizeof root)) return E_FAIL;
    char voice_name[64];
    if (!read_voice_name(pToken, voice_name, sizeof voice_name))
        return E_FAIL;

    char vin[MAX_PATH], vdb[MAX_PATH], vcf[MAX_PATH];
    char hpc[MAX_PATH], vocab[MAX_PATH], tab_a[MAX_PATH], tab_b[MAX_PATH];
    _snprintf(vin,   MAX_PATH - 1, "%s\\en-US\\%s\\%s.vin",  root, voice_name, voice_name);
    _snprintf(vdb,   MAX_PATH - 1, "%s\\en-US\\%s\\%s8.vdb", root, voice_name, voice_name);
    _snprintf(vcf,   MAX_PATH - 1, "%s\\en-US\\%s\\%s.vcf",  root, voice_name, voice_name);
    _snprintf(hpc,   MAX_PATH - 1, "%s\\spfy\\data\\tom_hpclass.bin",   root);
    _snprintf(vocab, MAX_PATH - 1, "%s\\spfy\\build\\fe_symbol_table.json", root);
    _snprintf(tab_a, MAX_PATH - 1, "%s\\spfy\\data\\fe_tables_a", root);
    _snprintf(tab_b, MAX_PATH - 1, "%s\\spfy\\data\\fe_tables",   root);

    spfy_voice_paths_t paths = {
        .vin = vin, .vdb = vdb, .vcf = vcf,
        .hpclass = hpc, .vocab = vocab,
        .fe_tables_a = tab_a, .fe_tables_b = tab_b,
    };
    if (impl->voice_loaded) {
        spfy_voice_free(&impl->voice);
        impl->voice_loaded = 0;
    }
    int rc = spfy_voice_load(&paths, &impl->voice);
    if (rc != SPFY_OK) return E_FAIL;
    impl->voice_loaded = 1;
    return S_OK;
}

/* ----------------------------------------------------------------- */
/* Class factory                                                       */
/* ----------------------------------------------------------------- */

typedef struct {
    IClassFactory iface;
    LONG          refcount;
} SpfyFactory;

static HRESULT STDMETHODCALLTYPE
factory_QueryInterface(IClassFactory *This, REFIID riid, void **ppv)
{
    if (!ppv) return E_POINTER;
    *ppv = NULL;
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IClassFactory)) {
        *ppv = This;
        ((SpfyFactory *)This)->iface.lpVtbl->AddRef(This);
        return S_OK;
    }
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE factory_AddRef(IClassFactory *This)
{
    return (ULONG)InterlockedIncrement(&((SpfyFactory *)This)->refcount);
}
static ULONG STDMETHODCALLTYPE factory_Release(IClassFactory *This)
{
    LONG r = InterlockedDecrement(&((SpfyFactory *)This)->refcount);
    return (ULONG)r;
}

static HRESULT STDMETHODCALLTYPE
factory_CreateInstance(IClassFactory *This, IUnknown *pUnkOuter,
                       REFIID riid, void **ppv)
{
    (void)This;
    if (pUnkOuter) return CLASS_E_NOAGGREGATION;
    if (!ppv) return E_POINTER;
    *ppv = NULL;
    SpfyEngineImpl *impl = (SpfyEngineImpl *)calloc(1, sizeof *impl);
    if (!impl) return E_OUTOFMEMORY;
    impl->tts_iface.lpVtbl   = (CONST_VTBL ISpTTSEngineVtbl       *)&g_tts_vtbl;
    impl->token_iface.lpVtbl = (CONST_VTBL ISpObjectWithTokenVtbl *)&g_tok_vtbl;
    impl->refcount = 1;
    InterlockedIncrement(&g_dll_refs);
    HRESULT hr = impl_query(impl, riid, ppv);
    impl_release(impl);     /* QI added a ref; balance our initial 1 */
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

static SpfyFactory g_factory = { { (CONST_VTBL IClassFactoryVtbl *)&g_factory_vtbl }, 1 };

/* Accessor used by spfy_sapi_host.c to share the singleton factory. */
IClassFactory *spfy_sapi_get_factory(void);
IClassFactory *spfy_sapi_get_factory(void)
{
    return (IClassFactory *)&g_factory;
}

/* ----------------------------------------------------------------- */
/* DLL entry points                                                    */
/* ----------------------------------------------------------------- */

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

/* ----------------------------------------------------------------- */
/* DllRegisterServer / DllUnregisterServer                             */
/* ----------------------------------------------------------------- */

static int write_reg_str(HKEY root, const WCHAR *path, const WCHAR *name,
                         const WCHAR *value)
{
    HKEY h;
    if (RegCreateKeyExW(root, path, 0, NULL, 0, KEY_WRITE,
                        NULL, &h, NULL) != ERROR_SUCCESS) return 0;
    LONG r = RegSetValueExW(h, name, 0, REG_SZ, (const BYTE *)value,
                            (DWORD)((wcslen(value) + 1) * sizeof(WCHAR)));
    RegCloseKey(h);
    return r == ERROR_SUCCESS;
}

/* Same as write_reg_str but opens the 64-bit registry view explicitly
 * (KEY_WOW64_64KEY). Lets a 32-bit process write into the 64-bit hives
 * so 64-bit SAPI clients can see our voice + LocalServer32 entries. */
static int write_reg_str_64(HKEY root, const WCHAR *path, const WCHAR *name,
                            const WCHAR *value)
{
    HKEY h;
    if (RegCreateKeyExW(root, path, 0, NULL, 0,
                        KEY_WRITE | KEY_WOW64_64KEY,
                        NULL, &h, NULL) != ERROR_SUCCESS) return 0;
    LONG r = RegSetValueExW(h, name, 0, REG_SZ, (const BYTE *)value,
                            (DWORD)((wcslen(value) + 1) * sizeof(WCHAR)));
    RegCloseKey(h);
    return r == ERROR_SUCCESS;
}

static int delete_reg_tree(HKEY root, const WCHAR *path)
{
    return RegDeleteTreeW(root, path) == ERROR_SUCCESS;
}

/* Register the CLSID in both 32- and 64-bit registry views.
 *
 * 32-bit view (the natural one we run under): full classic InprocServer32
 * pointing at this DLL — fast in-proc path for 32-bit SAPI clients.
 *
 * 64-bit view: LocalServer32 pointing at spfy_sapi_host.exe. 64-bit
 * clients can't load a 32-bit DLL in-proc, but COM cross-bitness lets a
 * 64-bit client launch our 32-bit EXE and marshal calls. The EXE shares
 * the same class factory as the DLL.
 *
 * The DLL writes both views. The EXE is expected to sit next to the DLL. */
static HRESULT register_clsid(void)
{
    WCHAR mod[MAX_PATH] = {0};
    if (!GetModuleFileNameW(g_hModule, mod, MAX_PATH)) return SELFREG_E_CLASS;
    WCHAR sapi64_path[MAX_PATH] = {0};
    /* Sibling spfy_sapi64.dll path (next to this 32-bit DLL). */
    {
        wcsncpy(sapi64_path, mod, MAX_PATH);
        WCHAR *slash = wcsrchr(sapi64_path, L'\\');
        if (!slash) slash = wcsrchr(sapi64_path, L'/');
        if (slash) {
            wcsncpy(slash + 1, L"spfy_sapi64.dll",
                    (size_t)(MAX_PATH - (slash + 1 - sapi64_path)));
        }
    }

    static const WCHAR CLSID_PATH[] =
        L"CLSID\\{9C3A7D1E-4F5A-4B6C-8EA2-5C71D08F1234}";
    static const WCHAR INPROC_PATH[] =
        L"CLSID\\{9C3A7D1E-4F5A-4B6C-8EA2-5C71D08F1234}\\InprocServer32";

    /* 32-bit view: this DLL as InprocServer32. 32-bit SAPI clients load
     * us in-proc and synth runs against our 32-bit spfy stack directly. */
    if (!write_reg_str(HKEY_CLASSES_ROOT, CLSID_PATH, NULL,
                       L"Speechify TTS Engine"))
        return SELFREG_E_CLASS;
    if (!write_reg_str(HKEY_CLASSES_ROOT, INPROC_PATH, NULL, mod))
        return SELFREG_E_CLASS;
    if (!write_reg_str(HKEY_CLASSES_ROOT, INPROC_PATH, L"ThreadingModel",
                       L"Both"))
        return SELFREG_E_CLASS;

    /* 64-bit view: spfy_sapi64.dll as InprocServer32. The 64-bit shim
     * loads in-proc to 64-bit clients and spawns 32-bit spfy_synth.exe
     * for the actual synthesis. (Cross-bitness LocalServer32 doesn't
     * work for ISpTTSEngine because SAPI doesn't register proxy/stub
     * for it — verified empirically). */
    if (GetFileAttributesW(sapi64_path) != INVALID_FILE_ATTRIBUTES) {
        write_reg_str_64(HKEY_LOCAL_MACHINE,
            L"SOFTWARE\\Classes\\CLSID\\{9C3A7D1E-4F5A-4B6C-8EA2-5C71D08F1234}",
            NULL, L"Speechify TTS Engine");
        write_reg_str_64(HKEY_LOCAL_MACHINE,
            L"SOFTWARE\\Classes\\CLSID\\{9C3A7D1E-4F5A-4B6C-8EA2-5C71D08F1234}\\InprocServer32",
            NULL, sapi64_path);
        write_reg_str_64(HKEY_LOCAL_MACHINE,
            L"SOFTWARE\\Classes\\CLSID\\{9C3A7D1E-4F5A-4B6C-8EA2-5C71D08F1234}\\InprocServer32",
            L"ThreadingModel", L"Both");
    }
    return S_OK;
}

/* Create a SAPI voice token for one voice in BOTH the 32-bit (current)
 * and 64-bit registry views. 32-bit SAPI clients see the 32-bit view
 * (which our 32-bit process writes naturally); 64-bit clients see the
 * 64-bit view (we explicitly use KEY_WOW64_64KEY for those writes).
 * Tokens in both views share the same CLSID — COM routes activation to
 * either InprocServer32 (32-bit caller) or LocalServer32 (64-bit). */
static HRESULT register_voice_token(HKEY base, const WCHAR *voice_name)
{
    WCHAR base_path[256], attr_path[256];
    _snwprintf(base_path, 256,
        L"SOFTWARE\\Microsoft\\Speech\\Voices\\Tokens\\Speechify_%ls",
        voice_name);
    _snwprintf(attr_path, 256, L"%ls\\Attributes", base_path);
    WCHAR clsid[64] = L"{9C3A7D1E-4F5A-4B6C-8EA2-5C71D08F1234}";
    WCHAR display[128];
    _snwprintf(display, 128, L"Speechify - %ls", voice_name);

    /* 32-bit view (natural for our 32-bit DLL). */
    if (!write_reg_str(base, base_path, NULL, display)) return E_FAIL;
    write_reg_str(base, base_path, L"CLSID", clsid);
    write_reg_str(base, base_path, L"VoiceName", voice_name);
    write_reg_str(base, attr_path, L"Name",     display);
    write_reg_str(base, attr_path, L"Vendor",   L"Speechify");
    write_reg_str(base, attr_path, L"Language", L"409");
    write_reg_str(base, attr_path, L"Gender",   L"Male");
    write_reg_str(base, attr_path, L"Age",      L"Adult");

    /* 64-bit view — only meaningful for HKEY_LOCAL_MACHINE writes (HKCU
     * has no separate 32/64 split). */
    if (base == HKEY_LOCAL_MACHINE) {
        write_reg_str_64(base, base_path, NULL,      display);
        write_reg_str_64(base, base_path, L"CLSID",  clsid);
        write_reg_str_64(base, base_path, L"VoiceName", voice_name);
        write_reg_str_64(base, attr_path, L"Name",     display);
        write_reg_str_64(base, attr_path, L"Vendor",   L"Speechify");
        write_reg_str_64(base, attr_path, L"Language", L"409");
        write_reg_str_64(base, attr_path, L"Gender",   L"Male");
        write_reg_str_64(base, attr_path, L"Age",      L"Adult");
    }
    return S_OK;
}

/* Auto-scan %USERPROFILE%\Documents\Speechify\en-US\* — for each subdir
 * that contains <name>.vin + <name>8.vdb + <name>.vcf, register a SAPI
 * voice token. */
static HRESULT scan_and_register_voices(void)
{
    WCHAR docs[MAX_PATH] = {0};
    if (FAILED(SHGetFolderPathW(NULL, CSIDL_PERSONAL, NULL,
                                 SHGFP_TYPE_CURRENT, docs)))
        return SELFREG_E_CLASS;
    WCHAR scan_pat[MAX_PATH];
    _snwprintf(scan_pat, MAX_PATH - 1,
               L"%ls\\Speechify\\en-US\\*", docs);

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(scan_pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return S_OK;   /* nothing to scan */

    HKEY base = HKEY_LOCAL_MACHINE;
    HKEY probe;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                      L"SOFTWARE\\Microsoft\\Speech\\Voices\\Tokens",
                      0, KEY_WRITE, &probe) != ERROR_SUCCESS) {
        base = HKEY_CURRENT_USER;
    } else {
        RegCloseKey(probe);
    }

    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (fd.cFileName[0] == L'.') continue;
        const WCHAR *name = fd.cFileName;
        WCHAR vin[MAX_PATH], vdb[MAX_PATH], vcf[MAX_PATH];
        _snwprintf(vin, MAX_PATH - 1,
            L"%ls\\Speechify\\en-US\\%ls\\%ls.vin",  docs, name, name);
        _snwprintf(vdb, MAX_PATH - 1,
            L"%ls\\Speechify\\en-US\\%ls\\%ls8.vdb", docs, name, name);
        _snwprintf(vcf, MAX_PATH - 1,
            L"%ls\\Speechify\\en-US\\%ls\\%ls.vcf",  docs, name, name);
        if (GetFileAttributesW(vin) == INVALID_FILE_ATTRIBUTES) continue;
        if (GetFileAttributesW(vdb) == INVALID_FILE_ATTRIBUTES) continue;
        if (GetFileAttributesW(vcf) == INVALID_FILE_ATTRIBUTES) continue;
        register_voice_token(base, name);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return S_OK;
}

HRESULT WINAPI DllRegisterServer(void)
{
    HRESULT hr = register_clsid();
    if (FAILED(hr)) return hr;
    return scan_and_register_voices();
}

/* Sweep tokens prefixed Speechify_ from one specific hive opened with
 * the caller-supplied access flags (so we can hit both 32-bit and
 * 64-bit views of HKLM by passing KEY_WOW64_64KEY where needed). */
static void clean_voice_tokens(HKEY hive, REGSAM extra_sam)
{
    HKEY tokens;
    if (RegOpenKeyExW(hive,
                      L"SOFTWARE\\Microsoft\\Speech\\Voices\\Tokens",
                      0, KEY_READ | KEY_WRITE | extra_sam,
                      &tokens) != ERROR_SUCCESS) return;
    WCHAR victims[64][256];
    int n_victims = 0;
    DWORD idx = 0;
    while (n_victims < 64) {
        WCHAR name[256];
        DWORD nlen = 256;
        if (RegEnumKeyExW(tokens, idx, name, &nlen, NULL, NULL, NULL,
                          NULL) != ERROR_SUCCESS) break;
        if (wcsncmp(name, L"Speechify_", 10) == 0) {
            wcsncpy(victims[n_victims], name, 256);
            victims[n_victims][255] = 0;
            n_victims++;
        }
        idx++;
    }
    for (int i = 0; i < n_victims; ++i) {
        RegDeleteTreeW(tokens, victims[i]);
    }
    RegCloseKey(tokens);
}

HRESULT WINAPI DllUnregisterServer(void)
{
    /* 32-bit HKLM + HKCU + 64-bit HKLM. */
    clean_voice_tokens(HKEY_LOCAL_MACHINE, 0);
    clean_voice_tokens(HKEY_CURRENT_USER,  0);
    clean_voice_tokens(HKEY_LOCAL_MACHINE, KEY_WOW64_64KEY);

    /* CLSID in both views. */
    delete_reg_tree(HKEY_CLASSES_ROOT,
        L"CLSID\\{9C3A7D1E-4F5A-4B6C-8EA2-5C71D08F1234}");
    {
        HKEY k;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Classes",
                          0, KEY_WRITE | KEY_WOW64_64KEY, &k) == ERROR_SUCCESS) {
            RegDeleteTreeW(k, L"CLSID\\{9C3A7D1E-4F5A-4B6C-8EA2-5C71D08F1234}");
            RegCloseKey(k);
        }
    }
    return S_OK;
}
