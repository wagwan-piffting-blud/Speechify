// mem.c - guest virtual memory. Region list (for setup/teardown) + a flat page map
// (software TLB) that translates any guest VA to a host pointer in O(1) - no linear scan.
#include "emu.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

mem_t MEM;

#define PAGE_BITS 12
#define PAGE_SIZE 4096u
#define NUM_PAGES (1u << (32 - PAGE_BITS))   // 1,048,576 entries covering the full 4 GB VA space

// g_pagemap[va>>12] = host pointer for guest VA (page<<12), or NULL if unmapped. All regions are
// page-aligned (IMAGE_BASE/STACK/HEAP/TEB/... are >=0x1000 aligned), so each page maps to one region.
static uint8_t** g_pagemap = NULL;

static inline uint8_t* xlat(uint32_t va) {
    uint8_t* base = g_pagemap[va >> PAGE_BITS];
    return base ? base + (va & (PAGE_SIZE - 1)) : NULL;
}

void mem_init(void) {
    for (int i = 0; i < MEM.nreg; i++) free(MEM.regions[i].host);
    memset(&MEM, 0, sizeof(MEM));
    if (!g_pagemap) g_pagemap = (uint8_t**)calloc(NUM_PAGES, sizeof(uint8_t*));
    else memset(g_pagemap, 0, (size_t)NUM_PAGES * sizeof(uint8_t*));
}

uint8_t* mem_map(uint32_t va, uint32_t size, const char* name) {
    if (MEM.nreg >= MAX_REGIONS) { fprintf(stderr, "mem_map: too many regions\n"); exit(1); }
    size = (size + (PAGE_SIZE - 1)) & ~(PAGE_SIZE - 1);   // whole pages so the page map is always safe
    uint8_t* host = (uint8_t*)calloc(1, size);
    if (!host) { fprintf(stderr, "mem_map: OOM %u for %s\n", size, name); exit(1); }
    MEM.regions[MEM.nreg].va = va;
    MEM.regions[MEM.nreg].size = size;
    MEM.regions[MEM.nreg].host = host;
    MEM.regions[MEM.nreg].name = name;
    MEM.nreg++;
    if (g_pagemap) {
        uint32_t p0 = va >> PAGE_BITS, np = size >> PAGE_BITS;
        for (uint32_t k = 0; k < np; k++) g_pagemap[p0 + k] = host + ((size_t)k << PAGE_BITS);
    }
    return host;
}

region_t* mem_region_of(uint32_t va) {
    for (int i = 0; i < MEM.nreg; i++) {
        region_t* r = &MEM.regions[i];
        if (va >= r->va && va < r->va + r->size) return r;
    }
    return NULL;
}

uint8_t* mem_host(uint32_t va) { return xlat(va); }

// JIT: base address of the page-map array (a host pointer = a WASM linear-memory offset). Stable for
// the process lifetime (the array is calloc'd once and never freed). Compiled blocks inline
// g_pagemap[va>>12] to translate guest VAs without a call.
uint32_t mem_pagemap_base(void){ return (uint32_t)(uintptr_t)g_pagemap; }

static int g_faults = 0;
static void fault(uint32_t va, const char* what) {
    CPU.faulted = 1; CPU.fault_addr = va; CPU.fault_msg = what; CPU.halted = 1;
    if (g_faults++ < 2)
        fprintf(stderr, "** MEM FAULT %s @ 0x%08x eip=0x%08x edi=%08x ecx=%08x\n",
                what, va, CPU.eip, CPU.r[EDI], CPU.r[ECX]);
}

// Hot path: one page-map lookup, then native little-endian load/store (host is LE: x86/x64/wasm).
uint8_t  rd8 (uint32_t va){ uint8_t* p=xlat(va); if(!p){fault(va,"rd8"); return 0;} return *p; }
uint16_t rd16(uint32_t va){ uint8_t* p=xlat(va); if(!p){fault(va,"rd16");return 0;} uint16_t v; memcpy(&v,p,2); return v; }
uint32_t rd32(uint32_t va){ uint8_t* p=xlat(va); if(!p){fault(va,"rd32");return 0;} uint32_t v; memcpy(&v,p,4); return v; }
void wr8 (uint32_t va, uint8_t  v){ uint8_t* p=xlat(va); if(!p){fault(va,"wr8"); return;} *p=v; }
void wr16(uint32_t va, uint16_t v){ uint8_t* p=xlat(va); if(!p){fault(va,"wr16");return;} memcpy(p,&v,2); }
void wr32(uint32_t va, uint32_t v){ uint8_t* p=xlat(va); if(!p){fault(va,"wr32");return;} memcpy(p,&v,4); }

void mem_read(uint32_t va, void* dst, uint32_t n){
    uint8_t* d=(uint8_t*)dst;
    for(uint32_t i=0;i<n;i++) d[i]=rd8(va+i);
}
void mem_write(uint32_t va, const void* src, uint32_t n){
    const uint8_t* s=(const uint8_t*)src;
    for(uint32_t i=0;i<n;i++) wr8(va+i, s[i]);
}
