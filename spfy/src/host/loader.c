/*
 * host/loader.c — minimal in-memory PE32 loader.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Implements the loader interface declared in host/loader.h.
 *
 * Lifecycle:
 *   1. Parse DOS + NT headers, verify PE32 + i386 machine.
 *   2. Reserve image-sized region (prefer original ImageBase).
 *   3. Copy headers + each section to its target VA.
 *   4. Apply base relocations from .reloc.
 *   5. Resolve imports via caller-supplied resolver, patching IAT.
 *   6. Apply section page-protection per Characteristics.
 *   7. Invoke TLS callbacks (none expected — SWIttsFe imports
 *      DisableThreadLibraryCalls).
 *   8. Call DllMain(DLL_PROCESS_ATTACH).
 *
 * Only the i386 cases are implemented. AMD64 / ARM relocations are
 * stubbed and will fail loudly if encountered.
 */

#include "loader.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
  #define HOST_STDCALL __stdcall
#else
  #include <sys/mman.h>
  #include <unistd.h>
  #ifndef MAP_ANONYMOUS
    #define MAP_ANONYMOUS MAP_ANON
  #endif
  #if defined(__i386__)
    #define HOST_STDCALL __attribute__((stdcall))
  #else
    #define HOST_STDCALL
  #endif
#endif

/* ---- PE format structs (kept self-contained; do not pull in windows.h
 * structs because the cross-platform build won't have them). ---- */
#pragma pack(push, 1)
typedef struct {
    uint16_t e_magic;       /* "MZ" */
    uint16_t e_cblp;
    uint16_t e_cp;
    uint16_t e_crlc;
    uint16_t e_cparhdr;
    uint16_t e_minalloc;
    uint16_t e_maxalloc;
    uint16_t e_ss;
    uint16_t e_sp;
    uint16_t e_csum;
    uint16_t e_ip;
    uint16_t e_cs;
    uint16_t e_lfarlc;
    uint16_t e_ovno;
    uint16_t e_res[4];
    uint16_t e_oemid;
    uint16_t e_oeminfo;
    uint16_t e_res2[10];
    int32_t  e_lfanew;
} pe_dos_header_t;

typedef struct {
    uint16_t Machine;          /* 0x014c = i386 */
    uint16_t NumberOfSections;
    uint32_t TimeDateStamp;
    uint32_t PointerToSymbolTable;
    uint32_t NumberOfSymbols;
    uint16_t SizeOfOptionalHeader;
    uint16_t Characteristics;
} pe_file_header_t;

typedef struct {
    uint32_t VirtualAddress;
    uint32_t Size;
} pe_data_dir_t;

#define PE_NUM_DATA_DIR 16
typedef struct {
    uint16_t Magic;            /* 0x010b = PE32 */
    uint8_t  MajorLinkerVersion;
    uint8_t  MinorLinkerVersion;
    uint32_t SizeOfCode;
    uint32_t SizeOfInitializedData;
    uint32_t SizeOfUninitializedData;
    uint32_t AddressOfEntryPoint;
    uint32_t BaseOfCode;
    uint32_t BaseOfData;
    uint32_t ImageBase;
    uint32_t SectionAlignment;
    uint32_t FileAlignment;
    uint16_t MajorOperatingSystemVersion;
    uint16_t MinorOperatingSystemVersion;
    uint16_t MajorImageVersion;
    uint16_t MinorImageVersion;
    uint16_t MajorSubsystemVersion;
    uint16_t MinorSubsystemVersion;
    uint32_t Win32VersionValue;
    uint32_t SizeOfImage;
    uint32_t SizeOfHeaders;
    uint32_t CheckSum;
    uint16_t Subsystem;
    uint16_t DllCharacteristics;
    uint32_t SizeOfStackReserve;
    uint32_t SizeOfStackCommit;
    uint32_t SizeOfHeapReserve;
    uint32_t SizeOfHeapCommit;
    uint32_t LoaderFlags;
    uint32_t NumberOfRvaAndSizes;
    pe_data_dir_t DataDirectory[PE_NUM_DATA_DIR];
} pe_opt_header32_t;

typedef struct {
    uint32_t        Signature;       /* "PE\0\0" */
    pe_file_header_t FileHeader;
    pe_opt_header32_t OptionalHeader;
} pe_nt_headers32_t;

