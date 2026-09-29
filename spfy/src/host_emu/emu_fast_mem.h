/* Inline guest memory access for cpu.c: the same one-load page-map
 * translation mem.c's xlat() does, without a call per access. An unmapped
 * page takes the real rd8..wr32 so faults are reported exactly as before. */
#ifndef EMU_FAST_MEM_H
#define EMU_FAST_MEM_H

#include <string.h>
#include "emu.h"

extern uint8_t** g_pagemap;

#ifndef EMU_AINL
#if defined(__GNUC__) || defined(__clang__)
#define EMU_AINL inline __attribute__((always_inline))
#elif defined(_MSC_VER)
#define EMU_AINL __forceinline
#else
#define EMU_AINL inline
#endif
#endif

static EMU_AINL uint8_t  fm_rd8 (uint32_t va){ uint8_t* b=g_pagemap[va>>12]; if(!b) return rd8(va);  return b[va&0xfff]; }
static EMU_AINL uint16_t fm_rd16(uint32_t va){ uint8_t* b=g_pagemap[va>>12]; if(!b) return rd16(va); uint16_t v; memcpy(&v,b+(va&0xfff),2); return v; }
static EMU_AINL uint32_t fm_rd32(uint32_t va){ uint8_t* b=g_pagemap[va>>12]; if(!b) return rd32(va); uint32_t v; memcpy(&v,b+(va&0xfff),4); return v; }
static EMU_AINL void fm_wr8 (uint32_t va, uint8_t  v){ uint8_t* b=g_pagemap[va>>12]; if(!b){ wr8(va,v);  return; } b[va&0xfff]=v; }
static EMU_AINL void fm_wr16(uint32_t va, uint16_t v){ uint8_t* b=g_pagemap[va>>12]; if(!b){ wr16(va,v); return; } memcpy(b+(va&0xfff),&v,2); }
static EMU_AINL void fm_wr32(uint32_t va, uint32_t v){ uint8_t* b=g_pagemap[va>>12]; if(!b){ wr32(va,v); return; } memcpy(b+(va&0xfff),&v,4); }

#define rd8  fm_rd8
#define rd16 fm_rd16
#define rd32 fm_rd32
#define wr8  fm_wr8
#define wr16 fm_wr16
#define wr32 fm_wr32

#endif
