// loader.c - minimal PE32 loader for the guest DLL
#include "emu.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

pe_info_t PE;

// raw file bytes (kept for export/section parsing)
static uint8_t* g_file = NULL;
static long      g_filesz = 0;
static uint32_t  g_tls_rva = 0;   // IMAGE_DIRECTORY_ENTRY_TLS rva (0 if none)

static uint32_t rd32f(uint32_t off){ return g_file[off]|(g_file[off+1]<<8)|(g_file[off+2]<<16)|((uint32_t)g_file[off+3]<<24); }
static uint16_t rd16f(uint32_t off){ return g_file[off]|(g_file[off+1]<<8); }

// import registration lives in win32.c
int  win32_register_import(const char* dll, const char* name); // returns pseudo VA, or 0
int  win32_is_import_va(uint32_t va);

// defined later in this file
uint32_t rd32f_at_rva(uint32_t rva);
void     mem_read_file_str(uint32_t rva, char* out, int n);

int pe_load(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "pe_load: cannot open %s\n", path); return -1; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); fprintf(stderr, "pe_load: empty file\n"); return -1; }
    uint8_t* buf = (uint8_t*)malloc((size_t)sz);
    if (!buf) { fclose(f); return -1; }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) { fclose(f); free(buf); return -1; }
    fclose(f);
    int rc = pe_load_mem(buf, (uint32_t)sz);
    free(buf);   /* pe_load_mem copied into its own g_file */
    return rc;
}

int pe_load_mem(const uint8_t* bytes, uint32_t len) {
    if (!bytes || len < 0x40) { fprintf(stderr, "pe_load_mem: too small (%u)\n", len); return -1; }
    /* Stash the buffer as g_file so the existing RVA-helpers below
     * (rd32f / rd16f / mem_read_file_str) keep working against the
     * raw file image. We copy into freshly-malloc'd memory so the
     * caller can free its source buffer (e.g. an embedded const blob
     * doesn't need it, but a file-loaded buffer does — see pe_load
     * above which transfers ownership intentionally). */
    g_file = (uint8_t*)malloc(len);
    if (!g_file) { fprintf(stderr, "pe_load_mem: OOM\n"); return -1; }
    memcpy(g_file, bytes, len);
    g_filesz = (long)len;

    if (rd16f(0) != 0x5A4D) { fprintf(stderr, "not MZ\n"); return -1; }
    uint32_t e_lfanew = rd32f(0x3c);
    if (rd32f(e_lfanew) != 0x00004550) { fprintf(stderr, "not PE\n"); return -1; }
    uint32_t coff = e_lfanew + 4;
    uint16_t nsec = rd16f(coff + 2);
    uint16_t optsz = rd16f(coff + 16);
    uint32_t opt = coff + 20;
    uint16_t magic = rd16f(opt + 0);
    if (magic != 0x10b) { fprintf(stderr, "not PE32\n"); return -1; }

    PE.entry_rva     = rd32f(opt + 16);
    PE.image_base    = rd32f(opt + 28);
    PE.size_of_image = rd32f(opt + 56);
    // data dir 0 = export, 5 = base reloc, 1 = import, 9 = TLS
    uint32_t ddir = opt + 96;
    PE.export_rva  = rd32f(ddir + 0*8); PE.export_size = rd32f(ddir + 0*8 + 4);
    uint32_t imp_rva = rd32f(ddir + 1*8);
    uint32_t reloc_rva = rd32f(ddir + 5*8), reloc_sz = rd32f(ddir + 5*8 + 4);
    g_tls_rva = rd32f(ddir + 9*8);

    // ---- map the image ----
    uint8_t* img = mem_map(PE.image_base, PE.size_of_image, "image");
    uint32_t headers_sz = rd32f(opt + 60); // SizeOfHeaders
    memcpy(img, g_file, headers_sz);
    uint32_t sectab = opt + optsz;
    for (int i = 0; i < nsec; i++) {
        uint32_t s = sectab + i * 40;
        uint32_t vsize = rd32f(s + 8), vaddr = rd32f(s + 12);
        uint32_t rawsz = rd32f(s + 16), rawptr = rd32f(s + 20);
        uint32_t n = rawsz < vsize ? rawsz : vsize;
        if (rawptr + n <= (uint32_t)g_filesz && vaddr + n <= PE.size_of_image)
            memcpy(img + vaddr, g_file + rawptr, n);
    }

    int32_t delta = (int32_t)(PE.image_base - rd32f(opt + 28)); // we load AT preferred base ⇒ 0
    // (kept generic: apply base relocations if delta != 0)
    if (delta != 0 && reloc_rva && reloc_sz) {
        uint32_t p = reloc_rva, end = reloc_rva + reloc_sz;
        while (p < end) {
            uint32_t page = rd32(PE.image_base + p);
            uint32_t blk  = rd32(PE.image_base + p + 4);
            uint32_t cnt = (blk - 8) / 2;
            for (uint32_t k = 0; k < cnt; k++) {
                uint16_t e = rd16(PE.image_base + p + 8 + k*2);
                if ((e >> 12) == 3) { uint32_t a = PE.image_base + page + (e & 0xfff); wr32(a, rd32(a) + delta); }
            }
            p += blk;
        }
    }

    // ---- resolve imports ----
    if (imp_rva) {
        uint32_t d = imp_rva;
        for (;; d += 20) {
            uint32_t oft = rd32f_at_rva(d + 0);
            uint32_t name_rva = rd32f_at_rva(d + 12);
            uint32_t ft  = rd32f_at_rva(d + 16);
            if (name_rva == 0 && ft == 0 && oft == 0) break;
            char dll[64]; mem_read_file_str(name_rva, dll, sizeof dll);
            uint32_t thunks = oft ? oft : ft; // INT (names) - OFT preferred
            for (uint32_t t = 0;; t += 4) {
                uint32_t ent = rd32f_at_rva(thunks + t);
                if (ent == 0) break;
                char fname[96];
                if (ent & 0x80000000u) { snprintf(fname, sizeof fname, "#%u", ent & 0xffff); }
                else { uint32_t hint_rva = ent; mem_read_file_str(hint_rva + 2, fname, sizeof fname); }
                int pseudo = win32_register_import(dll, fname);
                wr32(PE.image_base + ft + t, (uint32_t)pseudo);  // patch IAT slot
            }
        }
    }

    // ---- stack, TEB, PEB ----
    mem_map(STACK_TOP - STACK_SIZE, STACK_SIZE, "stack");
    mem_map(IMP_BASE, MAX_IMPORTS * IMP_STRIDE, "imports");
    uint8_t* teb = mem_map(TEB_BASE, 0x1000, "teb");
    uint8_t* peb = mem_map(PEB_BASE, 0x1000, "peb");
    (void)peb;
    wr32(TEB_BASE + 0x00, 0xFFFFFFFF);            // SEH chain end
    wr32(TEB_BASE + 0x04, STACK_TOP);             // stack base (top)
    wr32(TEB_BASE + 0x08, STACK_TOP - STACK_SIZE);// stack limit
    wr32(TEB_BASE + 0x18, TEB_BASE);              // TEB self
    wr32(TEB_BASE + 0x20, 0x1000);                // process id
    wr32(TEB_BASE + 0x24, 0x2000);                // thread id
    wr32(TEB_BASE + 0x30, PEB_BASE);              // PEB
    wr32(PEB_BASE + 0x08, PE.image_base);         // ImageBaseAddress
    CPU.seg_fs_base = TEB_BASE;
    (void)teb;
    return 0;
}