#define PE_SIZEOF_SHORT_NAME 8
typedef struct {
    uint8_t  Name[PE_SIZEOF_SHORT_NAME];
    union { uint32_t PhysicalAddress; uint32_t VirtualSize; } Misc;
    uint32_t VirtualAddress;
    uint32_t SizeOfRawData;
    uint32_t PointerToRawData;
    uint32_t PointerToRelocations;
    uint32_t PointerToLinenumbers;
    uint16_t NumberOfRelocations;
    uint16_t NumberOfLinenumbers;
    uint32_t Characteristics;
} pe_section_header_t;

typedef struct {
    uint32_t OriginalFirstThunk;     /* RVA of ILT (name table) */
    uint32_t TimeDateStamp;
    uint32_t ForwarderChain;
    uint32_t Name;                   /* RVA of imported DLL name (ascii) */
    uint32_t FirstThunk;             /* RVA of IAT (resolved addresses) */
} pe_import_descriptor_t;

typedef struct {
    uint32_t VirtualAddress;         /* RVA of block */
    uint32_t SizeOfBlock;            /* total bytes incl. this header */
    /* uint16_t entries[] follow */
} pe_base_relocation_t;

typedef struct {
    uint32_t Characteristics;
    uint32_t TimeDateStamp;
    uint16_t MajorVersion;
    uint16_t MinorVersion;
    uint32_t Name;
    uint32_t Base;
    uint32_t NumberOfFunctions;
    uint32_t NumberOfNames;
    uint32_t AddressOfFunctions;     /* RVA to array of RVAs of exports   */
    uint32_t AddressOfNames;         /* RVA to array of RVAs of name strs */
    uint32_t AddressOfNameOrdinals;  /* RVA to array of uint16_t ordinals */
} pe_export_directory_t;
#pragma pack(pop)

#define PE_IMAGE_ORDINAL_FLAG32 0x80000000u
#define PE_IMAGE_ORDINAL32(x)   ((x) & 0xffffu)

/* Section characteristics flags we care about */
#define PE_SCN_MEM_EXECUTE 0x20000000u
#define PE_SCN_MEM_READ    0x40000000u
#define PE_SCN_MEM_WRITE   0x80000000u

#define PE_DIR_EXPORT    0
#define PE_DIR_IMPORT    1
#define PE_DIR_RESOURCE  2
#define PE_DIR_EXCEPTION 3
#define PE_DIR_SECURITY  4
#define PE_DIR_BASERELOC 5
#define PE_DIR_TLS       9
#define PE_DIR_LOAD_CFG  10

#define PE_REL_ABSOLUTE  0
#define PE_REL_HIGHLOW   3
#define PE_REL_DIR64    10

#define DLL_PROCESS_ATTACH 1
#define DLL_PROCESS_DETACH 0

typedef int32_t (HOST_STDCALL *dllmain_fn)(void *hinst, uint32_t reason, void *reserved);

struct host_dll {
    uint8_t *base;          /* mapped image base */
    size_t   image_size;
    pe_nt_headers32_t *nt;  /* points into mapped image headers */
    dllmain_fn entry;       /* may be NULL */
    int attached;
};

/* ---- last-error glue ---- */
static char g_last_err[256];

static void seterr(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_last_err, sizeof(g_last_err), fmt, ap);
    va_end(ap);
}

const char *host_dll_last_error(void) { return g_last_err; }

/* ---- Cross-platform virtual memory helpers ---- */

