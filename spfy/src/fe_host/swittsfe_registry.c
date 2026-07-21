/* Language lookup over the build-time FE DLL registry.
 *
 * The table (`spfy_fe_dlls`) is generated into the build dir by
 * fe_host/CMakeLists.txt; only the lookup lives here.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later */

#include "swittsfe_registry.h"

#include <ctype.h>
#include <string.h>

static int lang_eq(const char *a, const char *b)
{
    /* VCF language tags are "en-US" style; compare case-insensitively and
     * treat '-' and '_' as equivalent so callers can pass either. */
    for (;; ++a, ++b) {
        unsigned char ca = (unsigned char)*a;
        unsigned char cb = (unsigned char)*b;
        if (ca == '_') ca = '-';
        if (cb == '_') cb = '-';
        ca = (unsigned char)tolower(ca);
        cb = (unsigned char)tolower(cb);
        if (ca != cb) return 0;
        if (ca == '\0') return 1;
    }
}

const spfy_fe_dll_entry_t *spfy_fe_dll_for_lang(const char *lang)
{
    if (!lang || !*lang) return NULL;
    for (size_t i = 0; i < spfy_fe_n_dlls; ++i) {
        if (lang_eq(spfy_fe_dlls[i].lang, lang)) return &spfy_fe_dlls[i];
    }
    return NULL;
}
