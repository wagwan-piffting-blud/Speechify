#ifndef SPFY_COMMON_FILE_IO_H
#define SPFY_COMMON_FILE_IO_H

#include <stddef.h>
#include <stdint.h>

/* Read entire file into a heap buffer. */
int spfy_slurp_file(const char *path, uint8_t **out, size_t *out_n);

/* Private copy-on-write view of the file: writable, never written back, and
 * only the pages touched are ever read. Falls back to spfy_slurp_file where
 * mapping is unavailable; *mapped says which, for spfy_release_file. */
int  spfy_map_file(const char *path, uint8_t **out, size_t *out_n, int *mapped);
void spfy_release_file(uint8_t *p, size_t n, int mapped);

#endif
