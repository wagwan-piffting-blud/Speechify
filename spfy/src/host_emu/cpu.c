// cpu.c - 32-bit x86 interpreter (flat protected mode), native-first.
// Covers the common MSVC6 integer instruction set + string ops + a partial x87.
// Unknown opcodes trap with EIP + bytes so coverage can be extended iteratively.
#include "emu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

cpu_t CPU;

void win32_dispatch(uint32_t va);   // win32.c
int  win32_is_import_va(uint32_t va);
region_t* mem_region_of(uint32_t va);   // mem.c
void jit_reset(void);               // JIT cache flush (defined below; called from cpu_reset)

// Cached code region: virtually all execution stays inside the loaded image, so derive the
// instruction host pointer with register arithmetic (base + (eip-lo)) instead of a per-instruction
// page-map array load. Reset on each load (cpu_reset) so a re-load can't reuse a freed host base.
static uint8_t*  g_code_host = 0;
static uint32_t  g_code_lo   = 0;
static uint32_t  g_code_size = 0;

// ALU logic helpers (defined at end of file)
uint32_t do_logic_or (uint32_t a,uint32_t b,int sz);
uint32_t do_logic_and(uint32_t a,uint32_t b,int sz);
uint32_t do_logic_xor(uint32_t a,uint32_t b,int sz);

void cpu_reset(void){ memset(&CPU,0,sizeof CPU); CPU.eflags=0x202; CPU.fpu_cw=0x037f; CPU.fpu_top=8; CPU.mxcsr=0x1f80;
    g_code_host=0; g_code_lo=0; g_code_size=0; jit_reset(); }
// x87 FIST/FISTP/FRNDINT round per FPU control-word RC bits (10-11); default 00 = round-to-nearest-even
static double fpu_round_rc(double v){
    switch((CPU.fpu_cw>>10)&3){
        case 1:  return floor(v);  // toward -inf
        case 2:  return ceil(v);   // toward +inf
        case 3:  return trunc(v);  // toward zero
        default: return nearbyint(v); // 0: nearest (host default FE_TONEAREST = nearest-even)
    }
}

// JIT x87 trampolines: reuse the EXACT interpreter C (same emcc, same flags) so the JIT's FIST/FISTP and
// transcendentals match bit-for-bit by construction (incl. the float->int conversion's edge behavior and
// fpu_round_rc reading the live CPU.fpu_cw). Compiled blocks call_indirect these by their function-table
// index; jit_helpers (wasm_main.c) takes their addresses, which forces a table slot and returns the indices.
int32_t jit_f2i(double v){ return (int32_t)fpu_round_rc(v); }
double  jit_frndint(double v){ return fpu_round_rc(v); }
double  jit_f2xm1(double x){ return pow(2.0,x)-1.0; }
double  jit_fyl2x(double y,double x){ return y*(log(x)/log(2.0)); }
double  jit_ftan(double x){ return tan(x); }
double  jit_fpatan(double y,double x){ return atan2(y,x); }
double  jit_fsin(double x){ return sin(x); }
double  jit_fcos(double x){ return cos(x); }
double  jit_fscale(double a,double b){ return ldexp(a,(int)b); }
void cpu_push32(uint32_t v){ CPU.r[ESP]-=4; wr32(CPU.r[ESP], v); }
uint32_t cpu_pop32(void){ uint32_t v=rd32(CPU.r[ESP]); CPU.r[ESP]+=4; return v; }

// ---------------- fetch ----------------
// g_ip is a raw host pointer to the current instruction stream, set once per instruction
// (see cpu_run). Fetching advances both g_ip and CPU.eip - no per-byte page-map lookup.
// Safe because the code section is one contiguous host region; instructions don't span regions.
static const uint8_t* g_ip;
static uint8_t  fetch8(void){ uint8_t v=*g_ip; g_ip++; CPU.eip++; return v; }
static uint16_t fetch16(void){ uint16_t v; memcpy(&v,g_ip,2); g_ip+=2; CPU.eip+=2; return v; }
static uint32_t fetch32(void){ uint32_t v; memcpy(&v,g_ip,4); g_ip+=4; CPU.eip+=4; return v; }

// ---------------- flags ----------------
static const uint32_t SZMASK[5]={0,0xff,0xffff,0,0xffffffff};
static const uint32_t SIGN[5]  ={0,0x80,0x8000,0,0x80000000};
// 1 for each x86 legacy/REX-less prefix byte; lets the hot path reject the common
// no-prefix case with a single table lookup instead of a chain of 8+ comparisons.
static const uint8_t IS_PREFIX[256]={
    [0x66]=1,[0x67]=1,[0xF0]=1,[0xF2]=1,[0xF3]=1,
    [0x2E]=1,[0x36]=1,[0x3E]=1,[0x26]=1,[0x64]=1,[0x65]=1,
};
static int parity8(uint32_t v){ v&=0xff; v^=v>>4; v^=v>>2; v^=v>>1; return (~v)&1; }
static void set_szp(uint32_t res,int sz){
    uint32_t m=SZMASK[sz]; res&=m;
    CPU.eflags &= ~(FL_ZF|FL_SF|FL_PF);
    if(res==0) CPU.eflags|=FL_ZF;
    if(res & SIGN[sz]) CPU.eflags|=FL_SF;
    if(parity8(res)) CPU.eflags|=FL_PF;
}
static void setf(uint32_t bit,int on){ if(on) CPU.eflags|=bit; else CPU.eflags&=~bit; }

static uint32_t do_add(uint32_t a,uint32_t b,int sz){
    uint64_t r=(uint64_t)(a&SZMASK[sz])+(b&SZMASK[sz]);
    uint32_t res=(uint32_t)r; set_szp(res,sz);
    setf(FL_CF, (r>>(sz*8))&1);
    setf(FL_AF, ((a^b^res)&0x10)!=0);
    setf(FL_OF, ((~(a^b)&(a^res))&SIGN[sz])!=0);
    return res&SZMASK[sz];
}
static uint32_t do_adc(uint32_t a,uint32_t b,int sz){
    uint32_t c=(CPU.eflags&FL_CF)?1:0;
    uint64_t r=(uint64_t)(a&SZMASK[sz])+(b&SZMASK[sz])+c;
    uint32_t res=(uint32_t)r; set_szp(res,sz);
    setf(FL_CF, (r>>(sz*8))&1);
    setf(FL_AF, ((a^b^res)&0x10)!=0);
    setf(FL_OF, ((~(a^b)&(a^res))&SIGN[sz])!=0);
    return res&SZMASK[sz];
}
static uint32_t do_sub(uint32_t a,uint32_t b,int sz){
    uint32_t aa=a&SZMASK[sz], bb=b&SZMASK[sz];
    uint32_t res=(aa-bb)&SZMASK[sz]; set_szp(res,sz);
    setf(FL_CF, aa<bb);
    setf(FL_AF, ((a^b^res)&0x10)!=0);
    setf(FL_OF, (((a^b)&(a^res))&SIGN[sz])!=0);
    return res;
}
static uint32_t do_sbb(uint32_t a,uint32_t b,int sz){
    uint32_t c=(CPU.eflags&FL_CF)?1:0;
    uint32_t aa=a&SZMASK[sz]; uint64_t bb=(uint64_t)(b&SZMASK[sz])+c;
    uint32_t res=(uint32_t)((aa-bb))&SZMASK[sz]; set_szp(res,sz);
    setf(FL_CF, (uint64_t)aa < bb);
    setf(FL_AF, ((a^b^res)&0x10)!=0);
    setf(FL_OF, (((a^b)&(a^res))&SIGN[sz])!=0);
    return res;
}
static uint32_t do_logic(uint32_t res,int sz){ set_szp(res,sz); setf(FL_CF,0); setf(FL_OF,0); setf(FL_AF,0); return res&SZMASK[sz]; }
static uint32_t do_inc(uint32_t a,int sz){ uint32_t res=(a+1)&SZMASK[sz]; int cf=CPU.eflags&FL_CF; set_szp(res,sz); setf(FL_AF,((a^1^res)&0x10)!=0); setf(FL_OF,(a&SZMASK[sz])==(SIGN[sz]-1)); setf(FL_CF,cf); return res; }
static uint32_t do_dec(uint32_t a,int sz){ uint32_t res=(a-1)&SZMASK[sz]; int cf=CPU.eflags&FL_CF; set_szp(res,sz); setf(FL_AF,((a^1^res)&0x10)!=0); setf(FL_OF,(a&SZMASK[sz])==SIGN[sz]); setf(FL_CF,cf); return res; }

// ---------------- register file access by encoded index+size ----------------
static uint32_t getreg(int idx,int sz){
    if(sz==4) return CPU.r[idx];                                 // common 32-bit path first
    if(sz==2) return CPU.r[idx]&0xffff;
    if(idx<4) return CPU.r[idx]&0xff; return (CPU.r[idx-4]>>8)&0xff;
}
static void setreg(int idx,int sz,uint32_t v){
    if(sz==4){ CPU.r[idx]=v; return; }                          // common 32-bit path first
    if(sz==2){ CPU.r[idx]=(CPU.r[idx]&~0xffffu)|(v&0xffff); return; }
    if(idx<4) CPU.r[idx]=(CPU.r[idx]&~0xffu)|(v&0xff); else CPU.r[idx-4]=(CPU.r[idx-4]&~0xff00u)|((v&0xff)<<8);
}

// ---------------- ModRM ----------------
typedef struct { int mod,reg,rm; int is_mem; uint32_t addr; } modrm_t;
static uint32_t g_seg_base; // current effective segment base (for FS/GS overrides)

static void decode_modrm(modrm_t* m,int addr_override){
    (void)addr_override;
    uint8_t b=fetch8();
    m->mod=b>>6; m->reg=(b>>3)&7; m->rm=b&7;
    m->is_mem=(m->mod!=3);
    if(!m->is_mem){ m->addr=0; return; }
    uint32_t addr=0;
    if(m->rm==4){ // SIB
        uint8_t sib=fetch8();
        int scale=sib>>6, index=(sib>>3)&7, base=sib&7;
        uint32_t idxval=(index==4)?0:CPU.r[index];
        uint32_t baseval;
        if(base==5 && m->mod==0){ baseval=fetch32(); }
        else baseval=CPU.r[base];
        addr=baseval + (idxval<<scale);
    } else if(m->rm==5 && m->mod==0){ addr=fetch32(); }
    else addr=CPU.r[m->rm];
    if(m->mod==1) addr+=(int8_t)fetch8();
    else if(m->mod==2) addr+=(int32_t)fetch32();
    m->addr = addr + g_seg_base;
}
static uint32_t rm_read(modrm_t* m,int sz){ return m->is_mem ? (sz==1?rd8(m->addr):sz==2?rd16(m->addr):rd32(m->addr)) : getreg(m->rm,sz); }
static void rm_write(modrm_t* m,int sz,uint32_t v){ if(m->is_mem){ if(sz==1)wr8(m->addr,v);else if(sz==2)wr16(m->addr,v);else wr32(m->addr,v);} else setreg(m->rm,sz,v); }

// ---------------- conditions ----------------
static int cond(int c){
    int cf=!!(CPU.eflags&FL_CF), zf=!!(CPU.eflags&FL_ZF), sf=!!(CPU.eflags&FL_SF), of=!!(CPU.eflags&FL_OF), pf=!!(CPU.eflags&FL_PF);
    switch(c&0xf){
        case 0x0:return of; case 0x1:return !of; case 0x2:return cf; case 0x3:return !cf;
        case 0x4:return zf; case 0x5:return !zf; case 0x6:return cf||zf; case 0x7:return !(cf||zf);
        case 0x8:return sf; case 0x9:return !sf; case 0xa:return pf; case 0xb:return !pf;
        case 0xc:return sf!=of; case 0xd:return sf==of; case 0xe:return zf||(sf!=of); case 0xf:return !(zf||(sf!=of));
    } return 0;
}

// ---------------- shifts/rotates ----------------
static uint32_t do_shift(int op,uint32_t v,uint32_t cnt,int sz){
    uint32_t m=SZMASK[sz]; v&=m; cnt &= 31; if(sz<4) {} // (count masked to 5 bits on 386+)
    if(cnt==0) return v;
    uint32_t res=v; int cf=0,of=0;
    switch(op){
        case 4: case 6: // SHL/SAL
            for(uint32_t i=0;i<cnt;i++){ cf=(res&SIGN[sz])?1:0; res=(res<<1)&m; }
            of=cf ^ ((res&SIGN[sz])?1:0); break;
        case 5: // SHR
            for(uint32_t i=0;i<cnt;i++){ cf=res&1; res>>=1; }
            of=(v&SIGN[sz])?1:0; break;
        case 7: // SAR
            { int neg=(v&SIGN[sz])?1:0;
              for(uint32_t i=0;i<cnt;i++){ cf=res&1; res>>=1; if(neg) res|=SIGN[sz]; }
              of=0; } break;
        case 0: // ROL
            for(uint32_t i=0;i<cnt;i++){ cf=(res&SIGN[sz])?1:0; res=((res<<1)|cf)&m; }
            of=cf ^ ((res&SIGN[sz])?1:0); break;
        case 1: // ROR
            for(uint32_t i=0;i<cnt;i++){ cf=res&1; res=((res>>1)|((uint32_t)cf*SIGN[sz]))&m; }
            of=((res&SIGN[sz])?1:0) ^ (((res<<1)&SIGN[sz])?1:0); break;
        case 2: // RCL
            for(uint32_t i=0;i<cnt;i++){ int nc=(res&SIGN[sz])?1:0; res=((res<<1)|(CPU.eflags&FL_CF?1:0))&m; setf(FL_CF,nc); cf=nc; }
            of=cf ^ ((res&SIGN[sz])?1:0); break;
        case 3: // RCR
            for(uint32_t i=0;i<cnt;i++){ int nc=res&1; res=(res>>1)|((CPU.eflags&FL_CF?1u:0u)*SIGN[sz]); res&=m; setf(FL_CF,nc); cf=nc; }
            break;
    }
    if(op>=4) set_szp(res,sz);   // shifts affect SZP; rotates (0..3) do not
    if(op<2||op>3) setf(FL_CF,cf);  // RCL/RCR already set CF inside the loop
    setf(FL_OF,of);
    return res&m;
}

