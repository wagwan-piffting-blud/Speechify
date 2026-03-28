// swi_min.h – tiny header to bind to SWItts.dll without the SDK

#pragma once
#include <windows.h>

#ifndef SWIAPI
#define SWIAPI __stdcall
#endif

typedef int SWIttsResult;
typedef int SWIttsPort;
#define SWITTS_INVALID_PORT (-1)

/* Callback status codes (from SWItts.h) */
#define SWITTS_CB_START         0  /* SWItts_cbStart */
#define SWITTS_CB_END           1  /* SWItts_cbEnd */
#define SWITTS_CB_STOPPED       2  /* SWItts_cbStopped */
#define SWITTS_CB_AUDIO         3  /* SWItts_cbAudio */
#define SWITTS_CB_BOOKMARK      4  /* SWItts_cbBookmark */
#define SWITTS_CB_PING          5  /* SWItts_cbPing */
#define SWITTS_CB_ERROR         6  /* SWItts_cbError */
#define SWITTS_CB_PORTCLOSED    7  /* SWItts_cbPortClosed */
#define SWITTS_CB_WORDMARK      8  /* SWItts_cbWordmark */
#define SWITTS_CB_PHONEMEMARK   9  /* SWItts_cbPhonememark */
#define SWITTS_CB_DIAGNOSTIC   10  /* SWItts_cbDiagnostic */
#define SWITTS_CB_LOGERROR     11  /* SWItts_cbLogError */

/* Phoneme mark (status = SWItts_cbPhonememark) -- from SWItts.h */
typedef struct SWIttsPhonemeMark {
    unsigned long   sampleNumber;   /* sample offset in output stream */
    const char *    name;           /* phoneme name (ARPAbet) */
    unsigned long   duration;       /* duration in samples */
    unsigned long   stress;         /* 0=unstressed, 1=primary stress */
} SWIttsPhonemeMark;

/* Word mark (status = SWItts_cbWordmark) -- from SWItts.h */
typedef struct SWIttsWordMark {
    unsigned long   sampleNumber;   /* sample offset in output stream */
    unsigned long   offset;         /* character offset in source text */
    unsigned long   length;         /* length in characters */
} SWIttsWordMark;

typedef struct SWIttsAudioPacket {
  void *samples;              // network byte order
  unsigned int numBytes;
  unsigned int firstSampleNumber;
} SWIttsAudioPacket;

typedef struct SWIttsMessagePacket {
  time_t messageTime;
  unsigned short messageTimeMs;
  unsigned int msgID;
  unsigned int numKeys;
  const wchar_t **infoKeys;
  const wchar_t **infoValues;
  const wchar_t *defaultMessage;
} SWIttsMessagePacket;

typedef SWIttsResult (SWIAPI *SWIttsCallback)(
  SWIttsPort ttsPort,
  int status,         // treat as opaque; we’ll log it
  void *data,
  void *userData
);

// Function pointer types
typedef SWIttsResult (SWIAPI *PFN_SWIttsInit)(SWIttsCallback cb, void *userData);
typedef SWIttsResult (SWIAPI *PFN_SWIttsTerm)(SWIttsCallback cb, void *userData);
typedef SWIttsResult (SWIAPI *PFN_SWIttsOpenPortEx)(SWIttsPort *outPort, const char *parameters,
                                                    void *reserved, SWIttsCallback cb, void *userData);
typedef SWIttsResult (SWIAPI *PFN_SWIttsClosePort)(SWIttsPort port);
typedef SWIttsResult (SWIAPI *PFN_SWIttsSpeak)(SWIttsPort port, const unsigned char *text,
                                               unsigned int lengthBytes, const char *content_type);
typedef SWIttsResult (SWIAPI *PFN_SWIttsStop)(SWIttsPort port);
typedef SWIttsResult (SWIAPI *PFN_SWIttsSetParameter)(SWIttsPort port, const char *name, const char *value);
typedef SWIttsResult (SWIAPI *PFN_SWIttsGetParameter)(SWIttsPort port, const char *name, char *value);

#define SWITTS_MAXVAL_SIZE 1000

// Loader struct
typedef struct SWIttsAPI {
  HMODULE h;
  PFN_SWIttsInit Init;
  PFN_SWIttsTerm Term;
  PFN_SWIttsOpenPortEx OpenPortEx;
  PFN_SWIttsClosePort ClosePort;
  PFN_SWIttsSpeak Speak;
  PFN_SWIttsStop Stop;
  PFN_SWIttsSetParameter SetParameter;
  PFN_SWIttsGetParameter GetParameter;
} SWIttsAPI;

static int LoadSWItts(SWIttsAPI *api, const wchar_t *dllPath /* e.g. L".\\bin\\SWItts.dll" */) {
  ZeroMemory(api, sizeof(*api));
  api->h = LoadLibraryW(dllPath);
  if (!api->h) return 0;
  // Names are taken from the manual; if stdcall-decorated, GetProcAddress still accepts the undecorated name in most builds.
  api->Init       = (PFN_SWIttsInit)       GetProcAddress(api->h, "SWIttsInit");
  api->Term       = (PFN_SWIttsTerm)       GetProcAddress(api->h, "SWIttsTerm");
  api->OpenPortEx = (PFN_SWIttsOpenPortEx) GetProcAddress(api->h, "SWIttsOpenPortEx");
  api->ClosePort  = (PFN_SWIttsClosePort)  GetProcAddress(api->h, "SWIttsClosePort");
  api->Speak      = (PFN_SWIttsSpeak)      GetProcAddress(api->h, "SWIttsSpeak");
  api->Stop       = (PFN_SWIttsStop)       GetProcAddress(api->h, "SWIttsStop");
  api->SetParameter=(PFN_SWIttsSetParameter)GetProcAddress(api->h, "SWIttsSetParameter");
  api->GetParameter=(PFN_SWIttsGetParameter)GetProcAddress(api->h, "SWIttsGetParameter");
  if (!api->Init || !api->Term || !api->OpenPortEx || !api->ClosePort || !api->Speak || !api->Stop || !api->SetParameter || !api->GetParameter) {
    FreeLibrary(api->h);
    ZeroMemory(api, sizeof(*api));
    return 0;
  }
  return 1;
}
