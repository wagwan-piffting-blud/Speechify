#ifndef _SWI_TTS_H__
#define _SWI_TTS_H__

/****************License************************************************
 *
 * (C) Copyright 1996-2003.  SpeechWorks International, Inc.
 * (C) Copyright 2003.  ScanSoft.
 * All rights reserved.
 *
 * Use of this software is subject to certain restrictions and
 * limitations set forth in a license agreement entered into between
 * ScanSoft and the licensee of this software.  Please refer to the
 * license agreement for license use rights and restrictions.
 *
 * SpeechWorks is a registered trademark, and SpeechWorks Here,
 * DialogModules and the SpeechWorks logo are trademarks of ScanSoft
 * in the United States and other countries.
 *
 ************************************************************************
 */

/*****************************************************************************
 *****************************************************************************
 *
 * SWItts API definition for SpeechWorks Speechify
 *
 *****************************************************************************
 ****************************************************************************/

#include <time.h>           /* For time_t     */
#include <limits.h>         /* For UINT_MAX   */
#include "VXIvalue.h"       /* For VXIMap     */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Ensure that code that calls SWItts functions is compiled correctly
 * by telling the compiler about our calling convention and structure
 * packing alignment.
 */
#if defined(_MSC_VER)            /* Microsoft Visual C++ */
  #if !defined(SWIAPI)
    #define SWIAPI __stdcall
  #endif
  #pragma pack(push, 8)
#elif defined(__BORLANDC__)      /* Borland C++ */
  #if !defined(SWIAPI)
    #define SWIAPI __stdcall
  #endif
  #pragma option -a8
#elif defined(__WATCOMC__)       /* Watcom C++ */
  #if !defined(SWIAPI)
    #define SWIAPI __stdcall
  #endif
  #pragma pack(push, 8)
#else                            /* Any other including Unix */
  #if !defined(SWIAPI)
    #define SWIAPI
  #endif
#endif

/**
 * Types
 */
typedef unsigned char   SSFT_U8;
typedef unsigned short  SSFT_U16;
typedef unsigned long   SSFT_U32;

#ifndef NO_SSFT_UNICODE
#define SSFT_UNICODE 1
#endif
#ifdef SSFT_UNICODE
typedef wchar_t SSFT_TCHAR;
#else
typedef char    SSFT_TCHAR;
#endif

/**
 * Resources for SWIttsOpenPortEx( ), reserved for future use
 */
#ifdef __cplusplus
struct SWIttsResources;
#else
typedef struct SWIttsResources { void * dummy; } SWIttsResources;
#endif

/**
 * Speechify port handle used by all API calls
 */
typedef SSFT_U32              SWITTS_PORT_ID;
typedef SWITTS_PORT_ID        SWIttsPort;
#define SWITTS_INVALID_PORT   UINT_MAX

/**
 * License types for SWIttsResourceAllocate( )
 */
#define SWItts_LICENSE_SPEAK  "tts.license.speak"

/**
 * Maximum size, in characters, of a value returned by SWIttsGetParameter( )
 */
#define SWITTS_MAXVAL_SIZE  1000

/**
 * Dictionary and SpeakEx structure version
 */
#define SWItts_CURRENT_VERSION     0x00030000  /* version 3.0.0 */
#define SWItts_MAJOR_VERSION(x)    (((x) >> 16) & 0xFFFF)
#define SWItts_MINOR_VERSION(x)    (((x) >> 8) & 0xFF)
#define SWItts_PATCH_VERSION(x)    ((x) & 0xFF)

/**
 * Dictionary data for SWIttsDictionaryLoad/Free/Activate( )
 */
typedef struct SWIttsDictionaryData {
  SSFT_U32               version;          /* Use SWItts_CURRENT_VERSION */
  const char *           uri;
  const SSFT_U8 *        data;
  SSFT_U32               lengthBytes;
  const char *           contentType;
  const VXIMap *         fetchProperties;
  VXIVector *            fetchCookieJar;   /* Updated by the API call */
} SWIttsDictionaryData;

/**
 * Speak data for SWIttsSpeakEx( )
 */
