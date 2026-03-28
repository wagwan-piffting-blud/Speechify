/*****************************************************************************

  Copyright 1996-2003 SpeechWorks International, Inc.  All Rights Reserved.

  Header File: SWIEmail.h

  API functions, return codes and mode definitions

*****************************************************************************/

#ifndef _SWI_EMAIL_H__
#define _SWI_EMAIL_H__

#ifdef __cplusplus
extern "C" {
#endif

#if !defined(SWIAPI)
  #if defined(WIN32)
    #define SWIAPI __stdcall
  #else
    #define SWIAPI
  #endif
#endif

/*****************************************************************************

                  MODES

*****************************************************************************/

#define DATE 0x01
#define FROM 0x02
#define SUBJECT 0x04
#define BODY 0x08
#define ADDRESS 0x10
#define MIME_FORMAT 0x20

/*****************************************************************************

                RETURN CODES

*****************************************************************************/

  typedef enum{
    SWIemail_SUCCESS = 0,
    SWIemail_MEM_REQUEST,
    SWIemail_ERROR,
    SWIemail_FILE_NOT_FOUND,
    SWIemail_BAD_FILE_FORMAT,
    SWIemail_UNINITIALIZED,
    SWIemail_EMPTY_MESSAGE,
    SWIemail_FATAL_EXCEPTION
  } SWIemailResult;

/*****************************************************************************

  Function:   SWIemailInit()

  Verifies and sorts the user dictionary and loads it into memory

  Returns:    SWIemail_SUCCESS    Successfully loaded Email.Dic
              SWIemail_ERROR      Couldn't find Email.Dic

*****************************************************************************/

SWIemailResult SWIAPI SWIemailInit(const char *FilePath);

/*****************************************************************************

  Function: SWIemailProcess()

  Top-level function for e-mail pre-processing.

  Returns:  SWIemail_SUCCESS        Successfully processed msg
            SWIemail_MEM_REQUEST    Successfully processed msg, but need more
                                    memory
            SWIemail_EMPTY_MESSAGE  Msg was empty
            SWIemail_ERROR          Memory error

*****************************************************************************/

SWIemailResult SWIAPI SWIemailProcess(char *Msg, int *MsgSize,
                                      unsigned char Modes);

/*****************************************************************************

  Function: SWIemailTerm()

  Frees the dictionary

  Returns:  SWIemail_SUCCESS

*****************************************************************************/

SWIemailResult SWIAPI SWIemailTerm(void);

#ifdef __cplusplus
}
#endif

#endif