// ---------------- x87 (partial) ----------------
static double* st(int i){ return &CPU.st[(CPU.fpu_top+i)&7]; }
static void fpush(double v){ CPU.fpu_top=(CPU.fpu_top-1)&7; CPU.st[CPU.fpu_top]=v; }
static double fpop(void){ double v=CPU.st[CPU.fpu_top]; CPU.fpu_top=(CPU.fpu_top+1)&7; return v; }

static void set_fcom(double a,double b){
    CPU.fpu_sw &= ~((1<<8)|(1<<10)|(1<<14)); // clear C0,C2,C3
    if(a!=a||b!=b) CPU.fpu_sw |= (1<<8)|(1<<10)|(1<<14); // unordered (NaN)
    else if(a<b)  CPU.fpu_sw |= (1<<8);   // C0
    else if(a==b) CPU.fpu_sw |= (1<<14);  // C3
    // a>b: all clear
}
static void set_fcomi(double a,double b){ // sets EFLAGS directly (FCOMI/FUCOMI)
    setf(FL_OF,0); setf(FL_SF,0); setf(FL_AF,0);
    if(a!=a||b!=b){ setf(FL_ZF,1); setf(FL_PF,1); setf(FL_CF,1); }
    else if(a<b){ setf(FL_ZF,0); setf(FL_PF,0); setf(FL_CF,1); }
    else if(a==b){ setf(FL_ZF,1); setf(FL_PF,0); setf(FL_CF,0); }
    else { setf(FL_ZF,0); setf(FL_PF,0); setf(FL_CF,0); }
}
static uint16_t fpu_status(void){ return (CPU.fpu_sw & ~0x3800) | (((uint16_t)CPU.fpu_top&7)<<11); }

static int do_x87(uint8_t op){
    modrm_t m; decode_modrm(&m,0);
    int reg=m.reg;
    if(m.is_mem){
        uint32_t a=m.addr;
        switch(op){
            case 0xD9: // FLD/FST/FSTP m32, FLDCW, FNSTCW, FLDENV/FSTENV
                if(reg==0){ uint32_t b=rd32(a); float f; memcpy(&f,&b,4); fpush(f); return 1; }
                if(reg==2){ float f=(float)*st(0); uint32_t b; memcpy(&b,&f,4); wr32(a,b); return 1; }
                if(reg==3){ float f=(float)*st(0); uint32_t b; memcpy(&b,&f,4); wr32(a,b); fpop(); return 1; }
                if(reg==5){ CPU.fpu_cw=rd16(a); if(EMU_VERBOSE) emu_log("[FLDCW] cw=0x%04x (PC=%d)\n",CPU.fpu_cw,(CPU.fpu_cw>>8)&3); return 1; }   // FLDCW
                if(reg==7){ wr16(a,CPU.fpu_cw); return 1; }   // FNSTCW
                if(reg==4||reg==6){ return 1; }               // FLDENV/FNSTENV (ignore)
                break;
            case 0xDD: // FLD/FST/FSTP m64, FRSTOR/FNSAVE, FNSTSW m16
                if(reg==0){ uint64_t b=rd32(a)|((uint64_t)rd32(a+4)<<32); double d; memcpy(&d,&b,8); fpush(d); return 1; }
                if(reg==2||reg==3){ double d=*st(0); uint64_t b; memcpy(&b,&d,8); wr32(a,(uint32_t)b); wr32(a+4,(uint32_t)(b>>32)); if(reg==3)fpop(); return 1; }
                if(reg==7){ wr16(a,fpu_status()); return 1; } // FNSTSW m16
                if(reg==4||reg==6){ return 1; }
                break;
            case 0xDB: // FILD/FIST/FISTP m32, FLD/FSTP m80
                if(reg==0){ int32_t v=(int32_t)rd32(a); fpush((double)v); return 1; }
                if(reg==2){ int32_t v=(int32_t)fpu_round_rc(*st(0)); wr32(a,(uint32_t)v); return 1; }
                if(reg==3){ int32_t v=(int32_t)fpu_round_rc(*st(0)); wr32(a,(uint32_t)v); fpop(); return 1; }
                if(reg==5){ /* FLD m80 -> approx via low 64 mantissa is hard; load as double from 80-bit */
                    uint64_t mant=rd32(a)|((uint64_t)rd32(a+4)<<32); uint16_t se=rd16(a+8);
                    int sign=(se>>15)&1; int exp=(se&0x7fff); double val;
                    if(exp==0&&mant==0) val=0; else { val=(double)mant*pow(2.0,(double)(exp-16383-63)); } if(sign)val=-val; fpush(val); return 1; }
                if(reg==7){ /* FSTP m80 */ double d=*st(0); uint64_t mant; uint16_t se; int sign=d<0; if(sign)d=-d;
                    if(d==0){mant=0;se=0;} else { int e; double mf=frexp(d,&e); mant=(uint64_t)ldexp(mf,64); se=(uint16_t)(e-1+16383); } if(sign)se|=0x8000;
                    wr32(a,(uint32_t)mant); wr32(a+4,(uint32_t)(mant>>32)); wr16(a+8,se); fpop(); return 1; }
                break;
            case 0xDF: // FILD/FIST/FISTP m16, FILD/FISTP m64, FBLD/FBSTP m80
                if(reg==0){ int16_t v=(int16_t)rd16(a); fpush((double)v); return 1; }
                if(reg==2){ wr16(a,(uint16_t)(int16_t)fpu_round_rc(*st(0))); return 1; }
                if(reg==3){ wr16(a,(uint16_t)(int16_t)fpu_round_rc(*st(0))); fpop(); return 1; }
                if(reg==4){ // FBLD m80: load 18-digit packed BCD integer
                    unsigned long long acc=0; for(int k=8;k>=0;k--){ uint8_t by=rd8(a+k); acc=acc*100+(by>>4)*10+(by&0xf); }
                    double dv=(double)acc; if(rd8(a+9)&0x80) dv=-dv; fpush(dv); return 1; }
                if(reg==5){ int64_t v=(int64_t)(rd32(a)|((uint64_t)rd32(a+4)<<32)); fpush((double)v); return 1; }
                if(reg==6){ // FBSTP m80: round ST(0) to integer, store as 18-digit packed BCD + sign, pop
                    double v=*st(0); int neg=v<0; double mag=neg?-v:v;
                    unsigned long long acc=(mag>=1.8e19)?0xFFFFFFFFFFFFFFFFULL:(unsigned long long)(mag+0.5);
                    for(int k=0;k<9;k++){ wr8(a+k,(uint8_t)((acc%10)|(((acc/10)%10)<<4))); acc/=100; }
                    wr8(a+9, neg?0x80:0x00); fpop(); return 1; }
                if(reg==7){ int64_t v=(int64_t)fpu_round_rc(*st(0)); wr32(a,(uint32_t)v); wr32(a+4,(uint32_t)((uint64_t)v>>32)); fpop(); return 1; }
                break;
            case 0xD8: case 0xDC: { // float arith with m32 (D8) / m64 (DC)
                double b; if(op==0xD8){ uint32_t bb=rd32(a); float f; memcpy(&f,&bb,4); b=f; }
                          else { uint64_t bb=rd32(a)|((uint64_t)rd32(a+4)<<32); memcpy(&b,&bb,8); }
                switch(reg){
                    case 0:*st(0)+=b;return 1; case 1:*st(0)*=b;return 1;
                    case 2:set_fcom(*st(0),b);return 1; case 3:set_fcom(*st(0),b);fpop();return 1;
                    case 4:*st(0)-=b;return 1; case 5:*st(0)=b-*st(0);return 1;
                    case 6:*st(0)/=b;return 1; case 7:*st(0)=b/ *st(0);return 1;
                } break; }
            case 0xDA: { // int arith with m32
                double b=(double)(int32_t)rd32(a);
                switch(reg){ case 0:*st(0)+=b;return 1;case 1:*st(0)*=b;return 1;
                    case 2:set_fcom(*st(0),b);return 1;case 3:set_fcom(*st(0),b);fpop();return 1;
                    case 4:*st(0)-=b;return 1;case 5:*st(0)=b-*st(0);return 1;
                    case 6:*st(0)/=b;return 1;case 7:*st(0)=b/ *st(0);return 1; } break; }
        }
        return 0;
    } else {
        int i=m.rm; uint8_t full=0xC0|(m.reg<<3)|i;
        switch(op){
            case 0xD8: // st(0) op st(i)
                switch(m.reg){case 0:*st(0)+=*st(i);return 1;case 1:*st(0)*=*st(i);return 1;
                    case 2:set_fcom(*st(0),*st(i));return 1;case 3:set_fcom(*st(0),*st(i));fpop();return 1;
                    case 4:*st(0)-=*st(i);return 1;case 5:*st(0)=*st(i)-*st(0);return 1;
                    case 6:*st(0)/=*st(i);return 1;case 7:*st(0)=*st(i)/ *st(0);return 1;} break;
            case 0xDC: // dest=st(i): DC /4=FSUBR(st0-sti) /5=FSUB(sti-st0) /6=FDIVR(st0/sti) /7=FDIV(sti/st0)
                // Same reversal as the DE pop-forms below (NOT like the DC *memory* form, where dest=st0).
                // Was swapped: corrupted exp()'s f2xm1 fraction -> wrong env coeffs -> sc4 gated. (capstone: dc e1 = fsubr st(1),st(0))
                switch(m.reg){case 0:*st(i)+=*st(0);return 1;case 1:*st(i)*=*st(0);return 1;
                    case 2:set_fcom(*st(0),*st(i));return 1;case 3:set_fcom(*st(0),*st(i));return 1;
                    case 4:*st(i)=*st(0)-*st(i);return 1;case 5:*st(i)-=*st(0);return 1;
                    case 6:*st(i)=*st(0)/ *st(i);return 1;case 7:*st(i)/=*st(0);return 1;} break;
            case 0xDE: // ...P (pop) ; DE D9 = FCOMPP
                if(full==0xD9){ set_fcom(*st(0),*st(1)); fpop(); fpop(); return 1; }
                switch(m.reg){case 0:*st(i)+=*st(0);fpop();return 1;case 1:*st(i)*=*st(0);fpop();return 1;
                    case 2:set_fcom(*st(0),*st(i));fpop();return 1;case 3:set_fcom(*st(0),*st(i));fpop();fpop();return 1;
                    case 4:*st(i)=*st(0)-*st(i);fpop();return 1;case 5:*st(i)-=*st(0);fpop();return 1;
                    case 6:*st(i)=*st(0)/ *st(i);fpop();return 1;case 7:*st(i)/=*st(0);fpop();return 1;} break;
            case 0xD9:
                if(m.reg==0){ double v=*st(i); fpush(v); return 1; }      // FLD st(i)
                if(m.reg==1){ double t=*st(0); *st(0)=*st(i); *st(i)=t; return 1; } // FXCH
                if(m.reg==2||m.reg==3){ return 1; }                      // FNOP/FST st (no-op-ish)
                switch(full){
                    case 0xE0:*st(0)=-*st(0);return 1;   // FCHS
                    case 0xE1:if(*st(0)<0)*st(0)=-*st(0);return 1; // FABS
                    case 0xE4:set_fcom(*st(0),0.0);return 1; // FTST
                    case 0xE5:{ // FXAM: classify ST(0) -> C3,C2,C1,C0 (MSVC exp/pow dispatch on this)
                        double v=*st(0); uint64_t bits; memcpy(&bits,&v,8);
                        CPU.fpu_sw &= ~((1u<<8)|(1u<<9)|(1u<<10)|(1u<<14)); // clear C0,C1,C2,C3
                        if(bits>>63) CPU.fpu_sw |= (1u<<9);                       // C1 = sign
                        if(v!=v)            CPU.fpu_sw |= (1u<<8);                 // NaN:      C0
                        else if(v==0.0)     CPU.fpu_sw |= (1u<<14);               // Zero:     C3
                        else { int e=(int)((bits>>52)&0x7ff);
                            if(e==0x7ff)    CPU.fpu_sw |= (1u<<8)|(1u<<10);        // Infinity: C0,C2
                            else if(e==0)   CPU.fpu_sw |= (1u<<10)|(1u<<14);       // Denormal: C2,C3
                            else            CPU.fpu_sw |= (1u<<10);               // Normal:   C2
                        }
                        return 1; }
                    case 0xE8:fpush(1.0);return 1;       // FLD1
                    case 0xE9:fpush(3.321928094887362);return 1; // FLDL2T
                    case 0xEA:fpush(1.442695040888963);return 1; // FLDL2E
                    case 0xEB:fpush(3.141592653589793);return 1; // FLDPI
                    case 0xEC:fpush(0.301029995663981);return 1; // FLDLG2
                    case 0xED:fpush(0.693147180559945);return 1; // FLDLN2
                    case 0xEE:fpush(0.0);return 1;       // FLDZ
                    case 0xF0:*st(0)=pow(2.0,*st(0))-1.0;return 1; // F2XM1
                    case 0xF1:{double y=*st(1),x=*st(0); fpop(); *st(0)=y*(log(x)/log(2.0)); return 1;} // FYL2X
                    case 0xF2:*st(0)=tan(*st(0));fpush(1.0);return 1; // FPTAN
                    case 0xF3:{double y=*st(1),x=*st(0); fpop(); *st(0)=atan2(y,x); return 1;} // FPATAN
                    case 0xFA:*st(0)=sqrt(*st(0));return 1; // FSQRT
                    case 0xFC:*st(0)=fpu_round_rc(*st(0));return 1; // FRNDINT (per RC)
                    case 0xFD:*st(0)=ldexp(*st(0),(int)*st(1));return 1; // FSCALE: ST0 = ST0 * 2^trunc(ST1), no pop
                    case 0xFE:*st(0)=sin(*st(0));return 1; // FSIN
                    case 0xFF:*st(0)=cos(*st(0));return 1; // FCOS
                    case 0xD0:return 1;                  // FNOP
                }
                if(m.reg==4||m.reg==5||m.reg==6||m.reg==7) return 1; // FLDENV etc reg-form (rare)
                break;
            case 0xDD:
                if(m.reg==0){ return 1; }                  // FFREE
                if(m.reg==2){ *st(i)=*st(0); return 1; }   // FST st(i)
                if(m.reg==3){ *st(i)=*st(0); fpop(); return 1; } // FSTP st(i)
                if(m.reg==4){ set_fcom(*st(0),*st(i)); return 1; } // FUCOM
                if(m.reg==5){ set_fcom(*st(0),*st(i)); fpop(); return 1; } // FUCOMP
                break;
            case 0xDA:
                if(full==0xE9){ set_fcom(*st(0),*st(1)); fpop(); fpop(); return 1; } // FUCOMPP
                // DA C0..DF = FCMOVcc (rare) -> ignore conditionally
                return 1;
            case 0xDB:
                if(full==0xE2){ CPU.fpu_sw=0; return 1; }  // FNCLEX
                if(full==0xE3){ CPU.fpu_top=0; CPU.fpu_sw=0; CPU.fpu_cw=0x037f; return 1; } // FNINIT
                if(full==0xE0||full==0xE1||full==0xE4){ return 1; } // FNENI/FNDISI/FNSETPM
                if(m.reg==5){ set_fcomi(*st(0),*st(i)); return 1; }   // FUCOMI
                if(m.reg==6){ set_fcomi(*st(0),*st(i)); return 1; }   // FCOMI
                break;
            case 0xDF:
                if(full==0xE0){ setreg(EAX,2,fpu_status()); return 1; } // FNSTSW AX
                if(m.reg==5){ set_fcomi(*st(0),*st(i)); fpop(); return 1; } // FUCOMIP
                if(m.reg==6){ set_fcomi(*st(0),*st(i)); fpop(); return 1; } // FCOMIP
                break;
        }
        return 0;
    }
}

