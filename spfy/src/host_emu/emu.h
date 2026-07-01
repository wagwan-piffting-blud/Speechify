// emu.h - shared types for the x86->WASM plugin emulator.
// Core (mem/cpu/loader) is reused verbatim from the AcuVoice Roger emulator;
// the host orchestration here targets VST2 audio plugins instead of a SAPI engine.
#ifndef EMU_H
#define EMU_H
#include <stdint.h>
#include <stddef.h>

// ---------------- guest memory (region-based VA translation) ----------------
#define MAX_REGIONS 32
typedef struct { uint32_t va; uint32_t size; uint8_t* host; const char* name; } region_t;
typedef struct { region_t regions[MAX_REGIONS]; int nreg; } mem_t;
extern mem_t MEM;

void     mem_init(void);
uint8_t* mem_map(uint32_t va, uint32_t size, const char* name);
uint8_t* mem_host(uint32_t va);
region_t* mem_region_of(uint32_t va);

uint8_t  rd8(uint32_t va);
uint16_t rd16(uint32_t va);
uint32_t rd32(uint32_t va);
void     wr8(uint32_t va, uint8_t v);
void     wr16(uint32_t va, uint16_t v);
void     wr32(uint32_t va, uint32_t v);
void     mem_read(uint32_t va, void* dst, uint32_t n);
void     mem_write(uint32_t va, const void* src, uint32_t n);

// ---------------- guest layout constants ----------------
#define IMAGE_BASE     0x10000000u
// Stack lives high so it never collides with the image: VST DLLs based at 0x10000000
// AND EXE-style plugins based at 0x00400000 both fit below it.
#define STACK_TOP      0xC0800000u
#define STACK_SIZE     0x00700000u
#define STACK_ESP0     (STACK_TOP - 0x00200000u)
#define HEAP_BASE      0x30000000u
#define HEAP_SIZE      0x08000000u   // 128 MB guest heap
#define VST_SCRATCH    0x50000000u   // audio I/O buffers + pointer arrays
#define VST_SCRATCH_SZ 0x01000000u   // 16 MB
#define VALLOC_BASE    0x40000000u   // VirtualAlloc arena (64KB-granular, like Win32)
#define VALLOC_SIZE    0x02000000u   // 32 MB (Delphi MM reserves a few MB of pools)
#define AUX_BASE       0x60000000u
#define TEB_BASE       0x7ffde000u
#define PEB_BASE       0x7ffdf000u
#define IMP_BASE       0x71000000u
#define IMP_STRIDE     16
#define MAX_IMPORTS    1024

// ---------------- CPU ----------------
enum { EAX,ECX,EDX,EBX,ESP,EBP,ESI,EDI };
typedef union { uint8_t b[16]; uint32_t u[4]; uint64_t q[2]; float f[4]; double d[2]; } xmreg_t;
typedef struct {
    uint32_t r[8];
    uint32_t eip;
    uint32_t eflags;
    uint32_t seg_fs_base;
    uint32_t seg_gs_base;
    double  st[8];
    int     fpu_top;
    uint16_t fpu_sw, fpu_cw;
    xmreg_t xmm[8];        // SSE/SSE2 registers
    uint32_t mxcsr;
    int      halted;
    uint32_t fault_addr; int faulted; const char* fault_msg;
} cpu_t;
extern cpu_t CPU;

#define FL_CF 0x0001
#define FL_PF 0x0004
#define FL_AF 0x0010
#define FL_ZF 0x0040
#define FL_SF 0x0080
#define FL_TF 0x0100
#define FL_IF 0x0200
#define FL_DF 0x0400
#define FL_OF 0x0800

void cpu_reset(void);
int  cpu_run(uint64_t max_insns);
void cpu_push32(uint32_t v);
uint32_t cpu_pop32(void);
#define RET_SENTINEL 0xdeadbeefu

// ---------------- PE loader ----------------
typedef struct {
    uint32_t image_base, size_of_image, entry_rva;
    uint32_t export_rva, export_size;
} pe_info_t;
extern pe_info_t PE;
int      pe_load(const char* path);
int      pe_load_mem(const uint8_t* bytes, uint32_t len);  // load from memory (WASM)
uint32_t pe_get_export(const char* name);
void     pe_run_dllmain(void);
void     pe_init_tls(void);                      // set up implicit TLS (TEB->ThreadLocalStoragePointer)

// ---------------- imports / Win32 shims ----------------
typedef void (*shim_fn)(void);
void     win32_init(void);
void     win32_reset(void);   // reset the import table (call before each pe_load)
int      win32_register_import(const char* dll, const char* name);
int      win32_is_import_va(uint32_t va);
void     win32_dispatch(uint32_t va);
uint32_t arg32(int i);          // stdcall/cdecl arg i (0-based): [ESP + 4 + 4*i]
void     ret_set(uint32_t eax);
extern int g_shim_cleanup;

// guest heap (exposed so the host can hand buffers to the guest)
uint32_t guest_alloc(uint32_t n, int zero);
void     guest_free(uint32_t p);

// a guest-callable host callback registered as a pseudo-import.
// argbytes is the stdcall cleanup (0 for cdecl).
uint32_t host_register_callback(shim_fn fn, int argbytes, const char* name);

// call a guest function (cdecl/stdcall) with n dword args; returns EAX.
uint32_t call_guest(uint32_t fn, const uint32_t* args, int n);

// logging
void emu_log(const char* fmt, ...);
extern int EMU_VERBOSE;

#endif