// helpers that read from the mapped image by RVA (post-map, so relocations/patches are visible)
uint32_t rd32f_at_rva(uint32_t rva){ return rd32(PE.image_base + rva); }
void mem_read_file_str(uint32_t rva, char* out, int n){
    int i=0; for(; i<n-1; i++){ char c=(char)rd8(PE.image_base+rva+i); out[i]=c; if(!c) break; } out[i<n?i:n-1]=0;
}

uint32_t pe_get_export(const char* name) {
    if (!PE.export_rva) return 0;
    uint32_t e = PE.export_rva;
    uint32_t nfuncs = rd32(PE.image_base + e + 0x14);
    uint32_t nnames = rd32(PE.image_base + e + 0x18);
    uint32_t funcs  = rd32(PE.image_base + e + 0x1c);
    uint32_t names  = rd32(PE.image_base + e + 0x20);
    uint32_t ords   = rd32(PE.image_base + e + 0x24);
    (void)nfuncs;
    for (uint32_t i = 0; i < nnames; i++) {
        uint32_t nrva = rd32(PE.image_base + names + i*4);
        char buf[96]; mem_read_file_str(nrva, buf, sizeof buf);
        if (strcmp(buf, name) == 0) {
            uint16_t ord = rd16(PE.image_base + ords + i*2);
            uint32_t frva = rd32(PE.image_base + funcs + ord*4);
            return PE.image_base + frva;
        }
    }
    return 0;
}

void pe_run_dllmain(void) {
    // call entry(hinst=IMAGE_BASE, DLL_PROCESS_ATTACH=1, lpReserved=0)
    uint32_t entry = PE.image_base + PE.entry_rva;
    CPU.r[ESP] = STACK_ESP0;
    cpu_push32(0);             // lpReserved
    cpu_push32(1);             // DLL_PROCESS_ATTACH
    cpu_push32(PE.image_base); // hinstDLL
    cpu_push32(RET_SENTINEL);  // return address
    CPU.eip = entry;
    cpu_run(200000000ULL);
}

// Implicit TLS: what the Windows loader does for __declspec(thread) data. Allocate this
// "thread"'s TLS block (copy of the .tls template), set _tls_index=0, build a TLS pointer
// array, and point TEB->ThreadLocalStoragePointer (TEB+0x2c) at it. Then run TLS callbacks.
uint32_t call_guest(uint32_t fn, const uint32_t* args, int n);  // vst_host.c / ladspa_host.c
void pe_init_tls(void) {
    if (!g_tls_rva) return;
    uint32_t t = PE.image_base + g_tls_rva;
    uint32_t start = rd32(t+0), end = rd32(t+4), idxAddr = rd32(t+8), cbAddr = rd32(t+12), zero = rd32(t+16);
    uint32_t rawSize = (end > start) ? (end - start) : 0;
    uint32_t blkSize = rawSize + zero; if (blkSize < 8) blkSize = 8;
    uint32_t blk = guest_alloc(blkSize, 1);
    for (uint32_t i = 0; i < rawSize; i++) wr8(blk + i, rd8(start + i));   // copy template
    if (idxAddr) wr32(idxAddr, 0);                                          // _tls_index = 0
    uint32_t arr = guest_alloc(256*4, 1);                                   // TLS slot array
    wr32(arr + 0, blk);                                                     // slot[0] = block
    wr32(TEB_BASE + 0x2c, arr);                                             // TEB->ThreadLocalStoragePointer
    // run TLS callbacks (DLL_PROCESS_ATTACH), null-terminated array of PIMAGE_TLS_CALLBACK
    if (cbAddr) {
        for (uint32_t p = cbAddr;; p += 4) {
            uint32_t cb = rd32(p); if (!cb) break;
            uint32_t a[3] = { PE.image_base, 1, 0 };
            call_guest(cb, a, 3);
        }
    }
}