// ---------------- SSE / SSE2 ----------------
// prefix p: 0 = none (packed single / "PS"), 1 = 66 (packed double / integer),
//           2 = F3 (scalar single / "SS"),   3 = F2 (scalar double / "SD")
static void sse_rm_read(modrm_t* m, void* dst, int nbytes){
    if(m->is_mem){ uint8_t* h=mem_host(m->addr); if(h) memcpy(dst,h,nbytes); else mem_read(m->addr,dst,nbytes); }
    else memcpy(dst, &CPU.xmm[m->rm], nbytes);
}
static void sse_rm_write(modrm_t* m, const void* src, int nbytes){
    if(m->is_mem){ uint8_t* h=mem_host(m->addr); if(h) memcpy(h,src,nbytes); else mem_write(m->addr,src,nbytes); }
    else memcpy(&CPU.xmm[m->rm], src, nbytes);
}
static double sse_arith(uint8_t op,double a,double b){
    switch(op){ case 0x58:return a+b; case 0x59:return a*b; case 0x5C:return a-b; case 0x5E:return a/b;
        case 0x5D:return (a<b||b!=b)?a:b; /*MIN*/ case 0x5F:return (a>b||b!=b)?a:b; /*MAX*/
        case 0x51:return sqrt(b); case 0x53:return 1.0/b; case 0x52:return 1.0/sqrt(b); }
    return 0;
}
static void sse_setcmp(double a,double b){ // COMISS/UCOMISS -> EFLAGS
    setf(FL_OF,0); setf(FL_SF,0); setf(FL_AF,0);
    if(a!=a||b!=b){ setf(FL_ZF,1); setf(FL_PF,1); setf(FL_CF,1); }
    else if(a<b){ setf(FL_ZF,0); setf(FL_PF,0); setf(FL_CF,1); }
    else if(a==b){ setf(FL_ZF,1); setf(FL_PF,0); setf(FL_CF,0); }
    else { setf(FL_ZF,0); setf(FL_PF,0); setf(FL_CF,0); }
}