static void *vm_reserve(void *desired, size_t size) {
#ifdef _WIN32
    void *p = VirtualAlloc(desired, size,
                           MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!p && desired) p = VirtualAlloc(NULL, size,
                           MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    return p;
#else
    void *p = mmap(desired, size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return NULL;
    return p;
#endif
}

static void vm_release(void *p, size_t size) {
#ifdef _WIN32
    (void)size;
    VirtualFree(p, 0, MEM_RELEASE);
#else
    munmap(p, size);
#endif
}

static int vm_protect(void *p, size_t size, uint32_t scn_chars) {
#ifdef _WIN32
    DWORD prot = PAGE_NOACCESS;
    int ex = (scn_chars & PE_SCN_MEM_EXECUTE) != 0;
    int wr = (scn_chars & PE_SCN_MEM_WRITE)   != 0;
    int rd = (scn_chars & PE_SCN_MEM_READ)    != 0;
    if (ex && wr) prot = PAGE_EXECUTE_READWRITE;
    else if (ex && rd) prot = PAGE_EXECUTE_READ;
    else if (ex) prot = PAGE_EXECUTE;
    else if (wr) prot = PAGE_READWRITE;
    else if (rd) prot = PAGE_READONLY;
    DWORD old;
    return VirtualProtect(p, size, prot, &old) ? 0 : -1;
#else
    int prot = 0;
    if (scn_chars & PE_SCN_MEM_READ)    prot |= PROT_READ;
    if (scn_chars & PE_SCN_MEM_WRITE)   prot |= PROT_WRITE;
    if (scn_chars & PE_SCN_MEM_EXECUTE) prot |= PROT_EXEC;
    return mprotect(p, size, prot ? prot : PROT_NONE);
#endif
}

/* ---- core loader ---- */

static pe_section_header_t *section_table(const pe_nt_headers32_t *nt) {
    return (pe_section_header_t *)((const uint8_t *)&nt->OptionalHeader +
                                   nt->FileHeader.SizeOfOptionalHeader);
}

static int copy_headers_and_sections(host_dll_t *dll,
                                     const uint8_t *src,
                                     size_t src_size) {
    pe_nt_headers32_t *src_nt =
        (pe_nt_headers32_t *)(src + ((pe_dos_header_t *)src)->e_lfanew);

    memcpy(dll->base, src, src_nt->OptionalHeader.SizeOfHeaders);
    dll->nt = (pe_nt_headers32_t *)(dll->base +
                ((pe_dos_header_t *)dll->base)->e_lfanew);

    pe_section_header_t *sec = section_table(src_nt);
    for (uint16_t i = 0; i < src_nt->FileHeader.NumberOfSections; i++) {
        if (sec[i].PointerToRawData + sec[i].SizeOfRawData > src_size) {
            seterr("section %u out of range", i);
            return -1;
        }
        if (sec[i].SizeOfRawData) {
            memcpy(dll->base + sec[i].VirtualAddress,
                   src + sec[i].PointerToRawData,
                   sec[i].SizeOfRawData);
        }
        /* Zero the BSS portion (VirtualSize > SizeOfRawData). vm_reserve
         * already gave us zeroed pages, but be explicit. */
        uint32_t vsize = sec[i].Misc.VirtualSize;
        if (vsize > sec[i].SizeOfRawData) {
            memset(dll->base + sec[i].VirtualAddress + sec[i].SizeOfRawData,
                   0, vsize - sec[i].SizeOfRawData);
        }
    }
    return 0;
}

static int apply_relocs(host_dll_t *dll) {
    pe_data_dir_t *d = &dll->nt->OptionalHeader.DataDirectory[PE_DIR_BASERELOC];
    if (!d->Size) return 0;

    intptr_t delta = (intptr_t)dll->base -
                     (intptr_t)dll->nt->OptionalHeader.ImageBase;
    if (delta == 0) return 0;

    uint8_t *p   = dll->base + d->VirtualAddress;
    uint8_t *end = p + d->Size;
    while (p < end) {
        pe_base_relocation_t *blk = (pe_base_relocation_t *)p;
        if (blk->SizeOfBlock == 0) break;
        uint32_t n = (blk->SizeOfBlock - sizeof(*blk)) / 2;
        uint16_t *e = (uint16_t *)(blk + 1);
        for (uint32_t i = 0; i < n; i++) {
            uint32_t type = e[i] >> 12;
            uint32_t off  = e[i] & 0xfff;
            if (type == PE_REL_ABSOLUTE) continue;
            if (type == PE_REL_HIGHLOW) {
                uint32_t *target = (uint32_t *)(dll->base +
                                                blk->VirtualAddress + off);
                *target = (uint32_t)((uint32_t)*target + (uint32_t)delta);
            } else {
                seterr("unsupported reloc type %u", type);
                return -1;
            }
        }
        p += blk->SizeOfBlock;
    }
    return 0;
}

static int resolve_imports(host_dll_t *dll,
                           host_import_resolver resolve,
                           void *user) {
    pe_data_dir_t *d = &dll->nt->OptionalHeader.DataDirectory[PE_DIR_IMPORT];
    if (!d->Size) return 0;

    pe_import_descriptor_t *imp =
        (pe_import_descriptor_t *)(dll->base + d->VirtualAddress);

    /* Lowercased DLL name buffer */
    char dll_name[64];

    for (; imp->Name; imp++) {
        const char *raw = (const char *)(dll->base + imp->Name);
        size_t i;
        for (i = 0; i < sizeof(dll_name) - 1 && raw[i]; i++) {
            char c = raw[i];
            if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
            dll_name[i] = c;
        }
        dll_name[i] = '\0';

        uint32_t *ilt = (uint32_t *)(dll->base +
            (imp->OriginalFirstThunk ? imp->OriginalFirstThunk
                                      : imp->FirstThunk));
        uint32_t *iat = (uint32_t *)(dll->base + imp->FirstThunk);
        for (; *ilt; ilt++, iat++) {
            void *fn;
            if (*ilt & PE_IMAGE_ORDINAL_FLAG32) {
                fn = resolve(dll_name, NULL,
                             (uint16_t)PE_IMAGE_ORDINAL32(*ilt), user);
            } else {
                /* hint/name: 2-byte hint then ASCII name */
                const char *name =
                    (const char *)(dll->base + *ilt + 2);
                fn = resolve(dll_name, name, 0, user);
            }
            if (!fn) {
                if (*ilt & PE_IMAGE_ORDINAL_FLAG32)
                    seterr("unresolved import %s @%u", dll_name,
                           (unsigned)PE_IMAGE_ORDINAL32(*ilt));
                else
                    seterr("unresolved import %s!%s", dll_name,
                           (const char *)(dll->base + *ilt + 2));
                return -1;
            }
            *iat = (uint32_t)(uintptr_t)fn;
        }
    }
    return 0;
}

static int apply_section_protection(host_dll_t *dll) {
    pe_section_header_t *sec = section_table(dll->nt);
    for (uint16_t i = 0; i < dll->nt->FileHeader.NumberOfSections; i++) {
        uint8_t *p = dll->base + sec[i].VirtualAddress;
        uint32_t vsize = sec[i].Misc.VirtualSize ? sec[i].Misc.VirtualSize
                                                 : sec[i].SizeOfRawData;
        /* Round up to alignment */
        uint32_t align = dll->nt->OptionalHeader.SectionAlignment;
        uint32_t mapped = (vsize + align - 1) & ~(align - 1);
        if (vm_protect(p, mapped, sec[i].Characteristics) != 0) {
            seterr("vm_protect failed on section %u", i);
            return -1;
        }
    }
    return 0;
}

host_dll_t *host_dll_load(const void *bytes, size_t size,
                          host_import_resolver resolve, void *user) {
    /* Host-architecture guard: the DLL is 32-bit x86 PE; the host
     * process must also be 32-bit. A 64-bit host would copy 32-bit
     * code into virtual memory and then jump into it — the CPU would
     * decode it as 64-bit instructions and SEGV. */
    if (sizeof(void *) != 4) {
        seterr("host process is %zu-bit; SWIttsFe-en-US.dll requires a "
               "32-bit host (rebuild with -m32 / configure 32-bit toolchain)",
               sizeof(void *) * 8);
        return NULL;
    }
    if (!bytes || size < sizeof(pe_dos_header_t)) {
        seterr("buffer too small");
        return NULL;
    }
    const uint8_t *src = (const uint8_t *)bytes;
    pe_dos_header_t *dos = (pe_dos_header_t *)src;
    if (dos->e_magic != 0x5a4d) { seterr("bad DOS magic"); return NULL; }
    if ((size_t)dos->e_lfanew + sizeof(pe_nt_headers32_t) > size) {
        seterr("e_lfanew out of range"); return NULL;
    }
    pe_nt_headers32_t *nt =
        (pe_nt_headers32_t *)(src + dos->e_lfanew);
    if (nt->Signature != 0x00004550) { seterr("not PE"); return NULL; }
    if (nt->FileHeader.Machine != 0x014c) {
        seterr("not i386 (machine=0x%x)", nt->FileHeader.Machine);
        return NULL;
    }
    if (nt->OptionalHeader.Magic != 0x010b) {
        seterr("not PE32 (opt magic=0x%x)", nt->OptionalHeader.Magic);
        return NULL;
    }

    host_dll_t *dll = (host_dll_t *)calloc(1, sizeof(*dll));
    if (!dll) { seterr("oom"); return NULL; }
    dll->image_size = nt->OptionalHeader.SizeOfImage;

    dll->base = (uint8_t *)vm_reserve(
        (void *)(uintptr_t)nt->OptionalHeader.ImageBase, dll->image_size);
    if (!dll->base) {
        seterr("vm_reserve failed for %zu bytes", dll->image_size);
        free(dll); return NULL;
    }

    if (copy_headers_and_sections(dll, src, size) != 0) goto fail;
    if (apply_relocs(dll) != 0) goto fail;
    if (resolve_imports(dll, resolve, user) != 0) goto fail;
    if (apply_section_protection(dll) != 0) goto fail;

    /* TLS callbacks — SWIttsFe doesn't use TLS (it calls
     * DisableThreadLibraryCalls in DllMain) but we honor the directory
     * if present. */
    pe_data_dir_t *tls_dir = &dll->nt->OptionalHeader.DataDirectory[PE_DIR_TLS];
    if (tls_dir->Size) {
        /* IMAGE_TLS_DIRECTORY32 layout: StartAddressOfRawData,
         * EndAddressOfRawData, AddressOfIndex, AddressOfCallBacks, etc.
         * We only need the callbacks array (a NULL-terminated array of
         * PIMAGE_TLS_CALLBACK == fn(hinst, reason, reserved)). */
        uint32_t *tls = (uint32_t *)(dll->base + tls_dir->VirtualAddress);
        uint32_t cb_va = tls[3];
        if (cb_va) {
            uint32_t *cb = (uint32_t *)(uintptr_t)cb_va;  /* absolute, post-reloc */
            while (*cb) {
                ((dllmain_fn)(uintptr_t)*cb)(dll->base, DLL_PROCESS_ATTACH, NULL);
                cb++;
            }
        }
    }

    if (nt->OptionalHeader.AddressOfEntryPoint) {
        dll->entry = (dllmain_fn)(dll->base +
                                  nt->OptionalHeader.AddressOfEntryPoint);
        int32_t r = dll->entry(dll->base, DLL_PROCESS_ATTACH, NULL);
        if (!r) {
            seterr("DllMain(DLL_PROCESS_ATTACH) returned 0");
            goto fail;
        }
        dll->attached = 1;
    }
    return dll;
fail:
    if (dll->base) vm_release(dll->base, dll->image_size);
    free(dll);
    return NULL;
}

void *host_dll_get_proc(host_dll_t *dll, const char *name) {
    if (!dll || !name) return NULL;
    pe_data_dir_t *d = &dll->nt->OptionalHeader.DataDirectory[PE_DIR_EXPORT];
    if (!d->Size) return NULL;
    pe_export_directory_t *e =
        (pe_export_directory_t *)(dll->base + d->VirtualAddress);
    uint32_t *funcs   = (uint32_t *)(dll->base + e->AddressOfFunctions);
    uint32_t *names   = (uint32_t *)(dll->base + e->AddressOfNames);
    uint16_t *ords    = (uint16_t *)(dll->base + e->AddressOfNameOrdinals);
    for (uint32_t i = 0; i < e->NumberOfNames; i++) {
        const char *n = (const char *)(dll->base + names[i]);
        if (strcmp(n, name) == 0) {
            uint32_t rva = funcs[ords[i]];
            return dll->base + rva;
        }
    }
    seterr("export %s not found", name);
    return NULL;
}

const void *host_dll_image_base(host_dll_t *dll) {
    return dll ? dll->base : NULL;
}

void host_dll_free(host_dll_t *dll) {
    if (!dll) return;
    if (dll->attached && dll->entry) {
        dll->entry(dll->base, DLL_PROCESS_DETACH, NULL);
    }
    if (dll->base) vm_release(dll->base, dll->image_size);
    free(dll);
}
