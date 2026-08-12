#ifndef SPFY_SAPI_TMP_PATH_H
#define SPFY_SAPI_TMP_PATH_H

#include <windows.h>
#include <stdio.h>

/* Open a SAPI diagnostic log in the user's TEMP directory.
 *
 * These logs used to be hardcoded to C:/tmp, which is not a directory Windows
 * creates and not a drive root a voice DLL should assume it may write to. A
 * SAPI DLL runs inside whatever host loaded it, so a failed fopen there is
 * silent and the one diagnostic a host has just stops working.
 *
 * GetTempPathA always succeeds -- TMP, then TEMP, then USERPROFILE, then the
 * Windows directory -- and the string it returns already ends in a backslash.
 * Returns NULL if the path does not fit or the file cannot be opened; every
 * caller already treats NULL as "logging off". */
static FILE *spfy_sapi_debug_fopen(const char *leaf)
{
    char dir[MAX_PATH + 1];
    char path[MAX_PATH + 64];
    DWORD n = GetTempPathA(MAX_PATH, dir);
    if (n == 0u || n > MAX_PATH) return NULL;
    if (snprintf(path, sizeof path, "%s%s", dir, leaf) < 0) return NULL;
    return fopen(path, "a");
}

#endif