static int do_sse(uint8_t op2,int p){
    modrm_t m;
    switch(op2){
    case 0x10: case 0x11: { // MOV UPS/SS/UPD/SD
        decode_modrm(&m,0); xmreg_t* d=&CPU.xmm[m.reg];
        if(op2==0x10){ // load -> xmm[reg]
            if(p==2){ uint32_t v; if(m.is_mem){ v=rd32(m.addr); d->u[1]=d->u[2]=d->u[3]=0; } else v=CPU.xmm[m.rm].u[0]; d->u[0]=v; }
            else if(p==3){ uint64_t v; if(m.is_mem){ v=rd32(m.addr)|((uint64_t)rd32(m.addr+4)<<32); d->q[1]=0; } else v=CPU.xmm[m.rm].q[0]; d->q[0]=v; }
            else sse_rm_read(&m,d,16);
        } else { // store xmm[reg] -> r/m
            if(p==2){ if(m.is_mem) wr32(m.addr,d->u[0]); else CPU.xmm[m.rm].u[0]=d->u[0]; }
            else if(p==3){ if(m.is_mem){ wr32(m.addr,d->u[0]); wr32(m.addr+4,d->u[1]); } else CPU.xmm[m.rm].q[0]=d->q[0]; }
            else sse_rm_write(&m,d,16);
        } return 1; }
    case 0x12: case 0x16: { // MOVLPS/MOVHLPS (12), MOVHPS/MOVLHPS (16)
        decode_modrm(&m,0); xmreg_t* d=&CPU.xmm[m.reg];
        int hi=(op2==0x16);
        if(m.is_mem){ uint64_t v=rd32(m.addr)|((uint64_t)rd32(m.addr+4)<<32); d->q[hi?1:0]=v; }
        else { d->q[hi?1:0] = hi ? CPU.xmm[m.rm].q[0] : CPU.xmm[m.rm].q[1]; } // MOVLHPS / MOVHLPS
        return 1; }
    case 0x13: case 0x17: { // MOVLPS/MOVHPS store m64
        decode_modrm(&m,0); xmreg_t* d=&CPU.xmm[m.reg]; int hi=(op2==0x17);
        if(m.is_mem){ wr32(m.addr,(uint32_t)d->q[hi?1:0]); wr32(m.addr+4,(uint32_t)(d->q[hi?1:0]>>32)); } return 1; }
    case 0x14: case 0x15: { // UNPCKLPS / UNPCKHPS
        decode_modrm(&m,0); xmreg_t s; sse_rm_read(&m,&s,16); xmreg_t* d=&CPU.xmm[m.reg]; xmreg_t r;
        if(p==1){ // PD
            if(op2==0x14){ r.q[0]=d->q[0]; r.q[1]=s.q[0]; } else { r.q[0]=d->q[1]; r.q[1]=s.q[1]; }
        } else {
            if(op2==0x14){ r.u[0]=d->u[0]; r.u[1]=s.u[0]; r.u[2]=d->u[1]; r.u[3]=s.u[1]; }
            else { r.u[0]=d->u[2]; r.u[1]=s.u[2]; r.u[2]=d->u[3]; r.u[3]=s.u[3]; }
        } *d=r; return 1; }
    case 0x28: case 0x29: { // MOVAPS/MOVAPD
        decode_modrm(&m,0);
        if(op2==0x28) sse_rm_read(&m,&CPU.xmm[m.reg],16); else sse_rm_write(&m,&CPU.xmm[m.reg],16);
        return 1; }
    case 0x2B: { decode_modrm(&m,0); sse_rm_write(&m,&CPU.xmm[m.reg],16); return 1; } // MOVNTPS store
    case 0x2A: { // CVTSI2SS/SD (src = int r/m32)
        decode_modrm(&m,0); int32_t v = m.is_mem ? (int32_t)rd32(m.addr) : (int32_t)getreg(m.rm,4);
        if(p==3) CPU.xmm[m.reg].d[0]=(double)v; else CPU.xmm[m.reg].f[0]=(float)v; return 1; }
    case 0x2C: case 0x2D: { // CVT(T)SS2SI / SD2SI (dst = GP reg)
        decode_modrm(&m,0); double val; xmreg_t s;
        if(p==3){ sse_rm_read(&m,&s,8); val=s.d[0]; } else { sse_rm_read(&m,&s,4); val=s.f[0]; }
        int32_t r = (op2==0x2C)?(int32_t)val:(int32_t)nearbyint(val);
        setreg(m.reg,4,(uint32_t)r); return 1; }
    case 0x2E: case 0x2F: { // UCOMISS/COMISS (+SD with 66)
        decode_modrm(&m,0); xmreg_t s; double a,b;
        if(p==1){ sse_rm_read(&m,&s,8); a=CPU.xmm[m.reg].d[0]; b=s.d[0]; }
        else { sse_rm_read(&m,&s,4); a=CPU.xmm[m.reg].f[0]; b=s.f[0]; }
        sse_setcmp(a,b); return 1; }
    case 0x50: { // MOVMSKPS / MOVMSKPD -> GP reg = sign bits
        decode_modrm(&m,0); xmreg_t* x=&CPU.xmm[m.rm]; uint32_t r;
        if(p==1) r=((x->q[1]>>63)<<1)|(x->q[0]>>63);
        else r=((x->u[3]>>31)<<3)|((x->u[2]>>31)<<2)|((x->u[1]>>31)<<1)|(x->u[0]>>31);
        setreg(m.reg,4,r); return 1; }
    case 0x51: case 0x52: case 0x53: case 0x58: case 0x59: case 0x5C: case 0x5D: case 0x5E: case 0x5F: {
        // SQRT/RSQRT/RCP/ADD/MUL/SUB/MIN/DIV/MAX
        decode_modrm(&m,0); xmreg_t s; xmreg_t* d=&CPU.xmm[m.reg];
        if(p==2){ sse_rm_read(&m,&s,4); d->f[0]=(float)sse_arith(op2,d->f[0],s.f[0]); }
        else if(p==3){ sse_rm_read(&m,&s,8); d->d[0]=sse_arith(op2,d->d[0],s.d[0]); }
        else if(p==1){ sse_rm_read(&m,&s,16); for(int i=0;i<2;i++) d->d[i]=sse_arith(op2,d->d[i],s.d[i]); }
        else { sse_rm_read(&m,&s,16); for(int i=0;i<4;i++) d->f[i]=(float)sse_arith(op2,d->f[i],s.f[i]); }
        return 1; }
    case 0x54: case 0x55: case 0x56: case 0x57: { // ANDPS/ANDNPS/ORPS/XORPS (bitwise, 128-bit)
        decode_modrm(&m,0); xmreg_t s; sse_rm_read(&m,&s,16); xmreg_t* d=&CPU.xmm[m.reg];
        for(int i=0;i<4;i++){ switch(op2){
            case 0x54: d->u[i]&=s.u[i]; break; case 0x55: d->u[i]=(~d->u[i])&s.u[i]; break;
            case 0x56: d->u[i]|=s.u[i]; break; case 0x57: d->u[i]^=s.u[i]; break; } }
        return 1; }
    case 0x5A: { // CVTSS2SD(F3)/CVTSD2SS(F2)/CVTPS2PD(none)/CVTPD2PS(66)
        decode_modrm(&m,0); xmreg_t s; xmreg_t* d=&CPU.xmm[m.reg];
        if(p==2){ sse_rm_read(&m,&s,4); d->d[0]=(double)s.f[0]; }
        else if(p==3){ sse_rm_read(&m,&s,8); d->f[0]=(float)s.d[0]; }
        else if(p==1){ sse_rm_read(&m,&s,16); float a=(float)s.d[0],b=(float)s.d[1]; d->f[0]=a; d->f[1]=b; d->q[1]=0; }
        else { sse_rm_read(&m,&s,8); double a=(double)s.f[0],b=(double)s.f[1]; d->d[0]=a; d->d[1]=b; }
        return 1; }
    case 0x5B: { // CVTDQ2PS(none)/CVTPS2DQ(66)/CVTTPS2DQ(F3)
        decode_modrm(&m,0); xmreg_t s; sse_rm_read(&m,&s,16); xmreg_t* d=&CPU.xmm[m.reg];
        if(p==0){ for(int i=0;i<4;i++) d->f[i]=(float)(int32_t)s.u[i]; }
        else { for(int i=0;i<4;i++){ double v=s.f[i]; d->u[i]=(uint32_t)(int32_t)((p==2)?(double)(int32_t)v:nearbyint(v)); } }
        return 1; }
    case 0x71: case 0x72: case 0x73: { // packed shift by imm8 (PSRLW/D/Q, PSLLW/D/Q, PSRAW/D, PSRLDQ/PSLLDQ)
        decode_modrm(&m,0); uint8_t imm=fetch8(); xmreg_t* d=&CPU.xmm[m.rm]; int sub=m.reg;
        if(op2==0x73){
            if(sub==2){ for(int i=0;i<2;i++) d->q[i]= imm>=64?0:(d->q[i]>>imm); }       // PSRLQ
            else if(sub==6){ for(int i=0;i<2;i++) d->q[i]= imm>=64?0:(d->q[i]<<imm); }  // PSLLQ
            else if(sub==3){ xmreg_t r; memset(&r,0,16); for(int i=0;i+imm<16;i++) r.b[i]=d->b[i+imm]; *d=r; } // PSRLDQ (bytes)
            else if(sub==7){ xmreg_t r; memset(&r,0,16); for(int i=15;i-imm>=0;i--) r.b[i]=d->b[i-imm]; *d=r; } // PSLLDQ
        } else if(op2==0x72){ for(int i=0;i<4;i++){
            if(sub==2) d->u[i]= imm>=32?0:(d->u[i]>>imm);
            else if(sub==6) d->u[i]= imm>=32?0:(d->u[i]<<imm);
            else if(sub==4) d->u[i]= (uint32_t)((int32_t)d->u[i] >> (imm>=32?31:imm)); }
        } else { for(int i=0;i<8;i++){ uint16_t w=d->b[2*i]|((uint16_t)d->b[2*i+1]<<8);
            if(sub==2) w= imm>=16?0:(w>>imm); else if(sub==6) w= imm>=16?0:(w<<imm);
            else if(sub==4) w=(uint16_t)((int16_t)w >> (imm>=16?15:imm)); d->b[2*i]=w&0xff; d->b[2*i+1]=w>>8; }
        }
        return 1; }
    case 0xE6: { // CVTDQ2PD(F3) / CVTPD2DQ(F2) / CVTTPD2DQ(66)
        decode_modrm(&m,0); xmreg_t s; xmreg_t* d=&CPU.xmm[m.reg];
        if(p==2){ sse_rm_read(&m,&s,8); double a=(double)(int32_t)s.u[0],b=(double)(int32_t)s.u[1]; d->d[0]=a; d->d[1]=b; }
        else { sse_rm_read(&m,&s,16); int32_t a=(int32_t)((p==1)?s.d[0]:nearbyint(s.d[0])), b=(int32_t)((p==1)?s.d[1]:nearbyint(s.d[1])); d->u[0]=a; d->u[1]=b; d->q[1]=0; }
        return 1; }
    case 0x6E: { // MOVD xmm, r/m32
        decode_modrm(&m,0); xmreg_t* d=&CPU.xmm[m.reg]; d->q[0]=0; d->q[1]=0;
        d->u[0] = m.is_mem ? rd32(m.addr) : getreg(m.rm,4); return 1; }
    case 0x7E: { // F3: MOVQ xmm,xmm/m64 ; 66: MOVD r/m32,xmm
        decode_modrm(&m,0);
        if(p==2){ xmreg_t* d=&CPU.xmm[m.reg]; d->q[1]=0; if(m.is_mem) d->q[0]=rd32(m.addr)|((uint64_t)rd32(m.addr+4)<<32); else d->q[0]=CPU.xmm[m.rm].q[0]; }
        else { uint32_t v=CPU.xmm[m.reg].u[0]; if(m.is_mem) wr32(m.addr,v); else setreg(m.rm,4,v); }
        return 1; }
    case 0x6F: { decode_modrm(&m,0); sse_rm_read(&m,&CPU.xmm[m.reg],16); return 1; } // MOVDQA/MOVDQU load
    case 0x7F: { decode_modrm(&m,0); sse_rm_write(&m,&CPU.xmm[m.reg],16); return 1; } // store
    case 0xD6: { decode_modrm(&m,0); xmreg_t* d=&CPU.xmm[m.reg]; // MOVQ xmm/m64, xmm
        if(m.is_mem){ wr32(m.addr,(uint32_t)d->q[0]); wr32(m.addr+4,(uint32_t)(d->q[0]>>32)); } else { CPU.xmm[m.rm].q[0]=d->q[0]; CPU.xmm[m.rm].q[1]=0; } return 1; }
    case 0xFC: case 0xFD: case 0xFE: case 0xD4: case 0xF8: case 0xF9: case 0xFA: case 0xFB: {
        // packed integer add/sub: PADDB/W/D/Q, PSUBB/W/D/Q (128-bit)
        decode_modrm(&m,0); xmreg_t s; sse_rm_read(&m,&s,16); xmreg_t* d=&CPU.xmm[m.reg];
        int add=(op2==0xFC||op2==0xFD||op2==0xFE||op2==0xD4);
        switch(op2){
            case 0xFC: case 0xF8: for(int i=0;i<16;i++) d->b[i]=(uint8_t)(add?d->b[i]+s.b[i]:d->b[i]-s.b[i]); break;
            case 0xFD: case 0xF9: for(int i=0;i<8;i++){ uint16_t a=d->b[2*i]|((uint16_t)d->b[2*i+1]<<8),b=s.b[2*i]|((uint16_t)s.b[2*i+1]<<8); uint16_t r=add?a+b:a-b; d->b[2*i]=r&0xff; d->b[2*i+1]=r>>8; } break;
            case 0xFE: case 0xFA: for(int i=0;i<4;i++) d->u[i]=add?d->u[i]+s.u[i]:d->u[i]-s.u[i]; break;
            case 0xD4: case 0xFB: for(int i=0;i<2;i++) d->q[i]=add?d->q[i]+s.q[i]:d->q[i]-s.q[i]; break;
        }
        return 1; }
    case 0x60: case 0x61: case 0x62: case 0x6C: case 0x68: case 0x69: case 0x6A: case 0x6D: { // PUNPCKL/H
        decode_modrm(&m,0); xmreg_t s; sse_rm_read(&m,&s,16); xmreg_t* d=&CPU.xmm[m.reg]; xmreg_t r;
        switch(op2){
            case 0x60: for(int i=0;i<8;i++){ r.b[2*i]=d->b[i]; r.b[2*i+1]=s.b[i]; } break;
            case 0x68: for(int i=0;i<8;i++){ r.b[2*i]=d->b[8+i]; r.b[2*i+1]=s.b[8+i]; } break;
            case 0x61: for(int i=0;i<4;i++){ r.b[4*i]=d->b[2*i]; r.b[4*i+1]=d->b[2*i+1]; r.b[4*i+2]=s.b[2*i]; r.b[4*i+3]=s.b[2*i+1]; } break;
            case 0x69: for(int i=0;i<4;i++){ r.b[4*i]=d->b[8+2*i]; r.b[4*i+1]=d->b[8+2*i+1]; r.b[4*i+2]=s.b[8+2*i]; r.b[4*i+3]=s.b[8+2*i+1]; } break;
            case 0x62: r.u[0]=d->u[0]; r.u[1]=s.u[0]; r.u[2]=d->u[1]; r.u[3]=s.u[1]; break;
            case 0x6A: r.u[0]=d->u[2]; r.u[1]=s.u[2]; r.u[2]=d->u[3]; r.u[3]=s.u[3]; break;
            case 0x6C: r.q[0]=d->q[0]; r.q[1]=s.q[0]; break;
            case 0x6D: r.q[0]=d->q[1]; r.q[1]=s.q[1]; break;
        }
        *d=r; return 1; }
    case 0xD7: { // PMOVMSKB r32, xmm
        decode_modrm(&m,0); xmreg_t* x=&CPU.xmm[m.rm]; uint32_t r=0;
        for(int i=0;i<16;i++) if(x->b[i]&0x80) r|=(1u<<i); setreg(m.reg,4,r); return 1; }
    case 0x64: case 0x65: case 0x66: case 0x74: case 0x75: case 0x76: { // PCMPGT/PCMPEQ B/W/D
        decode_modrm(&m,0); xmreg_t s; sse_rm_read(&m,&s,16); xmreg_t* d=&CPU.xmm[m.reg]; int eq=(op2>=0x74);
        switch(op2){
            case 0x64: case 0x74: for(int i=0;i<16;i++){ int c=eq?(d->b[i]==s.b[i]):((int8_t)d->b[i]>(int8_t)s.b[i]); d->b[i]=c?0xFF:0; } break;
            case 0x65: case 0x75: for(int i=0;i<8;i++){ int16_t a=d->b[2*i]|((uint16_t)d->b[2*i+1]<<8),b=s.b[2*i]|((uint16_t)s.b[2*i+1]<<8); uint16_t r=(eq?(a==b):(a>b))?0xFFFF:0; d->b[2*i]=r&0xff; d->b[2*i+1]=r>>8; } break;
            case 0x66: case 0x76: for(int i=0;i<4;i++){ int c=eq?(d->u[i]==s.u[i]):((int32_t)d->u[i]>(int32_t)s.u[i]); d->u[i]=c?0xFFFFFFFFu:0; } break;
        }
        return 1; }
    case 0xD1: case 0xD2: case 0xD3: case 0xE1: case 0xE2: case 0xF1: case 0xF2: case 0xF3: {
        // packed shift by variable count (low qword of src)
        decode_modrm(&m,0); xmreg_t s; sse_rm_read(&m,&s,16); xmreg_t* d=&CPU.xmm[m.reg]; uint64_t c=s.q[0];
        switch(op2){
            case 0xD1: for(int i=0;i<8;i++){ uint16_t w=d->b[2*i]|((uint16_t)d->b[2*i+1]<<8); w=c>=16?0:(w>>c); d->b[2*i]=w; d->b[2*i+1]=w>>8; } break;
            case 0xD2: for(int i=0;i<4;i++) d->u[i]=c>=32?0:(d->u[i]>>c); break;
            case 0xD3: for(int i=0;i<2;i++) d->q[i]=c>=64?0:(d->q[i]>>c); break;
            case 0xE1: for(int i=0;i<8;i++){ int16_t w=d->b[2*i]|((uint16_t)d->b[2*i+1]<<8); w=(int16_t)(w>>(c>=16?15:c)); d->b[2*i]=w&0xff; d->b[2*i+1]=(w>>8)&0xff; } break;
            case 0xE2: for(int i=0;i<4;i++) d->u[i]=(uint32_t)((int32_t)d->u[i]>>(c>=32?31:(int)c)); break;
            case 0xF1: for(int i=0;i<8;i++){ uint16_t w=d->b[2*i]|((uint16_t)d->b[2*i+1]<<8); w=c>=16?0:(w<<c); d->b[2*i]=w; d->b[2*i+1]=w>>8; } break;
            case 0xF2: for(int i=0;i<4;i++) d->u[i]=c>=32?0:(d->u[i]<<c); break;
            case 0xF3: for(int i=0;i<2;i++) d->q[i]=c>=64?0:(d->q[i]<<c); break;
        }
        return 1; }
    case 0xDA: case 0xDE: case 0xEA: case 0xEE: { // PMINUB/PMAXUB/PMINSW/PMAXSW
        decode_modrm(&m,0); xmreg_t s; sse_rm_read(&m,&s,16); xmreg_t* d=&CPU.xmm[m.reg];
        if(op2==0xDA){ for(int i=0;i<16;i++) if(s.b[i]<d->b[i]) d->b[i]=s.b[i]; }
        else if(op2==0xDE){ for(int i=0;i<16;i++) if(s.b[i]>d->b[i]) d->b[i]=s.b[i]; }
        else { for(int i=0;i<8;i++){ int16_t a=d->b[2*i]|((uint16_t)d->b[2*i+1]<<8), b=s.b[2*i]|((uint16_t)s.b[2*i+1]<<8);
            int16_t r=(op2==0xEA)?(a<b?a:b):(a>b?a:b); d->b[2*i]=r&0xff; d->b[2*i+1]=(r>>8)&0xff; } }
        return 1; }
    case 0xEF: case 0xDB: case 0xEB: case 0xDF: { // PXOR/PAND/POR/PANDN (128-bit)
        decode_modrm(&m,0); xmreg_t s; sse_rm_read(&m,&s,16); xmreg_t* d=&CPU.xmm[m.reg];
        for(int i=0;i<4;i++){ switch(op2){ case 0xEF:d->u[i]^=s.u[i];break; case 0xDB:d->u[i]&=s.u[i];break;
            case 0xEB:d->u[i]|=s.u[i];break; case 0xDF:d->u[i]=(~d->u[i])&s.u[i];break; } } return 1; }
    case 0xC4: { // PINSRW xmm, r/m16, imm8
        decode_modrm(&m,0); uint8_t imm=fetch8(); int sel=imm&7;
        uint16_t w = m.is_mem ? rd16(m.addr) : (uint16_t)getreg(m.rm,4);
        CPU.xmm[m.reg].b[2*sel]=w&0xff; CPU.xmm[m.reg].b[2*sel+1]=w>>8; return 1; }
    case 0xC5: { // PEXTRW r32, xmm, imm8
        decode_modrm(&m,0); uint8_t imm=fetch8(); int sel=imm&7;
        uint16_t w = CPU.xmm[m.rm].b[2*sel] | ((uint16_t)CPU.xmm[m.rm].b[2*sel+1]<<8);
        setreg(m.reg,4,w); return 1; }
    case 0xC6: { // SHUFPS / SHUFPD imm8
        decode_modrm(&m,0); xmreg_t s; sse_rm_read(&m,&s,16); uint8_t imm=fetch8(); xmreg_t* d=&CPU.xmm[m.reg]; xmreg_t r;
        if(p==1){ r.q[0]=d->q[(imm)&1]; r.q[1]=s.q[(imm>>1)&1]; }
        else { r.u[0]=d->u[imm&3]; r.u[1]=d->u[(imm>>2)&3]; r.u[2]=s.u[(imm>>4)&3]; r.u[3]=s.u[(imm>>6)&3]; }
        *d=r; return 1; }
    case 0x70: { // PSHUFD(66)/PSHUFHW(F3)/PSHUFLW(F2)/PSHUFW
        decode_modrm(&m,0); xmreg_t s; sse_rm_read(&m,&s,16); uint8_t imm=fetch8(); xmreg_t* d=&CPU.xmm[m.reg]; xmreg_t r=s;
        if(p==1){ for(int i=0;i<4;i++) r.u[i]=s.u[(imm>>(2*i))&3]; }
        *d=r; return 1; }
    case 0xC2: { // CMPPS/CMPSS imm8 predicate -> mask
        decode_modrm(&m,0); xmreg_t s; uint8_t imm; xmreg_t* d=&CPU.xmm[m.reg];
        #define CMPPRED(a,b) ( ((imm&7)==0)?((a)==(b)) : ((imm&7)==1)?((a)<(b)) : ((imm&7)==2)?((a)<=(b)) : ((imm&7)==3)?((a)!=(a)||(b)!=(b)) : ((imm&7)==4)?((a)!=(b)) : ((imm&7)==5)?(!((a)<(b))) : ((imm&7)==6)?(!((a)<=(b))) : (!((a)!=(a)||(b)!=(b))) )
        if(p==2){ sse_rm_read(&m,&s,4); imm=fetch8(); d->u[0]=CMPPRED(d->f[0],s.f[0])?0xFFFFFFFFu:0; }
        else if(p==3){ sse_rm_read(&m,&s,8); imm=fetch8(); d->q[0]=CMPPRED(d->d[0],s.d[0])?~0ull:0; }
        else if(p==1){ sse_rm_read(&m,&s,16); imm=fetch8(); for(int i=0;i<2;i++) d->q[i]=CMPPRED(d->d[i],s.d[i])?~0ull:0; }
        else { sse_rm_read(&m,&s,16); imm=fetch8(); for(int i=0;i<4;i++) d->u[i]=CMPPRED(d->f[i],s.f[i])?0xFFFFFFFFu:0; }
        #undef CMPPRED
        return 1; }
    }
    return 0;
}
static int sse_is(uint8_t op2){
    switch(op2){
    case 0x10: case 0x11: case 0x12: case 0x13: case 0x14: case 0x15: case 0x16: case 0x17:
    case 0x28: case 0x29: case 0x2A: case 0x2B: case 0x2C: case 0x2D: case 0x2E: case 0x2F:
    case 0x50: case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57:
    case 0x58: case 0x59: case 0x5A: case 0x5B: case 0x5C: case 0x5D: case 0x5E: case 0x5F:
    case 0x60: case 0x61: case 0x62: case 0x64: case 0x65: case 0x66: case 0x68: case 0x69: case 0x6A:
    case 0x6C: case 0x6D: case 0x74: case 0x75: case 0x76:
    case 0x6E: case 0x6F: case 0x70: case 0x71: case 0x72: case 0x73: case 0x7E: case 0x7F: case 0xD7:
    case 0xC2: case 0xC4: case 0xC5: case 0xC6:
    case 0xD1: case 0xD2: case 0xD3: case 0xD4: case 0xD6: case 0xDA: case 0xDB: case 0xDE: case 0xDF:
    case 0xE1: case 0xE2: case 0xE6: case 0xEA: case 0xEB: case 0xEE: case 0xEF:
    case 0xF1: case 0xF2: case 0xF3: case 0xF8: case 0xF9: case 0xFA: case 0xFB: case 0xFC: case 0xFD: case 0xFE:
        return 1;
    }
    return 0;
}