typedef struct SWIttsSpeakData {
  SSFT_U32               version;          /* Use SWItts_CURRENT_VERSION */
  const char *           uri;
  const SSFT_U8 *        data;
  SSFT_U32               lengthBytes;
  const char *           contentType;
  const VXIMap *         fetchProperties;
  VXIVector *            fetchCookieJar;   /* Updated by the API call */
} SWIttsSpeakData;

/**
 * Status codes passed to the user supplied API callback function
 */
typedef enum SWItts_cbStatus {
  SWItts_cbStart = 0,
  SWItts_cbEnd,
  SWItts_cbStopped,
  SWItts_cbAudio,
  SWItts_cbBookmark,
  SWItts_cbPing,
  SWItts_cbError,
  SWItts_cbPortClosed,
  SWItts_cbWordmark,
  SWItts_cbPhonememark,
  SWItts_cbDiagnostic,
  SWItts_cbLogError,
  SWItts_cbPaused,
  SWItts_cbResumed
} SWItts_cbStatus;

/**
 * Audio packet passed to the user supplied API callback function when
 * the status is SWItts_cbAudio
 */
typedef struct SWIttsAudioPacket {
  void *    samples;
  SSFT_U32  numBytes;
  SSFT_U32  firstSampleNumber;
} SWIttsAudioPacket;

/**
 * Bookmark passed to the user supplied API callback function when the
 * status is SWItts_cbBookmark
 */
typedef struct SWIttsBookMark {
  const SSFT_TCHAR *  ID;
  SSFT_U32            sampleNumber;
} SWIttsBookMark;

/**
 * Word mark passed to the user supplied API callback function when
 * the status is SWItts_cbWordmark
 */
typedef struct SWIttsWordMark {
  SSFT_U32  sampleNumber;
  SSFT_U32  offset;
  SSFT_U32  length;
} SWIttsWordMark;

/**
 * Phoneme mark passed to the user supplied API callback function when
 * the status is SWItts_cbPhonememark
 */
typedef struct SWIttsPhonemeMark {
  SSFT_U32       sampleNumber;
  const char *   name;
  SSFT_U32       duration;
  SSFT_U32       stress;
} SWIttsPhonemeMark;

/**
 * Error or informational message passed to the user supplied API
 * callback function when the status is SWItts_cbDiagnostic or
 * SWItts_cbLogError
 */
typedef struct SWIttsMessagePacket {
  time_t               messageTime;
  SSFT_U16             messageTimeMs;
  SSFT_U32             msgID;
  const SSFT_TCHAR **  infoKeys;
  const SSFT_TCHAR **  infoValues;
  SSFT_U32             numKeys;
  const SSFT_TCHAR *   defaultMessage;
} SWIttsMessagePacket;

/**
 * Result codes for SWItts API functions
 */
