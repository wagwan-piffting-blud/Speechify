#include "file_io.h"
#include "../../include/spfy/spfy.h"

#include <stdio.h>
#include <stdlib.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#elif !defined(__EMSCRIPTEN__)
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#define SPFY_HAVE_MMAP 1
#endif

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

int spfy_map_file(const char *path, uint8_t **out, size_t *out_n, int *mapped)
{
    *out = NULL; *out_n = 0; *mapped = 0;
#if defined(_WIN32)
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f != INVALID_HANDLE_VALUE) {
        LARGE_INTEGER sz;
        if (GetFileSizeEx(f, &sz) && sz.QuadPart > 0
            && (unsigned long long)sz.QuadPart <= (size_t)-1) {
            HANDLE m = CreateFileMappingA(f, NULL, PAGE_WRITECOPY, 0, 0, NULL);
            if (m) {
                void *v = MapViewOfFile(m, FILE_MAP_COPY, 0, 0, 0);
                CloseHandle(m);
                if (v) {
                    CloseHandle(f);
                    *out = (uint8_t *)v;
                    *out_n = (size_t)sz.QuadPart;
                    *mapped = 1;
                    return SPFY_OK;
                }
            }
        }
        CloseHandle(f);
    }
#elif defined(SPFY_HAVE_MMAP)
    int fd = open(path, O_RDONLY);
    if (fd >= 0) {
        struct stat st;
        if (fstat(fd, &st) == 0 && st.st_size > 0) {
            void *v = mmap(NULL, (size_t)st.st_size, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE, fd, 0);
            if (v != MAP_FAILED) {
                close(fd);
                *out = (uint8_t *)v;
                *out_n = (size_t)st.st_size;
                *mapped = 1;
                return SPFY_OK;
            }
        }
        close(fd);
    }
#endif
    return spfy_slurp_file(path, out, out_n);
}

void spfy_release_file(uint8_t *p, size_t n, int mapped)
{
    if (!p) return;
    if (!mapped) { free(p); return; }
#if defined(_WIN32)
    (void)n;
    UnmapViewOfFile(p);
#elif defined(SPFY_HAVE_MMAP)
    munmap(p, n);
#else
    (void)n;
#endif
}