// ---------------- main step ----------------
static void trap(const char* what,uint32_t eip0){
    CPU.faulted=1; CPU.fault_msg=what; CPU.halted=1;
    uint8_t b0=rd8(eip0),b1=rd8(eip0+1),b2=rd8(eip0+2),b3=rd8(eip0+3);
    fprintf(stderr,"** CPU TRAP (%s) eip=0x%08x bytes=%02x %02x %02x %02x\n",what,eip0,b0,b1,b2,b3);
}

int ITRACE = 0; static int itrace_count = 0; static uint32_t g_p5 = 0;
uint64_t g_insns = 0;   // total guest instructions executed (profiling)
uint64_t g_ophist[1024];  // PROFILING: [op] for 1-byte, [256+op2] for 0F xx (temporary)
// focused x87 trace (env EMU_FPUTRACE=lo,hi,max)
static int g_fputrace=-1, g_ftn=0, g_ftmax=0; static uint32_t g_ftlo=0, g_fthi=0;
static int g_newbt = -1;   // BT/BTS memory-operand byte-addressing fix; EMU_OLDBT=1 forces legacy (buggy) path

// ---------------- JIT dispatch (Phase 1: plumbing + deopt; empty blocks just deopt) ----------------
// A compiled block is a () -> i32 WASM function in the shared __indirect_function_table; it reads/writes
// the global CPU + linear memory directly and returns a status. The interpreter stays the baseline and
// the deopt fallback, so correctness never depends on JIT coverage.
#define JIT_OK 0
#define JIT_DEOPT 1
#define JIT_FAULT 2
typedef int (*jit_block_fn)(void);
#ifdef __EMSCRIPTEN__
EM_JS(int, emu_jit_request, (unsigned eip), { return (Module.__jit_compile ? (Module.__jit_compile(eip) | 0) : -1); });
EM_JS(void, emu_jit_flush, (void), { if (Module.__jit_flush) Module.__jit_flush(); });
#else
static int emu_jit_request(unsigned eip){ (void)eip; return -1; }
static void emu_jit_flush(void){}
#endif
#define JITMAP_BITS 17
#define JITMAP_SIZE (1u<<JITMAP_BITS)
#define JITMAP_MASK (JITMAP_SIZE-1)
typedef struct { uint32_t eip; int32_t slot; uint32_t count; } jitent_t;  // slot: table idx, -1 new, -2 tried+failed
static jitent_t* g_jitmap = 0;
int g_jit_enabled = 0, g_jit_force = 0, g_jit_threshold = 50;
uint64_t g_jit_calls = 0, g_jit_deopts = 0, g_jit_compiles = 0;
void jit_reset(void){
    if(!g_jit_enabled && !g_jitmap) return;   // JIT never used this session -> no allocation, zero overhead
    if(!g_jitmap) g_jitmap = (jitent_t*)malloc(JITMAP_SIZE*sizeof(jitent_t));
    if(g_jitmap) for(uint32_t i=0;i<JITMAP_SIZE;i++){ g_jitmap[i].eip=0xFFFFFFFFu; g_jitmap[i].slot=-1; g_jitmap[i].count=0; }
    g_jit_calls=g_jit_deopts=g_jit_compiles=0;
    emu_jit_flush();   // jitmap cleared -> tell JS to free the now-orphaned compiled-block instances (null their table slots)
}
// JIT block linking: base of g_jitmap so compiled blocks can inline the lookup (entry = base + ((eip>>1)&
// JITMAP_MASK)*12; eip@0, slot@4) and return_call_indirect a compiled successor directly. The runtime eip==
// target check makes it eviction-safe. 0 until the map is allocated (first jit_set_enabled).
uint32_t jit_jitmap_addr(void){ return (uint32_t)(uintptr_t)g_jitmap; }
static inline jitent_t* jit_find(uint32_t eip){
    jitent_t* e = &g_jitmap[(eip>>1) & JITMAP_MASK];
    if(e->eip != eip){ e->eip=eip; e->slot=-1; e->count=0; }   // miss or collision -> (re)claim the slot
    return e;
}