typedef enum SWIttsResult {
  SWItts_SUCCESS = 0,
  SWItts_UNINITIALIZED,
  SWItts_INVALID_PARAMETER,
  SWItts_INVALID_PORT,
  SWItts_NO_ENGINE,           /* only for deprecated Speechify 2.x APIs */
  SWItts_ENGINE_ERROR,        /* only for deprecated Speechify 2.x APIs */
  SWItts_PROTOCOL_ERROR,
  SWItts_WINSOCK_FAILED,
  SWItts_SOCKET_ERROR,
  SWItts_CONNECT_ERROR,
  SWItts_NO_MEMORY,
  SWItts_NO_THREAD,
  SWItts_NO_MUTEX,
  SWItts_SERVER_ERROR,
  SWItts_HOST_NOT_FOUND,
  SWItts_FATAL_EXCEPTION,
  SWItts_MUST_BE_IDLE,
  SWItts_PORT_SHUTTING_DOWN,
  SWItts_PORT_ALREADY_SHUTTING_DOWN,
  SWItts_PORT_ALREADY_SHUT_DOWN,
  SWItts_ALREADY_EXECUTING_API,
  SWItts_NOT_EXECUTING_API,
  SWItts_ERROR_PORT_ALREADY_STOPPING,
  SWItts_ERROR_STOP_NOT_SPEAKING,
  SWItts_UPDATE_DICT_PARTIAL_SUCCESS,  /* only for deprecated
                                          Speechify 2.x APIs */
  SWItts_READ_ONLY,
  SWItts_UNKNOWN_CHARSET,
  SWItts_SSML_PARSE_ERROR,
  SWItts_URI_FETCH_ERROR,
  SWItts_URI_NOT_FOUND,
  SWItts_URI_TIMEOUT,
  SWItts_DICTIONARY_INVALID_TYPE,      /* only for deprecated
                                          Speechify 2.x APIs */
  SWItts_DICTIONARY_ACTIVE,
  SWItts_DICTIONARY_NOT_LOADED,
  SWItts_DICTIONARY_LOADED,
  SWItts_DICTIONARY_PRIORITY_ALREADY_EXISTS,
  SWItts_DICTIONARY_INVALID_PRIORITY,
  SWItts_DICTIONARY_PARSE_ERROR,
  SWItts_NO_LICENSE,
  SWItts_LICENSE_ALLOCATED,
  SWItts_LICENSE_FREED,
  SWItts_UNSUPPORTED,
  SWItts_ALREADY_INITIALIZED,
  SWItts_INVALID_MEDIATYPE,
  SWItts_NOT_SPEAKING,
  SWItts_NOT_PAUSED,
  SWItts_ALREADY_PAUSED,
} SWIttsResult;

/**
 * Logging IDs for client errors
 */
#define SWItts_LOGERRORBASE 100

#define SWItts_errLOGICUNKNOWN     (SWItts_LOGERRORBASE + 0)
#define SWItts_errLOGICUNEXPECTED  (SWItts_LOGERRORBASE + 1)
#define SWItts_errMEMORYALLOCATE   (SWItts_LOGERRORBASE + 2)
#define SWItts_errOSFAILURE        (SWItts_LOGERRORBASE + 3)
#define SWItts_errTTSENGINE        (SWItts_LOGERRORBASE + 4)
#define SWItts_errTRANSMIT         (SWItts_LOGERRORBASE + 5)
#define SWItts_errSOCKETSETUP      (SWItts_LOGERRORBASE + 6)
#define SWItts_errCONNECTIONLOST   (SWItts_LOGERRORBASE + 7)
#define SWItts_errTIMEOUT          (SWItts_LOGERRORBASE + 8)
#define SWItts_errDICTIONARY       (SWItts_LOGERRORBASE + 9)
#define SWItts_errSPEECH           (SWItts_LOGERRORBASE + 10)
#define SWItts_errEXECUTESPEAK     (SWItts_LOGERRORBASE + 11)
#define SWItts_errEXECUTESTOP      (SWItts_LOGERRORBASE + 12)
#define SWItts_errEXECUTEOPEN      (SWItts_LOGERRORBASE + 13)
#define SWItts_errEXECUTECLOSE     (SWItts_LOGERRORBASE + 14)
#define SWItts_errSTOPNOSPEECH     (SWItts_LOGERRORBASE + 15)
#define SWItts_errSTOPREPEATED     (SWItts_LOGERRORBASE + 16)
#define SWItts_errABORTING         (SWItts_LOGERRORBASE + 17)
#define SWItts_errILLEGALAPI       (SWItts_LOGERRORBASE + 18)
#define SWItts_errINITMULTIPLE     (SWItts_LOGERRORBASE + 19)
#define SWItts_errTHREADORMUTEX    (SWItts_LOGERRORBASE + 20)
#define SWItts_errSOCKETFAILED     (SWItts_LOGERRORBASE + 21)
#define SWItts_errOPENPARAM        (SWItts_LOGERRORBASE + 22)
#define SWItts_errPROTOCOL         (SWItts_LOGERRORBASE + 23)

/**
 * Logging IDs for server errors
 */
#define LOGERR_BASE           10000

