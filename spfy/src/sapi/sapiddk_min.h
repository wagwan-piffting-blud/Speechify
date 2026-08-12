/* Minimal SAPI 5 DDK declarations not present in MinGW's sapi.h family. */

#ifndef SPFY_SAPI_DDK_MIN_H
#define SPFY_SAPI_DDK_MIN_H

#include <windows.h>
#include <objbase.h>
#include <sapi.h>

#ifdef __cplusplus
extern "C" {
#endif


typedef struct SPVTEXTFRAG {
    struct SPVTEXTFRAG *pNext;
    SPVSTATE            State;
    LPCWSTR             pTextStart;
    ULONG               ulTextLen;
    ULONG               ulTextSrcOffset;
} SPVTEXTFRAG;

typedef enum SPVSKIPTYPE {
    SPVST_SENTENCE = 1
} SPVSKIPTYPE;

#define SPVES_CONTINUE 0
#define SPVES_ABORT    1
#define SPVES_SKIP     2
#define SPVES_RATE     4
#define SPVES_VOLUME   8

/* SAPI HRESULT codes we use. */
#ifndef SPERR_UNINITIALIZED
#define SPERR_UNINITIALIZED ((HRESULT)0x80045004L)
#endif

#ifndef SPF_NLP_SPEAK_PUNC
#define SPF_NLP_SPEAK_PUNC 0x00000040
#endif

/* SAPI audio format ID for PCM. */
DEFINE_GUID(SPFY_SPDFID_WaveFormatEx,
    0xc31adbae, 0x527f, 0x4ff5, 0xa2,0x30, 0xf6,0x2b,0xb6,0x1f,0xf7,0x0c);


DEFINE_GUID(IID_ISpTTSEngineSite,
    0x9880499b, 0xcce9, 0x11d2, 0xb5,0x03, 0x00,0xc0,0x4f,0x79,0x73,0x96);

typedef struct ISpTTSEngineSite ISpTTSEngineSite;
typedef struct ISpTTSEngineSiteVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(ISpTTSEngineSite *This,
        REFIID riid, void **ppvObject);
    ULONG   (STDMETHODCALLTYPE *AddRef)(ISpTTSEngineSite *This);
    ULONG   (STDMETHODCALLTYPE *Release)(ISpTTSEngineSite *This);
    HRESULT (STDMETHODCALLTYPE *AddEvents)(ISpTTSEngineSite *This,
        const SPEVENT *pEventArray, ULONG ulCount);
    HRESULT (STDMETHODCALLTYPE *GetEventInterest)(ISpTTSEngineSite *This,
        ULONGLONG *pullEventInterest);
    DWORD   (STDMETHODCALLTYPE *GetActions)(ISpTTSEngineSite *This);
    HRESULT (STDMETHODCALLTYPE *Write)(ISpTTSEngineSite *This,
        const void *pBuff, ULONG cb, ULONG *pcbWritten);
    HRESULT (STDMETHODCALLTYPE *GetRate)(ISpTTSEngineSite *This,
        long *pRateAdjust);
    HRESULT (STDMETHODCALLTYPE *GetVolume)(ISpTTSEngineSite *This,
        USHORT *pusVolume);
    HRESULT (STDMETHODCALLTYPE *GetSkipInfo)(ISpTTSEngineSite *This,
        SPVSKIPTYPE *peType, long *plNumItems);
    HRESULT (STDMETHODCALLTYPE *CompleteSkip)(ISpTTSEngineSite *This,
        long ulNumSkipped);
} ISpTTSEngineSiteVtbl;
struct ISpTTSEngineSite {
    CONST_VTBL ISpTTSEngineSiteVtbl *lpVtbl;
};

#define ISpTTSEngineSite_Write(This,b,cb,pcb)  (This)->lpVtbl->Write(This,b,cb,pcb)
#define ISpTTSEngineSite_GetActions(This)      (This)->lpVtbl->GetActions(This)
#define ISpTTSEngineSite_AddEvents(This,p,n)   (This)->lpVtbl->AddEvents(This,p,n)
#define ISpTTSEngineSite_GetRate(This,p)       (This)->lpVtbl->GetRate(This,p)
#define ISpTTSEngineSite_GetVolume(This,p)     (This)->lpVtbl->GetVolume(This,p)


DEFINE_GUID(IID_ISpTTSEngine,
    0xa74d7c8e, 0x4cc5, 0x4f2f, 0xa6,0xeb, 0x80,0x4d,0xee,0x18,0x50,0x0e);

typedef struct ISpTTSEngine ISpTTSEngine;
typedef struct ISpTTSEngineVtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(ISpTTSEngine *This,
        REFIID riid, void **ppvObject);
    ULONG   (STDMETHODCALLTYPE *AddRef)(ISpTTSEngine *This);
    ULONG   (STDMETHODCALLTYPE *Release)(ISpTTSEngine *This);
    HRESULT (STDMETHODCALLTYPE *Speak)(ISpTTSEngine *This,
        DWORD dwSpeakFlags, REFGUID rguidFormatId,
        const WAVEFORMATEX *pWaveFormatEx,
        const SPVTEXTFRAG *pTextFragList,
        ISpTTSEngineSite *pOutputSite);
    HRESULT (STDMETHODCALLTYPE *GetOutputFormat)(ISpTTSEngine *This,
        const GUID *pTargetFormatId,
        const WAVEFORMATEX *pTargetWaveFormatEx,
        GUID *pDesiredFormatId,
        WAVEFORMATEX **ppCoMemDesiredWaveFormatEx);
} ISpTTSEngineVtbl;
struct ISpTTSEngine {
    CONST_VTBL ISpTTSEngineVtbl *lpVtbl;
};

/* Our engine's CLSID. */
DEFINE_GUID(CLSID_SpfyTTSEngine,
    0x9c3a7d1e, 0x4f5a, 0x4b6c, 0x8e,0xa2, 0x5c,0x71,0xd0,0x8f,0x12,0x34);

#ifdef __cplusplus
}
#endif
#endif
