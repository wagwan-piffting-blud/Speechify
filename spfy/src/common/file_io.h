#ifndef SPFY_COMMON_FILE_IO_H
#define SPFY_COMMON_FILE_IO_H

#include <stddef.h>
#include <stdint.h>

/* Read entire file into a heap buffer. */
int spfy_slurp_file(const char *path, uint8_t **out, size_t *out_n);

#endif