#define LOGERR_FILENOTFOUND      (LOGERR_BASE + 0)
#define LOGERR_NETWORKFAILURE    (LOGERR_BASE + 1)
#define LOGERR_MEMORYFAILURE     (LOGERR_BASE + 2)
#define LOGERR_OSERROR           (LOGERR_BASE + 3)
#define LOGERR_PROTOCOLMISMATCH  (LOGERR_BASE + 4)
#define LOGERR_LOGICERROR        (LOGERR_BASE + 5)
#define LOGERR_GENERICERROR      (LOGERR_BASE + 6)
#define LOGERR_FILEIO            (LOGERR_BASE + 7)
#define LOGERR_INVALIDDATAFILE   (LOGERR_BASE + 8)

#define LOGERR_SSMLFAILURE       (LOGERR_BASE + 9)
#define LOGERR_SSMLINITFAILURE   (LOGERR_BASE + 10)
#define LOGERR_SSMLPARSEFAILURE  (LOGERR_BASE + 11)
#define LOGERR_INPUTTEXTERROR    (LOGERR_BASE + 12)


/**
 * @name SWIttsCallback( ) 
 * @memo User-supplied handler for audio buffers and notification messages. 
 *
 * @doc
 * Mode: Synchronous. 
 * IMPORTANT: You must not block/wait in this function.
 *
 * @param ttsPort   [IN] The port handle returned by SWIttsOpenPortEx( )
 *                    or SWITTS_INVALID_PORT if the callback is called
 *                    from within SWIttsInit( ), SWIttsOpenPortEx( ), or
 *                    SWIttsTerm( ).
 * @param status    [IN] These are enumerated types that are used to
 *                    inform the callback function of the port status and
 *                    the type of data pointed at by the "data" parameter.
 * @param data      [IN] Pointer to a structure containing data generated
 *                    by Speechify. This pointer is declared as void *
 *                    because the exact type varies. The status
 *                    parameter indicates the exact type to which 
 *                    this pointer should be cast.
 * @param userData  [IN] This is a void * in which the application
 *                    programmer may include any information that he/she
 *                    wishes to be passed back to the callback function.
 *                    A typical example is a thread ID that is meaningful to
 *                    the application. The userData variable is a value
 *                    you pass to SWIttsOpenPortEx( ).  
 */
typedef SWIttsResult (SWIAPI SWIttsCallback)(SWIttsPort       ttsPort,
                                             SWItts_cbStatus  status,
                                             void *           data,
                                             void *           userData);

/**
 * @name SWIttsInit( ) 
 * @memo Initializes the Speechify library so that it is ready to open ports. 
 *
 * @doc
 * Mode: Synchronous 
 *
 * @param callback  [IN] A pointer to a callback function that may
 *                    receive SWItts_cbLogError and/or
 *                    SWItts_cbDiagnostic messages during the
 *                    SWIttsInit( ) call. If this callback is called,
 *                    the ttsPort parameter is SWITTS_INVALID_PORT.
 *                    This may be the same callback that is passed to
 *                    SWIttsOpenPortEx( ) or SWIttsTerm( ).
 * @param userData  [IN] User information passed back to callback. It
 *                    is not interpreted or modified in any way by
 *                    Speechify.
 */
SWIttsResult SWIAPI SWIttsInit(SWIttsCallback *  callback,
                               void *            userData);

/**
 * @name SWIttsTerm( ) 
 * @memo Closes all ports, terminates their respective threads, shuts
 * down the API library, and cleans up memory usage.
 *
 * @doc
 * Mode: Synchronous 
 *
 * @param callback  [IN] A pointer to a callback function that may
 *                    receive SWItts_cbError, SWItts_cbLogError,
 *                    and/or SWItts_cbDiagnostic messages during the
 *                    SWIttsTerm( ) call.
 * @param userData  [IN] User information passed back to callback. 
 */
SWIttsResult SWIAPI SWIttsTerm(SWIttsCallback *  callback,
                               void *            userData);

