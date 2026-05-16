/*
 * host/imports.h — default import resolver for the in-process PE loader.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef HOST_IMPORTS_H
#define HOST_IMPORTS_H

#include "loader.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Default import resolver. Pass this to host_dll_load(...). It backs
 * the imports that SWIttsFe-en-US.dll needs from KERNEL32/USER32/WINMM/
 * MSVCR71. Unknown imports return NULL. */
void *host_default_resolver(const char *dll,
                            const char *name_or_ordinal,
                            uint16_t ordinal,
                            void *user);

/* Linux-only: install a fake Thread Information Block and point FS at
 * it via set_thread_area(2). MSVC-compiled DLLs read fs:[0x00] (SEH
 * chain head) during function prologues — without a valid TIB, those
 * reads segfault. No-op on Windows. Returns 0 on success. */
int host_tib_setup_if_needed(void);

#ifdef __cplusplus
}
#endif

#endif /* HOST_IMPORTS_H */
