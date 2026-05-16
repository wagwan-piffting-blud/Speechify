#include "file_io.h"
#include "../../include/spfy/spfy.h"

#include <stdio.h>
#include <stdlib.h>

int spfy_slurp_file(const char *path, uint8_t **out, size_t *out_n)
{
    *out = NULL; *out_n = 0;

    FILE *fp = fopen(path, "rb");
    if (!fp) return SPFY_E_IO;

    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return SPFY_E_IO; }
    long sz = ftell(fp);
    if (sz < 0)                      { fclose(fp); return SPFY_E_IO; }
    if (fseek(fp, 0, SEEK_SET) != 0) { fclose(fp); return SPFY_E_IO; }

    uint8_t *buf = (uint8_t *)malloc((size_t)sz ? (size_t)sz : 1);
    if (!buf) { fclose(fp); return SPFY_E_NOMEM; }

    size_t n = fread(buf, 1, (size_t)sz, fp);
    fclose(fp);
    if (n != (size_t)sz) { free(buf); return SPFY_E_IO; }

    *out = buf;
    *out_n = n;
    return SPFY_OK;
}