/**
 * @name SWIttsOpenPortEx( ) 
 * @memo Creates and opens a Speechify resource.
 *
 * @doc
 * Mode: Synchronous 
 * Call this function after SWIttsInit( ).
 *
 * @param ttsPort     [OUT] Address of a location to place the new port's
 *                      handle.
 * @param parameters  [IN] Key/value parameter list in
 *                      <key1>=<value1>;<key2>=<value2> form. For the
 *                      Speechify client, the keys are "hostname"
 *                      and "hostport".
 * @param resources   [IN] Reserved for future use, pass NULL.
 * @param callback    [IN] A pointer to a callback function that receives
 *                      audio buffers and other notifications when 
 *                      Speechify sends data. If an error occurs during
 *                      the call to SWIttsOpenPortEx( ), the callback is
 *                      called with a SWItts_cbLogError message and a
 *                      ttsPort of SWITTS_INVALID_PORT.
 * @param userData    [IN] User information passed back to callback.
 */
SWIttsResult SWIAPI SWIttsOpenPortEx(SWIttsPort *       ttsPort,
                                     const char *       parameters,
                                     SWIttsResources *  resources,
                                     SWIttsCallback *   callback,
                                     void *             userData);

/**
 * @name SWIttsClosePort( ) 
 * @memo Closes a TTS port which frees all resources.
 *
 * @doc
 * Mode: Asynchronous 
 *
 * @param ttsPort  [IN] Port handle returned by SWIttsOpenPortEx( )
 */
SWIttsResult SWIAPI SWIttsClosePort(SWIttsPort  ttsPort);

/**
 * @name SWIttsResourceAllocate( ) 
 * @memo Explicitly assign a license for a specified Speechify port. 
 *
 * @doc
 * Mode: Synchronous 
 *
 * @param ttsPort   [IN] Port handle returned by SWIttsOpenPortEx( )
 * @param feature   [IN] Use SWItts_LICENSE_SPEAK for licensing functionality
 * @param reserved  [IN] This parameter is reserved for future use by
 *                       ScanSoft. Pass in NULL. 
 */
SWIttsResult SWIAPI SWIttsResourceAllocate(SWIttsPort     ttsPort,
                                           const char *   feature,
                                           void *         reserved);

/**
 * @name SWIttsResourceFree( ) 
 * @memo Explicitly free the user license for the specified Speechify port. 
 *
 * @doc
 * Mode: Synchronous 
 *
 * @param ttsPort   [IN] Port handle returned by SWIttsOpenPortEx( )
 * @param feature   [IN] Use SWItts_LICENSE_SPEAK for licensing functionality
 * @param reserved  [IN] This parameter is reserved for future use by
 *                       ScanSoft. Pass in NULL. 
 */
SWIttsResult SWIAPI SWIttsResourceFree(SWIttsPort     ttsPort,
                                       const char *   feature,
                                       void *         reserved);

/**
 * @name SWIttsSetParameter( ) 
 * @memo Sets a Speechify parameter.
 *
 * @doc
 * Mode: Synchronous 
 *
 * @param ttsPort  [IN] Port handle returned by SWIttsOpenPortEx( )
 * @param name     [IN] A parameter name represented as a NULL-terminated
 *                   US-ASCII string.
 * @param value    [IN] A parameter value represented as a NULL-terminated
 *                   US-ASCII string.
 */
SWIttsResult SWIAPI SWIttsSetParameter(SWIttsPort     ttsPort,
                                       const char *   name,
                                       const char *   value);

/**
 * @name SWIttsGetParameter( )
 * @memo Retrieves the value of a Speechify parameter.
 *
 * @doc
 * Mode: Synchronous 
 *
 * @param ttsPort  [IN] Port handle returned by SWIttsOpenPortEx( )
 * @param name     [IN] The name of the parameter to retrieve.
 * @param value    [OUT] Takes a preallocated buffer of size
 *                   SWITTS_MAXVAL_SIZE.
 */
SWIttsResult SWIAPI SWIttsGetParameter(SWIttsPort     ttsPort,
                                       const char *   name,
                                       char *         value);

