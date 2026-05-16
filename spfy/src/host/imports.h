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

#ifdef __cplusplus
}
#endif

#endif /* HOST_IMPORTS_H */