int cpu_run(uint64_t max_insns){
    if(g_newbt<0) g_newbt = getenv("EMU_OLDBT") ? 0 : 1;
    if(g_fputrace<0){ const char* e=getenv("EMU_FPUTRACE"); if(e){ unsigned lo,hi,mx; if(sscanf(e,"%x,%x,%u",&lo,&hi,&mx)==3){ g_ftlo=lo; g_fthi=hi; g_ftmax=mx; g_fputrace=1; } else g_fputrace=0; } else g_fputrace=0; }
    static uint32_t eipring[512]; static int eipri=0;
    static uint32_t g_dumpat=0xffffffff; static int g_dumped=0;
    if(g_dumpat==0xffffffff){ const char* e=getenv("EMU_DUMPAT"); g_dumpat = e? (uint32_t)strtoul(e,0,16) : 0; }
    int trace = ITRACE || EMU_VERBOSE || (g_fputrace>0) || g_dumpat;   // hoist all debug out of the hot path
    for(uint64_t n=0; n<max_insns; n++){
#ifdef EMU_OPHIST
        g_insns++;
#endif
        if(CPU.halted){
            if(trace && CPU.faulted && !g_dumped){ g_dumped=1;
                fprintf(stderr,"[fault-ring] eip=%08x fault@%08x eax=%08x ebx=%08x ecx=%08x edx=%08x esi=%08x edi=%08x ebp=%08x esp=%08x\n",
                    CPU.eip,CPU.fault_addr,CPU.r[EAX],CPU.r[EBX],CPU.r[ECX],CPU.r[EDX],CPU.r[ESI],CPU.r[EDI],CPU.r[EBP],CPU.r[ESP]);
                fprintf(stderr,"  call chain (newest last):\n");
                for(int s=0;s<512;s++){ fprintf(stderr," %08x", eipring[(eipri+s)&511]); if((s&15)==15) fprintf(stderr,"\n"); } fprintf(stderr,"\n"); }
            return CPU.faulted?-1:1;
        }
        if(CPU.eip==RET_SENTINEL) return 1;
        if(g_jit_enabled && g_jitmap){
            jitent_t* je = jit_find(CPU.eip);
            if(je->slot >= 0){
                g_jit_calls++;
                int rc = ((jit_block_fn)(uintptr_t)je->slot)();   // call_indirect the compiled block
                if(rc == JIT_OK) continue;                        // block already advanced CPU.eip
                g_jit_deopts++;                                   // DEOPT/FAULT -> interpret from CPU.eip
            } else if(je->slot == -1){
                if(g_jit_force || ++je->count >= (uint32_t)g_jit_threshold){
                    int idx = emu_jit_request(CPU.eip);
                    je->slot = (idx > 0) ? idx : -2;
                    if(idx > 0) g_jit_compiles++;
                }
            }
        }
        if(g_dumpat && CPU.eip==g_dumpat && !g_dumped){ g_dumped=1;
            fprintf(stderr,"[dumpat %08x] retaddr=%08x arg0=%08x eax=%08x ecx=%08x edx=%08x esi=%08x edi=%08x ebp=%08x esp=%08x\n",
                CPU.eip,rd32(CPU.r[ESP]),rd32(CPU.r[ESP]+4),CPU.r[EAX],CPU.r[ECX],CPU.r[EDX],CPU.r[ESI],CPU.r[EDI],CPU.r[EBP],CPU.r[ESP]);
            fprintf(stderr,"  stack:"); for(int s=0;s<12;s++) fprintf(stderr," [esp+%02x]=%08x", s*4, rd32(CPU.r[ESP]+s*4)); fprintf(stderr,"\n");
            fprintf(stderr,"  ebp+ :"); for(int s=0;s<10;s++) fprintf(stderr," [ebp+%02x]=%08x", s*4, rd32(CPU.r[EBP]+s*4)); fprintf(stderr,"\n");
            fprintf(stderr,"  call chain (newest last):\n");
            for(int s=0;s<512;s++){ fprintf(stderr," %08x", eipring[(eipri+s)&511]); if((s&15)==15) fprintf(stderr,"\n"); } fprintf(stderr,"\n"); }
        // (null-call eip<0x1000 is handled by the code-region resolver below: mem_region_of()==NULL -> trap)
        if(trace){
        eipring[eipri&511]=CPU.eip; eipri++;
        if(g_fputrace && CPU.eip>=g_ftlo && CPU.eip<g_fthi && g_ftn<g_ftmax){ g_ftn++;
            if(getenv("EMU_FT8")) fprintf(stderr,"%08x top=%d st: %g %g %g %g %g %g %g %g\n", CPU.eip, CPU.fpu_top,
                CPU.st[CPU.fpu_top&7],CPU.st[(CPU.fpu_top+1)&7],CPU.st[(CPU.fpu_top+2)&7],CPU.st[(CPU.fpu_top+3)&7],
                CPU.st[(CPU.fpu_top+4)&7],CPU.st[(CPU.fpu_top+5)&7],CPU.st[(CPU.fpu_top+6)&7],CPU.st[(CPU.fpu_top+7)&7]);
            else fprintf(stderr,"%08x top=%d st0=%g st1=%g st2=%g\n", CPU.eip, CPU.fpu_top, CPU.st[CPU.fpu_top&7], CPU.st[(CPU.fpu_top+1)&7], CPU.st[(CPU.fpu_top+2)&7]); }
        if(ITRACE && CPU.eip>=0x1001b0a0 && CPU.eip<=0x1001b104 && itrace_count<400){
            itrace_count++;
            fprintf(stderr,"%08x eax=%08x ebx=%08x ecx=%08x edx=%08x esi=%08x edi=%08x ebp=%08x\n",
                CPU.eip,CPU.r[EAX],CPU.r[EBX],CPU.r[ECX],CPU.r[EDX],CPU.r[ESI],CPU.r[EDI],CPU.r[EBP]);
        }
        if(EMU_VERBOSE){
            switch(CPU.eip){
            case 0x10046a60: emu_log("[trace] _DllMainCRTStartup(reason=%u)\n",rd32(CPU.r[ESP]+8)); break;
            case 0x100468f0: emu_log("[trace] _CRT_INIT(reason=%u)\n",rd32(CPU.r[ESP]+8)); break;
            case 0x100481c0: emu_log("[trace]   heap_init\n"); break;
            case 0x1004a7e0: emu_log("[trace]   ioinit/mtinit\n"); break;
            case 0x1004b2d0: emu_log("[trace]   env_init\n"); break;
            case 0x1004a9b0: emu_log("[trace]   _cinit (static ctors)\n"); break;
            case 0x10018800: emu_log("[trace] DllMain(hinst=%08x reason=%u)\n",rd32(CPU.r[ESP]+4),rd32(CPU.r[ESP]+8)); break;
            case 0x10018420: emu_log("[trace] config_loader entered\n"); break;
            case 0x10040820: emu_log("[trace] FUN_10040820 (dict/sb setup) entered\n"); break;
            case 0x10042840: emu_log("[trace] load_all_hash_table entered\n"); break;
            #define KEYLOG(tag) do{ char w[48]; uint32_t p=CPU.r[EDI]; int k=0; for(;k<47;k++){char c=(char)rd8(p+k);w[k]=c;if(!c)break;} w[k]=0; emu_log("[key %s] '%s'\n",tag,w); }while(0)
            case 0x10014ca0: { int t=(int)rd32(CPU.r[ESP]+4); int p3=(int)(char)rd32(CPU.r[ESP]+0xc); char s[4][24];
                uint32_t pp[4]={rd32(CPU.r[ESP]+0x10),rd32(CPU.r[ESP]+0x1c),rd32(CPU.r[ESP]+0x20),rd32(CPU.r[ESP]+0x24)};
                for(int a=0;a<4;a++){int k=0;for(;k<23;k++){char c=(char)rd8(pp[a]+k);s[a][k]=c;if(!c)break;}s[a][k]=0;}
                emu_log("[ctxbuild] type=%d p3='%c'(%d) key='%s' p7='%s' p8='%s' p9='%s'\n",t,p3>=32?p3:'.',p3,s[0],s[1],s[2],s[3]); } break;
            case 0x1002f746: { uint32_t base=CPU.r[ESP]+0x20; for(int u=0;u<28;u++){ char w[24]; uint32_t p=base+0x14+u*0x14; int k=0; for(;k<23;k++){char c=(char)rd8(p+k);w[k]=c;if(!c)break;} w[k]=0; if(!w[0]&&u>2)continue; emu_log("[emu11570 %2d] '%s'\n",u,w);} } break;
            case 0x1002f730: { char w[256]; uint32_t p=CPU.r[EDI]; int k=0; for(;k<255;k++){char c=(char)rd8(p+k);w[k]=c;if(!c)break;} w[k]=0; emu_log("[b3a0->11570 in] '%s'\n",w);
                if(getenv("EMU_DUMP11570IN")){ FILE* df=fopen(getenv("EMU_DUMP11570IN"),"wb"); if(df){ fwrite(w,1,k,df); fclose(df); } } } break;
            case 0x1002f743: { char w[256]; uint32_t p=CPU.r[EDI]; int k=0; for(;k<255;k++){char c=(char)rd8(p+k);w[k]=c;if(!c)break;} w[k]=0; emu_log("[after 11570 in] '%s'\n",w); } break;
            case 0x1002f751: KEYLOG("after clear"); break;
            case 0x1002f764: { char w[256]; uint32_t p=CPU.r[EDI]; int k=0; for(;k<255;k++){char c=(char)rd8(p+k);w[k]=c;if(!c)break;} w[k]=0; emu_log("[seg] '%s'\n",w); } break;
            case 0x1001b317: { char a[40],b[40]; uint32_t pa=rd32(CPU.r[ESP]+0),pb=rd32(CPU.r[ESP]+4); int k=0;
                for(;k<39;k++){char c=(char)rd8(pa+k);a[k]=c;if(!c)break;} a[k]=0;
                for(k=0;k<39;k++){char c=(char)rd8(pb+k);b[k]=c;if(!c)break;} b[k]=0;
                emu_log("[ctxcmp] candidate='%s' vs target='%s'\n",a,b); } break;
            case 0x1001af40: { char w[40]; uint32_t pw=rd32(CPU.r[ESP]+4); int k=0; for(;k<39;k++){char c=(char)rd8(pw+k); w[k]=c; if(!c)break;} w[k]=0;
                emu_log("[GetPospPhonenc] word='%s' p6=%08x p7=%08x p8=%08x p9=%08x sz=%u\n", w,
                    rd32(CPU.r[ESP]+0x18),rd32(CPU.r[ESP]+0x1c),rd32(CPU.r[ESP]+0x20),rd32(CPU.r[ESP]+0x24),rd32(CPU.r[ESP]+0x28)); } break;
            case 0x100211c4: { uint32_t off=CPU.r[EBP]; char key[24]; uint32_t kp=CPU.r[ESP]+0x18; int k=0;
                for(;k<23;k++){char c=(char)rd8(kp+k);key[k]=c;if(!c)break;} key[k]=0;
                if(off>=24500 && off<=28000) emu_log("[unit @%6u] '%s'\n", off, key); } break;
            case 0x100152d0: { char p4[48],p5[48]; uint32_t a4=rd32(CPU.r[ESP]+0x10),a5=rd32(CPU.r[ESP]+0x14);
                int k=0; for(;k<47;k++){char c=(char)rd8(a4+k);p4[k]=c;if(!c)break;} p4[k]=0; int l4=k;
                for(k=0;k<47;k++){char c=(char)rd8(a5+k);p5[k]=c;if(!c)break;} p5[k]=0;
                emu_log("[ctxsel] follow_vowel='%s'(len%d) follow_onset='%s'(len%d) onset_bytes=%02x %02x %02x %02x\n",
                    p4,l4,p5,k,rd8(a5),rd8(a5+1),rd8(a5+2),rd8(a5+3)); } break;
            case 0x10015314: { char p5[48]; uint32_t a5=CPU.r[ESI]; int k=0;
                for(;k<47;k++){char c=(char)rd8(a5+k);p5[k]=c;if(!c)break;} p5[k]=0;
                emu_log("[classify] onset='%s' -> class=%d\n", p5, (int)CPU.r[EAX]); } break;
            }
        }
        }   // end if(trace)
        uint32_t eip0=CPU.eip;
        if((uint32_t)(eip0 - g_code_lo) >= g_code_size){   // outside cached code region (rare: branch into another region)
            if(win32_is_import_va(eip0)){ win32_dispatch(eip0); continue; }   // import thunk dispatch (was per-instruction; now only on region miss)
            region_t* r = mem_region_of(eip0);
            if(!r){ if(eip0<0x1000) fprintf(stderr,"[null-call] eip=%08x esp=%08x\n",eip0,CPU.r[ESP]); trap("fetch", eip0); break; }
            g_code_host=r->host; g_code_lo=r->va; g_code_size=r->size;
        }
        g_ip = g_code_host + (eip0 - g_code_lo);    // raw code pointer (register arithmetic, no page-map load)
        // ---- prefixes ----
        int opsz=4, rep=0, repne=0; g_seg_base=0;
        while(IS_PREFIX[*g_ip]){
            uint8_t p=*g_ip; g_ip++; CPU.eip++;
            if(p==0x66) opsz=2;
            else if(p==0xF2) repne=1;
            else if(p==0xF3) rep=1;
            else if(p==0x64) g_seg_base=CPU.seg_fs_base;
            else if(p==0x65) g_seg_base=CPU.seg_gs_base;
            // 0x67 (addr-size), 0xF0 (lock), 0x2E/36/3E/26 (CS/SS/DS/ES, flat) = no effect
        }
        uint8_t op=fetch8();
#ifdef EMU_OPHIST
        g_ophist[op]++;
#endif
        modrm_t m;
        switch(op){
        // ----- ALU reg/rm families: 00..3D -----
        #define ALU_GROUP(base,fn) \
            case base+0:{decode_modrm(&m,0);uint32_t a=rm_read(&m,1),b=getreg(m.reg,1);rm_write(&m,1,fn(a,b,1));}break; \
            case base+1:{decode_modrm(&m,0);uint32_t a=rm_read(&m,opsz),b=getreg(m.reg,opsz);rm_write(&m,opsz,fn(a,b,opsz));}break; \
            case base+2:{decode_modrm(&m,0);uint32_t a=getreg(m.reg,1),b=rm_read(&m,1);setreg(m.reg,1,fn(a,b,1));}break; \
            case base+3:{decode_modrm(&m,0);uint32_t a=getreg(m.reg,opsz),b=rm_read(&m,opsz);setreg(m.reg,opsz,fn(a,b,opsz));}break; \
            case base+4:{uint32_t a=getreg(EAX,1),b=fetch8();setreg(EAX,1,fn(a,b,1));}break; \
            case base+5:{uint32_t a=getreg(EAX,opsz),b=(opsz==2?fetch16():fetch32());setreg(EAX,opsz,fn(a,b,opsz));}break;
        ALU_GROUP(0x00,do_add) ALU_GROUP(0x08,do_logic_or) ALU_GROUP(0x10,do_adc)
        ALU_GROUP(0x18,do_sbb) ALU_GROUP(0x20,do_logic_and) ALU_GROUP(0x28,do_sub)
        ALU_GROUP(0x30,do_logic_xor)
        // CMP (0x38..0x3D): like SUB but discard result
        case 0x38:{decode_modrm(&m,0);do_sub(rm_read(&m,1),getreg(m.reg,1),1);}break;
        case 0x39:{decode_modrm(&m,0);do_sub(rm_read(&m,opsz),getreg(m.reg,opsz),opsz);}break;
        case 0x3A:{decode_modrm(&m,0);do_sub(getreg(m.reg,1),rm_read(&m,1),1);}break;
        case 0x3B:{decode_modrm(&m,0);do_sub(getreg(m.reg,opsz),rm_read(&m,opsz),opsz);}break;
        case 0x3C:{do_sub(getreg(EAX,1),fetch8(),1);}break;
        case 0x3D:{do_sub(getreg(EAX,opsz),(opsz==2?fetch16():fetch32()),opsz);}break;

        case 0x40:case 0x41:case 0x42:case 0x43:case 0x44:case 0x45:case 0x46:case 0x47:
            setreg(op-0x40,opsz,do_inc(getreg(op-0x40,opsz),opsz)); break;
        case 0x48:case 0x49:case 0x4A:case 0x4B:case 0x4C:case 0x4D:case 0x4E:case 0x4F:
            setreg(op-0x48,opsz,do_dec(getreg(op-0x48,opsz),opsz)); break;
        case 0x50:case 0x51:case 0x52:case 0x53:case 0x54:case 0x55:case 0x56:case 0x57:
            cpu_push32(CPU.r[op-0x50]); break;
        case 0x58:case 0x59:case 0x5A:case 0x5B:case 0x5C:case 0x5D:case 0x5E:case 0x5F:
            CPU.r[op-0x58]=cpu_pop32(); break;
        case 0x68: cpu_push32(fetch32()); break;
        case 0x6A: cpu_push32((int32_t)(int8_t)fetch8()); break;
        case 0x69:{decode_modrm(&m,0);int32_t a=(int32_t)rm_read(&m,opsz);int32_t b=(int32_t)(opsz==2?(int16_t)fetch16():(int32_t)fetch32());int64_t r=(int64_t)a*b;setreg(m.reg,opsz,(uint32_t)r);setf(FL_CF,(int64_t)(int32_t)r!=r);setf(FL_OF,(int64_t)(int32_t)r!=r);}break;
        case 0x6B:{decode_modrm(&m,0);int32_t a=(int32_t)rm_read(&m,opsz);int32_t b=(int8_t)fetch8();int64_t r=(int64_t)a*b;setreg(m.reg,opsz,(uint32_t)r);setf(FL_CF,(int64_t)(int32_t)r!=r);setf(FL_OF,(int64_t)(int32_t)r!=r);}break;

        // Jcc short
        case 0x70:case 0x71:case 0x72:case 0x73:case 0x74:case 0x75:case 0x76:case 0x77:
        case 0x78:case 0x79:case 0x7A:case 0x7B:case 0x7C:case 0x7D:case 0x7E:case 0x7F:
            { int8_t d=(int8_t)fetch8(); if(cond(op-0x70)) CPU.eip+=d; } break;

        // group1 80/81/83
        case 0x80:{decode_modrm(&m,0);uint32_t a=rm_read(&m,1),b=fetch8();uint32_t r;switch(m.reg){
            case 0:r=do_add(a,b,1);break;case 1:r=do_logic_or(a,b,1);break;case 2:r=do_adc(a,b,1);break;case 3:r=do_sbb(a,b,1);break;
            case 4:r=do_logic_and(a,b,1);break;case 5:r=do_sub(a,b,1);break;case 6:r=do_logic_xor(a,b,1);break;default:do_sub(a,b,1);r=a;}
            if(m.reg!=7)rm_write(&m,1,r);}break;
        case 0x81:{decode_modrm(&m,0);uint32_t a=rm_read(&m,opsz),b=(opsz==2?fetch16():fetch32());uint32_t r;switch(m.reg){
            case 0:r=do_add(a,b,opsz);break;case 1:r=do_logic_or(a,b,opsz);break;case 2:r=do_adc(a,b,opsz);break;case 3:r=do_sbb(a,b,opsz);break;
            case 4:r=do_logic_and(a,b,opsz);break;case 5:r=do_sub(a,b,opsz);break;case 6:r=do_logic_xor(a,b,opsz);break;default:do_sub(a,b,opsz);r=a;}
            if(m.reg!=7)rm_write(&m,opsz,r);}break;
        case 0x83:{decode_modrm(&m,0);uint32_t a=rm_read(&m,opsz),b=(uint32_t)(int32_t)(int8_t)fetch8();uint32_t r;switch(m.reg){
            case 0:r=do_add(a,b,opsz);break;case 1:r=do_logic_or(a,b,opsz);break;case 2:r=do_adc(a,b,opsz);break;case 3:r=do_sbb(a,b,opsz);break;
            case 4:r=do_logic_and(a,b,opsz);break;case 5:r=do_sub(a,b,opsz);break;case 6:r=do_logic_xor(a,b,opsz);break;default:do_sub(a,b,opsz);r=a;}
            if(m.reg!=7)rm_write(&m,opsz,r);}break;

        case 0x84:{decode_modrm(&m,0);do_logic(rm_read(&m,1)&getreg(m.reg,1),1);}break;
        case 0x85:{decode_modrm(&m,0);do_logic(rm_read(&m,opsz)&getreg(m.reg,opsz),opsz);}break;
        case 0x86:{decode_modrm(&m,0);uint32_t a=rm_read(&m,1),b=getreg(m.reg,1);rm_write(&m,1,b);setreg(m.reg,1,a);}break;
        case 0x87:{decode_modrm(&m,0);uint32_t a=rm_read(&m,opsz),b=getreg(m.reg,opsz);rm_write(&m,opsz,b);setreg(m.reg,opsz,a);}break;
        case 0x88:{decode_modrm(&m,0);rm_write(&m,1,getreg(m.reg,1));}break;
        case 0x89:{decode_modrm(&m,0);rm_write(&m,opsz,getreg(m.reg,opsz));}break;
        case 0x8A:{decode_modrm(&m,0);setreg(m.reg,1,rm_read(&m,1));}break;
        case 0x8B:{decode_modrm(&m,0);setreg(m.reg,opsz,rm_read(&m,opsz));}break;
        case 0x8D:{decode_modrm(&m,0);setreg(m.reg,opsz,m.addr);}break; // LEA (addr already includes seg base 0)
        case 0x8C:{decode_modrm(&m,0); static const uint16_t sel[8]={0x23,0x1b,0x23,0x23,0x3b,0x63,0,0}; rm_write(&m,2,sel[m.reg&7]);}break; // MOV r/m16, Sreg (flat selectors)
        case 0x8E:{decode_modrm(&m,0); (void)rm_read(&m,2);}break; // MOV Sreg, r/m16 (flat: ignore)
        case 0x8F:{decode_modrm(&m,0);rm_write(&m,opsz,cpu_pop32());}break; // POP r/m

        case 0x9B: break; // FWAIT/WAIT - no-op (FPU never faults here)
        case 0x90: break; // NOP (also XCHG eAX,eAX)
        case 0x91:case 0x92:case 0x93:case 0x94:case 0x95:case 0x96:case 0x97:
            { uint32_t t=CPU.r[EAX]; CPU.r[EAX]=CPU.r[op-0x90]; CPU.r[op-0x90]=t; } break;
        case 0x98: if(opsz==4) CPU.r[EAX]=(int32_t)(int16_t)(CPU.r[EAX]&0xffff); else CPU.r[EAX]=(CPU.r[EAX]&~0xffffu)|((uint32_t)(int16_t)(int8_t)(CPU.r[EAX]&0xff)&0xffff); break; // CWDE/CBW
        case 0x99: if(opsz==4) CPU.r[EDX]=(CPU.r[EAX]&0x80000000)?0xffffffff:0; else setreg(EDX,2,(getreg(EAX,2)&0x8000)?0xffff:0); break; // CDQ/CWD

        case 0xA0:{uint32_t a=fetch32()+g_seg_base;setreg(EAX,1,rd8(a));}break;
        case 0xA1:{uint32_t a=fetch32()+g_seg_base;setreg(EAX,opsz,opsz==2?rd16(a):rd32(a));}break;
        case 0xA2:{uint32_t a=fetch32()+g_seg_base;wr8(a,getreg(EAX,1));}break;
        case 0xA3:{uint32_t a=fetch32()+g_seg_base;if(opsz==2)wr16(a,getreg(EAX,2));else wr32(a,getreg(EAX,4));}break;
        case 0xA8:{do_logic(getreg(EAX,1)&fetch8(),1);}break;
        case 0xA9:{do_logic(getreg(EAX,opsz)&(opsz==2?fetch16():fetch32()),opsz);}break;

        // string ops
        case 0xA4: case 0xA5: { int sz=(op==0xA4)?1:opsz; uint32_t cnt=rep||repne?CPU.r[ECX]:1;
            while(cnt){ uint32_t v=(sz==1?rd8(CPU.r[ESI]):sz==2?rd16(CPU.r[ESI]):rd32(CPU.r[ESI]));
                if(sz==1)wr8(CPU.r[EDI],v);else if(sz==2)wr16(CPU.r[EDI],v);else wr32(CPU.r[EDI],v);
                int d=(CPU.eflags&FL_DF)?-sz:sz; CPU.r[ESI]+=d; CPU.r[EDI]+=d; cnt--; if(rep||repne)CPU.r[ECX]--; if(!(rep||repne))break; }
            } break;
        case 0xAA: case 0xAB: { int sz=(op==0xAA)?1:opsz; uint32_t cnt=rep||repne?CPU.r[ECX]:1; uint32_t val=getreg(EAX,sz);
            while(cnt){ if(sz==1)wr8(CPU.r[EDI],val);else if(sz==2)wr16(CPU.r[EDI],val);else wr32(CPU.r[EDI],val);
                int d=(CPU.eflags&FL_DF)?-sz:sz; CPU.r[EDI]+=d; cnt--; if(rep||repne)CPU.r[ECX]--; if(!(rep||repne))break; }
            } break;
        case 0xAC: case 0xAD: { int sz=(op==0xAC)?1:opsz; uint32_t cnt=rep||repne?CPU.r[ECX]:1;
            while(cnt){ setreg(EAX,sz,(sz==1?rd8(CPU.r[ESI]):sz==2?rd16(CPU.r[ESI]):rd32(CPU.r[ESI])));
                int d=(CPU.eflags&FL_DF)?-sz:sz; CPU.r[ESI]+=d; cnt--; if(rep||repne)CPU.r[ECX]--; if(!(rep||repne))break; }
            } break;
        case 0xAE: case 0xAF: { int sz=(op==0xAE)?1:opsz; uint32_t cnt=(rep||repne)?CPU.r[ECX]:1;
            while(cnt){ uint32_t a=getreg(EAX,sz),b=(sz==1?rd8(CPU.r[EDI]):sz==2?rd16(CPU.r[EDI]):rd32(CPU.r[EDI])); do_sub(a,b,sz);
                int d=(CPU.eflags&FL_DF)?-sz:sz; CPU.r[EDI]+=d; cnt--; if(rep||repne){CPU.r[ECX]--; int zf=!!(CPU.eflags&FL_ZF); if(rep&&!zf)break; if(repne&&zf)break;} else break; }
            } break;
        case 0xA6: case 0xA7: { int sz=(op==0xA6)?1:opsz; uint32_t cnt=(rep||repne)?CPU.r[ECX]:1;
            while(cnt){ uint32_t a=(sz==1?rd8(CPU.r[ESI]):sz==2?rd16(CPU.r[ESI]):rd32(CPU.r[ESI])),b=(sz==1?rd8(CPU.r[EDI]):sz==2?rd16(CPU.r[EDI]):rd32(CPU.r[EDI])); do_sub(a,b,sz);
                int d=(CPU.eflags&FL_DF)?-sz:sz; CPU.r[ESI]+=d; CPU.r[EDI]+=d; cnt--; if(rep||repne){CPU.r[ECX]--; int zf=!!(CPU.eflags&FL_ZF); if(rep&&!zf)break; if(repne&&zf)break;} else break; }
            } break;

        case 0xB0:case 0xB1:case 0xB2:case 0xB3:case 0xB4:case 0xB5:case 0xB6:case 0xB7:
            setreg(op-0xB0,1,fetch8()); break;
        case 0xB8:case 0xB9:case 0xBA:case 0xBB:case 0xBC:case 0xBD:case 0xBE:case 0xBF:
            setreg(op-0xB8,opsz,opsz==2?fetch16():fetch32()); break;

        // group2 shifts
        case 0xC0:{decode_modrm(&m,0);uint32_t v=rm_read(&m,1);uint8_t c=fetch8();rm_write(&m,1,do_shift(m.reg,v,c,1));}break;
        case 0xC1:{decode_modrm(&m,0);uint32_t v=rm_read(&m,opsz);uint8_t c=fetch8();rm_write(&m,opsz,do_shift(m.reg,v,c,opsz));}break;
        case 0xD0:{decode_modrm(&m,0);rm_write(&m,1,do_shift(m.reg,rm_read(&m,1),1,1));}break;
        case 0xD1:{decode_modrm(&m,0);rm_write(&m,opsz,do_shift(m.reg,rm_read(&m,opsz),1,opsz));}break;
        case 0xD2:{decode_modrm(&m,0);rm_write(&m,1,do_shift(m.reg,rm_read(&m,1),CPU.r[ECX]&0xff,1));}break;
        case 0xD3:{decode_modrm(&m,0);rm_write(&m,opsz,do_shift(m.reg,rm_read(&m,opsz),CPU.r[ECX]&0xff,opsz));}break;

        case 0xC2:{uint16_t imm=fetch16();uint32_t ra=cpu_pop32();CPU.r[ESP]+=imm;CPU.eip=ra;}break;
        case 0xC3:{CPU.eip=cpu_pop32();}break;
        case 0xC6:{decode_modrm(&m,0);uint8_t imm=fetch8();rm_write(&m,1,imm);}break;
        case 0xC7:{decode_modrm(&m,0);uint32_t imm=(opsz==2?fetch16():fetch32());rm_write(&m,opsz,imm);}break;
        case 0xC9:{CPU.r[ESP]=CPU.r[EBP];CPU.r[EBP]=cpu_pop32();}break; // LEAVE
        case 0xCC: trap("int3",eip0); break;

        case 0xE8:{int32_t d=(int32_t)fetch32();cpu_push32(CPU.eip);CPU.eip+=d;}break; // CALL rel32
        case 0xE9:{int32_t d=(int32_t)fetch32();CPU.eip+=d;}break; // JMP rel32
        case 0xEB:{int8_t d=(int8_t)fetch8();CPU.eip+=d;}break;    // JMP rel8
        case 0xE3:{int8_t d=(int8_t)fetch8(); if((opsz==2?getreg(ECX,2):CPU.r[ECX])==0) CPU.eip+=d;}break; // JECXZ
        case 0xE0:case 0xE1:case 0xE2:{int8_t d=(int8_t)fetch8();CPU.r[ECX]--;int take=(CPU.r[ECX]!=0);
            if(op==0xE1)take=take&&(CPU.eflags&FL_ZF);if(op==0xE0)take=take&&!(CPU.eflags&FL_ZF);if(take)CPU.eip+=d;}break;

        // group3 F6/F7
        case 0xF6:{decode_modrm(&m,0);uint32_t a=rm_read(&m,1);switch(m.reg){
            case 0:case 1:{uint8_t imm=fetch8();do_logic(a&imm,1);}break;
            case 2:rm_write(&m,1,~a);break;
            case 3:rm_write(&m,1,do_sub(0,a,1));break;
            case 4:{uint32_t r=(getreg(EAX,1)&0xff)*(a&0xff);setreg(EAX,2,r);setf(FL_CF,(r&0xff00)!=0);setf(FL_OF,(r&0xff00)!=0);}break;
            case 5:{int32_t r=(int8_t)getreg(EAX,1)*(int8_t)a;setreg(EAX,2,(uint32_t)r);setf(FL_CF,(int8_t)r!=r);setf(FL_OF,(int8_t)r!=r);}break;
            case 6:{uint32_t ax=getreg(EAX,2);if((a&0xff)==0){trap("div0",eip0);break;}setreg(EAX,1,ax/(a&0xff));setreg(EAX,1,ax/(a&0xff));{uint32_t q=ax/(a&0xff),rem=ax%(a&0xff);setreg(EAX,1,q);setreg(EAX,1,q);CPU.r[EAX]=(CPU.r[EAX]&~0xffffu)|((q&0xff)|((rem&0xff)<<8));}}break;
            case 7:{int16_t ax=(int16_t)getreg(EAX,2);if((a&0xff)==0){trap("idiv0",eip0);break;}int8_t q=ax/(int8_t)a,rem=ax%(int8_t)a;CPU.r[EAX]=(CPU.r[EAX]&~0xffffu)|((uint8_t)q|((uint8_t)rem<<8));}break;
            }}break;
        case 0xF7:{decode_modrm(&m,0);uint32_t a=rm_read(&m,opsz);switch(m.reg){
            case 0:case 1:{uint32_t imm=(opsz==2?fetch16():fetch32());do_logic(a&imm,opsz);}break;
            case 2:rm_write(&m,opsz,~a);break;
            case 3:rm_write(&m,opsz,do_sub(0,a,opsz));break;
            case 4:{ if(opsz==4){uint64_t r=(uint64_t)CPU.r[EAX]*a;CPU.r[EAX]=(uint32_t)r;CPU.r[EDX]=(uint32_t)(r>>32);setf(FL_CF,CPU.r[EDX]!=0);setf(FL_OF,CPU.r[EDX]!=0);} else {uint32_t r=getreg(EAX,2)*(a&0xffff);setreg(EAX,2,r);setreg(EDX,2,r>>16);setf(FL_CF,(r>>16)!=0);setf(FL_OF,(r>>16)!=0);} }break;
            case 5:{ if(opsz==4){int64_t r=(int64_t)(int32_t)CPU.r[EAX]*(int32_t)a;CPU.r[EAX]=(uint32_t)r;CPU.r[EDX]=(uint32_t)((uint64_t)r>>32);setf(FL_CF,(int64_t)(int32_t)r!=r);setf(FL_OF,(int64_t)(int32_t)r!=r);} else {int32_t r=(int16_t)getreg(EAX,2)*(int16_t)a;setreg(EAX,2,r);setreg(EDX,2,r>>16);} }break;
            case 6:{ if(opsz==4){ if(a==0){trap("div0",eip0);break;} uint64_t num=((uint64_t)CPU.r[EDX]<<32)|CPU.r[EAX]; CPU.r[EAX]=(uint32_t)(num/a); CPU.r[EDX]=(uint32_t)(num%a);} else { uint32_t num=(getreg(EDX,2)<<16)|getreg(EAX,2); if((a&0xffff)==0){trap("div0",eip0);break;} setreg(EAX,2,num/(a&0xffff)); setreg(EDX,2,num%(a&0xffff)); } }break;
            case 7:{ if(opsz==4){ if(a==0){trap("idiv0",eip0);break;} int64_t num=((int64_t)(int32_t)CPU.r[EDX]<<32)|CPU.r[EAX]; CPU.r[EAX]=(uint32_t)(num/(int32_t)a); CPU.r[EDX]=(uint32_t)(num%(int32_t)a);} else { int32_t num=((int32_t)(int16_t)getreg(EDX,2)<<16)|getreg(EAX,2); if((a&0xffff)==0){trap("idiv0",eip0);break;} setreg(EAX,2,(uint32_t)(num/(int16_t)a)); setreg(EDX,2,(uint32_t)(num%(int16_t)a)); } }break;
            }}break;

        case 0xF5: setf(FL_CF,!(CPU.eflags&FL_CF)); break; // CMC
        case 0xF8: setf(FL_CF,0); break;
        case 0xF9: setf(FL_CF,1); break;
        case 0xFC: setf(FL_DF,0); break;
        case 0xFD: setf(FL_DF,1); break;

        case 0xFE:{decode_modrm(&m,0);uint32_t a=rm_read(&m,1);rm_write(&m,1,m.reg==0?do_inc(a,1):do_dec(a,1));}break;
        case 0xFF:{decode_modrm(&m,0);switch(m.reg){
            case 0:rm_write(&m,opsz,do_inc(rm_read(&m,opsz),opsz));break;
            case 1:rm_write(&m,opsz,do_dec(rm_read(&m,opsz),opsz));break;
            case 2:{uint32_t t=rm_read(&m,opsz);cpu_push32(CPU.eip);CPU.eip=t;}break; // CALL r/m
            case 3:{uint32_t t=rm_read(&m,opsz);cpu_push32(CPU.eip);CPU.eip=t;}break; // CALL far (treat near)
            case 4:{CPU.eip=rm_read(&m,opsz);}break; // JMP r/m
            case 5:{CPU.eip=rm_read(&m,opsz);}break; // JMP far (treat near)
            case 6:cpu_push32(rm_read(&m,opsz));break; // PUSH r/m
            }}break;

        case 0xD7: { uint32_t a=CPU.r[EBX]+(CPU.r[EAX]&0xff)+g_seg_base; setreg(EAX,1,rd8(a)); } break; // XLAT
        case 0x9C: cpu_push32(CPU.eflags); break; // PUSHFD
        case 0x9D: CPU.eflags=(cpu_pop32()&0xcd5)|0x2; break; // POPFD (keep arith/DF/IF-ish)
        case 0x9E:{uint8_t ah=(CPU.r[EAX]>>8)&0xff; CPU.eflags=(CPU.eflags&~0xd5u)|((ah&0xd5)|0x2);}break; // SAHF
        case 0x9F:{uint8_t f=CPU.eflags&0xd5; f|=0x2; CPU.r[EAX]=(CPU.r[EAX]&~0xff00u)|((uint32_t)f<<8);}break; // LAHF
        case 0x60:{uint32_t sp=CPU.r[ESP];cpu_push32(CPU.r[EAX]);cpu_push32(CPU.r[ECX]);cpu_push32(CPU.r[EDX]);cpu_push32(CPU.r[EBX]);cpu_push32(sp);cpu_push32(CPU.r[EBP]);cpu_push32(CPU.r[ESI]);cpu_push32(CPU.r[EDI]);}break; // PUSHAD
        case 0x61:{CPU.r[EDI]=cpu_pop32();CPU.r[ESI]=cpu_pop32();CPU.r[EBP]=cpu_pop32();cpu_pop32();CPU.r[EBX]=cpu_pop32();CPU.r[EDX]=cpu_pop32();CPU.r[ECX]=cpu_pop32();CPU.r[EAX]=cpu_pop32();}break; // POPAD

        // x87 escape range D8..DF
        case 0xD8:case 0xD9:case 0xDA:case 0xDB:case 0xDC:case 0xDD:case 0xDE:case 0xDF:
            if(!do_x87(op)) trap("x87",eip0); break;

        case 0x0F: { uint8_t op2=fetch8();
#ifdef EMU_OPHIST
            g_ophist[256+op2]++;
#endif
            if(sse_is(op2)){ int ssep = rep?2 : repne?3 : (opsz==2)?1 : 0; if(!do_sse(op2,ssep)) trap("sse",eip0); }
            else if(op2>=0x80 && op2<=0x8F){ int32_t d=(int32_t)(opsz==2?(int16_t)fetch16():(int32_t)fetch32()); if(cond(op2-0x80)) CPU.eip+=d; }
            else if(op2>=0x90 && op2<=0x9F){ decode_modrm(&m,0); rm_write(&m,1,cond(op2-0x90)?1:0); }
            else if(op2>=0x40 && op2<=0x4F){ decode_modrm(&m,0); uint32_t v=rm_read(&m,opsz); if(cond(op2-0x40)) setreg(m.reg,opsz,v); } // CMOVcc
            else switch(op2){
                case 0xB6:{decode_modrm(&m,0);setreg(m.reg,opsz,rm_read(&m,1)&0xff);}break; // MOVZX r,r/m8
                case 0xB7:{decode_modrm(&m,0);setreg(m.reg,opsz,rm_read(&m,2)&0xffff);}break; // MOVZX r,r/m16
                case 0xBE:{decode_modrm(&m,0);setreg(m.reg,opsz,(uint32_t)(int32_t)(int8_t)rm_read(&m,1));}break; // MOVSX r,r/m8
                case 0xBF:{decode_modrm(&m,0);setreg(m.reg,opsz,(uint32_t)(int32_t)(int16_t)rm_read(&m,2));}break; // MOVSX r,r/m16
                case 0xAF:{decode_modrm(&m,0);int32_t a=(int32_t)getreg(m.reg,opsz),b=(int32_t)rm_read(&m,opsz);int64_t r=(int64_t)a*b;setreg(m.reg,opsz,(uint32_t)r);setf(FL_CF,(int64_t)(int32_t)r!=r);setf(FL_OF,(int64_t)(int32_t)r!=r);}break; // IMUL
                case 0xA2: { uint32_t leaf=CPU.r[EAX]; // CPUID: advertise FPU/TSC/CMOV/MMX/FXSR/SSE/SSE2 (what we emulate)
                    if(leaf==0){ CPU.r[EAX]=1; CPU.r[EBX]=0x756e6547; CPU.r[EDX]=0x49656e69; CPU.r[ECX]=0x6c65746e; } // "GenuineIntel"
                    else if(leaf==1){ CPU.r[EAX]=0x00000623; CPU.r[EBX]=0x00000800; CPU.r[ECX]=0; CPU.r[EDX]=0x07808011; }
                    else { CPU.r[EAX]=CPU.r[EBX]=CPU.r[ECX]=CPU.r[EDX]=0; } } break;
                case 0x31: CPU.r[EAX]=0;CPU.r[EDX]=0; break; // RDTSC stub
                // BT/BTS/BTR/BTC with REGISTER bit index. With a MEMORY bit-base the index is a
                // signed bit offset into the bit string at m.addr: byte = addr+(idx>>3), bit = idx&7
                // (idx can exceed 31 - e.g. MSVC strcspn's 256-bit table indexed by a full char byte).
                // With a register bit-base the index is taken modulo the operand size.
                #define BT_SIDX() ((opsz==2)?(int32_t)(int16_t)getreg(m.reg,2):(int32_t)getreg(m.reg,4))
                #define BTNEW (g_newbt && m.is_mem)
                case 0xA3:{decode_modrm(&m,0); if(BTNEW){ int32_t bit=BT_SIDX(); uint8_t byte=rd8(m.addr+(uint32_t)(bit>>3)); setf(FL_CF,(byte>>(bit&7))&1); }
                    else { uint32_t bit=getreg(m.reg,opsz)&(opsz*8-1); uint32_t v=rm_read(&m,opsz); setf(FL_CF,(v>>bit)&1); } }break; // BT
                case 0xAB:{decode_modrm(&m,0); if(BTNEW){ int32_t bit=BT_SIDX(); uint32_t a=m.addr+(uint32_t)(bit>>3); int b=bit&7; uint8_t byte=rd8(a); setf(FL_CF,(byte>>b)&1); wr8(a,byte|(uint8_t)(1u<<b)); }
                    else { uint32_t bit=getreg(m.reg,opsz)&(opsz*8-1); uint32_t v=rm_read(&m,opsz); setf(FL_CF,(v>>bit)&1); rm_write(&m,opsz,v|(1u<<bit)); } }break; // BTS
                case 0xB3:{decode_modrm(&m,0); if(BTNEW){ int32_t bit=BT_SIDX(); uint32_t a=m.addr+(uint32_t)(bit>>3); int b=bit&7; uint8_t byte=rd8(a); setf(FL_CF,(byte>>b)&1); wr8(a,byte&(uint8_t)~(1u<<b)); }
                    else { uint32_t bit=getreg(m.reg,opsz)&(opsz*8-1); uint32_t v=rm_read(&m,opsz); setf(FL_CF,(v>>bit)&1); rm_write(&m,opsz,v&~(1u<<bit)); } }break; // BTR
                case 0xBB:{decode_modrm(&m,0); if(BTNEW){ int32_t bit=BT_SIDX(); uint32_t a=m.addr+(uint32_t)(bit>>3); int b=bit&7; uint8_t byte=rd8(a); setf(FL_CF,(byte>>b)&1); wr8(a,byte^(uint8_t)(1u<<b)); }
                    else { uint32_t bit=getreg(m.reg,opsz)&(opsz*8-1); uint32_t v=rm_read(&m,opsz); setf(FL_CF,(v>>bit)&1); rm_write(&m,opsz,v^(1u<<bit)); } }break; // BTC
                case 0xBA:{decode_modrm(&m,0);uint8_t imm=fetch8();uint32_t bit=imm&(opsz*8-1);uint32_t v=rm_read(&m,opsz);setf(FL_CF,(v>>bit)&1);
                    if(m.reg==5)rm_write(&m,opsz,v|(1u<<bit));else if(m.reg==6)rm_write(&m,opsz,v&~(1u<<bit));else if(m.reg==7)rm_write(&m,opsz,v^(1u<<bit));}break;
                case 0xBC:{decode_modrm(&m,0);uint32_t v=rm_read(&m,opsz);if(v==0){setf(FL_ZF,1);}else{setf(FL_ZF,0);uint32_t i=0;while(!((v>>i)&1))i++;setreg(m.reg,opsz,i);}}break; // BSF
                case 0xBD:{decode_modrm(&m,0);uint32_t v=rm_read(&m,opsz);if(v==0){setf(FL_ZF,1);}else{setf(FL_ZF,0);uint32_t i=opsz*8-1;while(!((v>>i)&1))i--;setreg(m.reg,opsz,i);}}break; // BSR
                case 0xA4: case 0xA5:{ decode_modrm(&m,0); uint32_t cnt=((op2==0xA4)?fetch8():(CPU.r[ECX]&0xff))&31;
                    uint32_t dst=rm_read(&m,opsz),src=getreg(m.reg,opsz); int bits=opsz*8;
                    if(cnt){ uint64_t lo=(opsz==2)?(src&0xffff):src; uint64_t wide=((uint64_t)dst<<bits)|lo; wide<<=cnt;
                        uint32_t res=(uint32_t)(wide>>bits)&SZMASK[opsz]; setf(FL_CF,(dst>>(bits-cnt))&1); rm_write(&m,opsz,res); set_szp(res,opsz); } }break; // SHLD
                case 0xAC: case 0xAD:{ decode_modrm(&m,0); uint32_t cnt=((op2==0xAC)?fetch8():(CPU.r[ECX]&0xff))&31;
                    uint32_t dst=rm_read(&m,opsz),src=getreg(m.reg,opsz); int bits=opsz*8;
                    if(cnt){ uint64_t hi=(opsz==2)?(src&0xffff):src; uint64_t dl=(opsz==2)?(dst&0xffff):dst; uint64_t wide=(hi<<bits)|dl; wide>>=cnt;
                        uint32_t res=(uint32_t)wide&SZMASK[opsz]; setf(FL_CF,(dst>>(cnt-1))&1); rm_write(&m,opsz,res); set_szp(res,opsz); } }break; // SHRD
                case 0xC0:{decode_modrm(&m,0);uint32_t a=rm_read(&m,1),b=getreg(m.reg,1);uint32_t s=do_add(a,b,1);setreg(m.reg,1,a);rm_write(&m,1,s);}break; // XADD r/m8
                case 0xC1:{decode_modrm(&m,0);uint32_t a=rm_read(&m,opsz),b=getreg(m.reg,opsz);uint32_t s=do_add(a,b,opsz);setreg(m.reg,opsz,a);rm_write(&m,opsz,s);}break; // XADD
                case 0xB0:{decode_modrm(&m,0);uint32_t a=rm_read(&m,1),acc=getreg(EAX,1);do_sub(acc,a,1);if(acc==a)rm_write(&m,1,getreg(m.reg,1));else setreg(EAX,1,a);}break; // CMPXCHG8
                case 0xB1:{decode_modrm(&m,0);uint32_t a=rm_read(&m,opsz),acc=getreg(EAX,opsz);do_sub(acc,a,opsz);if(acc==a)rm_write(&m,opsz,getreg(m.reg,opsz));else setreg(EAX,opsz,a);}break; // CMPXCHG
                case 0xC8:case 0xC9:case 0xCA:case 0xCB:case 0xCC:case 0xCD:case 0xCE:case 0xCF:{uint32_t v=CPU.r[op2-0xC8];CPU.r[op2-0xC8]=((v&0xff)<<24)|((v&0xff00)<<8)|((v>>8)&0xff00)|((v>>24)&0xff);}break; // BSWAP
                case 0x1F: decode_modrm(&m,0); break; // multi-byte NOP
                case 0x18:case 0x19:case 0x1A:case 0x1B:case 0x1C:case 0x1D:case 0x1E: decode_modrm(&m,0); break; // PREFETCH/NOP hints
                case 0x77: break; // EMMS
                case 0xAE: decode_modrm(&m,0); break; // FXSAVE/LFENCE/etc family - accept+ignore
                default: trap("0F-unknown",eip0); break;
            }
            } break;

        default: trap("opcode",eip0); break;
        }
    }
    return 0;
}

// extra ALU fns referenced by the macro
uint32_t do_logic_or (uint32_t a,uint32_t b,int sz){ return do_logic(a|b,sz); }
uint32_t do_logic_and(uint32_t a,uint32_t b,int sz){ return do_logic(a&b,sz); }
uint32_t do_logic_xor(uint32_t a,uint32_t b,int sz){ return do_logic(a^b,sz); }