/**
 * @name SWIttsSpeak( ) 
 * @memo Sends a text string to be synthesized. Call this function or
 * SWIttsSpeakEx( ) for every text string to synthesize.
 *
 * @doc
 * Mode: Asynchronous 
 * If the data is stored at a URI, use SWIttsSpeakEx( ) instead.
 *
 * @param ttsPort      [IN] Port handle returned by SWIttsOpenPortEx( )
 * @param text         [IN] The text to be synthesized: an array of bytes
 *                       representing a string in a given character set.
 * @param lengthBytes  [IN] The length of the text array in bytes; note
 *                       that this means any NULL in the text is treated
 *                       as just another character. 

 * @param contentType  [IN] Description of the input text according to
 *                       the MIME standard (per RFC-2045 Sec. 5.1 and
 *                       RFC 2046).
 */
SWIttsResult SWIAPI SWIttsSpeak(SWIttsPort       ttsPort,
                                const SSFT_U8 *  text,
                                SSFT_U32         lengthBytes,
                                const char *     contentType);

/**
 * @name SWIttsSpeakEx( ) 
 * @memo Sends a URI or a text string to be synthesized. Call this function
 * or SWIttsSpeak( ) for every URI or text string to synthesize.
 *
 * @doc
 * Mode: Asynchronous 
 *
 * @param ttsPort    [IN] Port handle returned by SWIttsOpenPortEx( )
 * @param speakData  [IN/OUT] Object containing the URI and fetch
 *                     parameters, or a string. If
 *                     speakData->fetchCookieJar is non-NULL and the
 *                     speak is successfully initiated, the cookie jar
 *                     will be updated with any cookies that resulted
 *                     from speak data fetches.  
 */
SWIttsResult SWIAPI SWIttsSpeakEx(SWIttsPort               ttsPort,
                                  const SWIttsSpeakData *  speakData);

/**
 * @name SWIttsStop( ) 
 * @memo Interrupts a speak operation.
 *
 * @doc
 * Mode: Asynchronous 
 *
 * @param ttsPort  [IN] Port handle returned by SWIttsOpenPortEx( )
 */
SWIttsResult SWIAPI SWIttsStop(SWIttsPort  ttsPort);


/**
 * @name SWIttsPause( ) 
 * @memo Pauses audio and mark delivery for a speak operation.
 *
 * @doc
 * Mode: Asynchronous 
 *
 * @param ttsPort  [IN] Port handle returned by SWIttsOpenPortEx( )
 **/
SWIttsResult SWIAPI SWIttsPause(SWIttsPort ttsPort);


/**
 * @name SWIttsResume( ) 
 * @memo Resumes audio and mark delivery for a paused speak operation.
 *
 * @doc
 * Mode: Asynchronous 
 *
 * @param ttsPort  [IN] Port handle returned by SWIttsOpenPortEx( )
 **/
SWIttsResult SWIAPI SWIttsResume(SWIttsPort ttsPort);


/**
 * @name SWIttsDictionaryLoad( ) 
 * @memo Load a complete dictionary from a URI or string to prepare it
 * for future activation.
 *
 * @doc
 * Mode: Synchronous 
 *
 * @param ttsPort     [IN] Port handle returned by SWIttsOpenPortEx( )
 * @param dictionary  [IN/OUT] Object containing the URI and fetch parameters,
 *                      or a string. If dictionary->fetchCookieJar is
 *                      non-NULL and the load completes successfully,
 *                      the cookie jar will be updated with any cookies
 *                      that resulted from dictionary fetches.
 */
SWIttsResult SWIAPI SWIttsDictionaryLoad(SWIttsPort              ttsPort,
                                   const SWIttsDictionaryData *  dictionary);

/**
 * @name SWIttsDictionaryFree( ) 
 * @memo Signals Speechify that the dictionary is no longer needed.
 *
 * @doc
 * Mode: Synchronous 
 *
 * @param ttsPort     [IN] Port handle returned by SWIttsOpenPortEx( )
 * @param dictionary  [IN] Object containing the URI and fetch parameters,
 *                      or a string.
 */
SWIttsResult SWIAPI SWIttsDictionaryFree(SWIttsPort               ttsPort,
                                    const SWIttsDictionaryData *  dictionary);

/**
 * @name SWIttsDictionaryActivate( ) 
 * @memo Activate a dictionary for subsequent speak requests.
 *
 * @doc
 * Mode: Synchronous 
 *
 * @param ttsPort     [IN] Port handle returned by SWIttsOpenPortEx( ). 
 * @param dictionary  [IN] Object containing the URI and fetch parameters,
 *                      or a string.
 * @param priority    [IN] Priority to assign to this dictionary compared
 *                      to other active dictionaries. Values: Integers
 *                      1 - 2^31. Lowest priority: 1.
 */
SWIttsResult SWIAPI SWIttsDictionaryActivate (SWIttsPort           ttsPort,
                                     const SWIttsDictionaryData *  dictionary,
                                     SSFT_U32                      priority);

/**
 * @name SWIttsDictionariesDeactivate( ) 
 * @memo Deactivates all activated dictionaries for subsequent speak requests. 
 *
 * @doc
 * Mode: Synchronous 
 *
 * @param ttsPort  [IN] Port handle returned by SWIttsOpenPortEx( )
 */
SWIttsResult SWIAPI SWIttsDictionariesDeactivate(SWIttsPort   ttsPort);

/**
 * IMPORTANT NOTE: 
 *
 * All of the functions and associated data structures listed below
 * are deprecated. They are only supported by this release for
 * backwards compatibility purposes, and may be removed in a future
 * release. Avoid using these.
 */

/**
 * Deprecated, the Speechify server performs heartbeating over
 * connections that eliminate any need for using this.  */
SWIttsResult SWIAPI SWIttsPing(SWIttsPort ttsPort);

/**
 * Deprecated, use SWIttsOpenPortEx( ) instead
 */
SWIttsResult SWIAPI SWIttsOpenPort(SWIttsPort *ttsPort,
#ifdef SWITTS_SOLO
                                   const char *parameters,
#else
                                   const char *hostAddr,
                                   SSFT_U16 connectionPort,
#endif
                                   SWIttsCallback *callback,
                                   void *userData);

/**
 * Deprecated user dictionary structures and APIs, use
 * SWIttsDictionaryLoad/Free/Activate( ) instead
 */
typedef struct SWIttsDictionaryEntry {
  const SSFT_U8 *  key;
  SSFT_U32         keyLengthBytes;
  const SSFT_U8 *  translation;
  SSFT_U32         translationLengthBytes;
} SWIttsDictionaryEntry;

typedef void *SWIttsDictionaryPosition;

SWIttsResult SWIAPI SWIttsAddDictionaryEntry(SWIttsPort ttsPort,
                                             const char *dictionaryType,
                                             const char *charset,
                                             SSFT_U32 numEntries,
                                             SWIttsDictionaryEntry *entries);

SWIttsResult SWIAPI SWIttsDeleteDictionaryEntry(SWIttsPort ttsPort,
                                               const char *dictionaryType,
                                               const char *charset,
                                               SSFT_U32 numEntries,
                                               SWIttsDictionaryEntry *entries);

SWIttsResult SWIAPI SWIttsResetDictionary(SWIttsPort ttsPort,
                                          const char *dictionaryType);

SWIttsResult SWIAPI SWIttsLookupDictionaryEntry(SWIttsPort ttsPort,
                                              const char *dictionaryType,
                                              const SSFT_U8 *key,
                                              const char *charset,
                                              SSFT_U32 keyLengthBytes,
                                              SSFT_U32 *numEntries,
                                              SWIttsDictionaryEntry **entries);

SWIttsResult SWIAPI SWIttsGetDictionaryKeys(SWIttsPort ttsPort,
                                    const char *dictionaryType,
                                    SWIttsDictionaryPosition *startingPosition,
                                    SSFT_U32 *numKeys,
                                    SWIttsDictionaryEntry **keys,
                                    const char *reserved);

#ifdef __cplusplus
}
#endif

/**
 * Reset the structure packing alignments for different compilers.
 */
#if defined(_MSC_VER)            /* Microsoft Visual C++ */
  #pragma pack(pop)
#elif defined(__BORLANDC__)      /* Borland C++ */
  #pragma option -a.
#elif defined(__WATCOMC__)       /* Watcom C++ */
  #pragma pack(pop)
#endif

#endif /* _SWI_TTS_H__ */
