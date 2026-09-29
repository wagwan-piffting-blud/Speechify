/* x86 flag and ALU helpers, shared by the interpreter (cpu.c) and the
 * statically recompiled FE code (recomp_gen_*.c) so both execute the SAME
 * operation for every instruction. Moved verbatim out of cpu.c. */
#ifndef CPU_OPS_H
#define CPU_OPS_H
#include "emu.h"

uint32_t do_logic_or (uint32_t a,uint32_t b,int sz);
uint32_t do_logic_and(uint32_t a,uint32_t b,int sz);
uint32_t do_logic_xor(uint32_t a,uint32_t b,int sz);

static const uint32_t SZMASK[5]={0,0xff,0xffff,0,0xffffffff};
static const uint32_t SIGN[5]  ={0,0x80,0x8000,0,0x80000000};

static inline int parity8(uint32_t v){ v&=0xff; v^=v>>4; v^=v>>2; v^=v>>1; return (~v)&1; }
static inline void set_szp(uint32_t res,int sz){
    uint32_t m=SZMASK[sz]; res&=m;
    CPU.eflags &= ~(FL_ZF|FL_SF|FL_PF);
    if(res==0) CPU.eflags|=FL_ZF;
    if(res & SIGN[sz]) CPU.eflags|=FL_SF;
    if(parity8(res)) CPU.eflags|=FL_PF;
}
static inline void setf(uint32_t bit,int on){ if(on) CPU.eflags|=bit; else CPU.eflags&=~bit; }

static inline uint32_t do_add(uint32_t a,uint32_t b,int sz){
    uint64_t r=(uint64_t)(a&SZMASK[sz])+(b&SZMASK[sz]);
    uint32_t res=(uint32_t)r; set_szp(res,sz);
    setf(FL_CF, (r>>(sz*8))&1);
    setf(FL_AF, ((a^b^res)&0x10)!=0);
    setf(FL_OF, ((~(a^b)&(a^res))&SIGN[sz])!=0);
    return res&SZMASK[sz];
}
static inline uint32_t do_adc(uint32_t a,uint32_t b,int sz){
    uint32_t c=(CPU.eflags&FL_CF)?1:0;
    uint64_t r=(uint64_t)(a&SZMASK[sz])+(b&SZMASK[sz])+c;
    uint32_t res=(uint32_t)r; set_szp(res,sz);
    setf(FL_CF, (r>>(sz*8))&1);
    setf(FL_AF, ((a^b^res)&0x10)!=0);
    setf(FL_OF, ((~(a^b)&(a^res))&SIGN[sz])!=0);
    return res&SZMASK[sz];
}
static inline uint32_t do_sub(uint32_t a,uint32_t b,int sz){
    uint32_t aa=a&SZMASK[sz], bb=b&SZMASK[sz];
    uint32_t res=(aa-bb)&SZMASK[sz]; set_szp(res,sz);
    setf(FL_CF, aa<bb);
    setf(FL_AF, ((a^b^res)&0x10)!=0);
    setf(FL_OF, (((a^b)&(a^res))&SIGN[sz])!=0);
    return res;
}
static inline uint32_t do_sbb(uint32_t a,uint32_t b,int sz){
    uint32_t c=(CPU.eflags&FL_CF)?1:0;
    uint32_t aa=a&SZMASK[sz]; uint64_t bb=(uint64_t)(b&SZMASK[sz])+c;
    uint32_t res=(uint32_t)((aa-bb))&SZMASK[sz]; set_szp(res,sz);
    setf(FL_CF, (uint64_t)aa < bb);
    setf(FL_AF, ((a^b^res)&0x10)!=0);
    setf(FL_OF, (((a^b)&(a^res))&SIGN[sz])!=0);
    return res;
}
static inline uint32_t do_logic(uint32_t res,int sz){ set_szp(res,sz); setf(FL_CF,0); setf(FL_OF,0); setf(FL_AF,0); return res&SZMASK[sz]; }
static inline uint32_t do_inc(uint32_t a,int sz){ uint32_t res=(a+1)&SZMASK[sz]; int cf=CPU.eflags&FL_CF; set_szp(res,sz); setf(FL_AF,((a^1^res)&0x10)!=0); setf(FL_OF,(a&SZMASK[sz])==(SIGN[sz]-1)); setf(FL_CF,cf); return res; }
static inline uint32_t do_dec(uint32_t a,int sz){ uint32_t res=(a-1)&SZMASK[sz]; int cf=CPU.eflags&FL_CF; set_szp(res,sz); setf(FL_AF,((a^1^res)&0x10)!=0); setf(FL_OF,(a&SZMASK[sz])==SIGN[sz]); setf(FL_CF,cf); return res; }

static inline uint32_t getreg(int idx,int sz){
    if(sz==4) return CPU.r[idx];
    if(sz==2) return CPU.r[idx]&0xffff;
    if(idx<4) return CPU.r[idx]&0xff; return (CPU.r[idx-4]>>8)&0xff;
}
static inline void setreg(int idx,int sz,uint32_t v){
    if(sz==4){ CPU.r[idx]=v; return; }
    if(sz==2){ CPU.r[idx]=(CPU.r[idx]&~0xffffu)|(v&0xffff); return; }
    if(idx<4) CPU.r[idx]=(CPU.r[idx]&~0xffu)|(v&0xff); else CPU.r[idx-4]=(CPU.r[idx-4]&~0xff00u)|((v&0xff)<<8);
}

static inline int cond(int c){
    int cf=!!(CPU.eflags&FL_CF), zf=!!(CPU.eflags&FL_ZF), sf=!!(CPU.eflags&FL_SF), of=!!(CPU.eflags&FL_OF), pf=!!(CPU.eflags&FL_PF);
    switch(c&0xf){
        case 0x0:return of; case 0x1:return !of; case 0x2:return cf; case 0x3:return !cf;
        case 0x4:return zf; case 0x5:return !zf; case 0x6:return cf||zf; case 0x7:return !(cf||zf);
        case 0x8:return sf; case 0x9:return !sf; case 0xa:return pf; case 0xb:return !pf;
        case 0xc:return sf!=of; case 0xd:return sf==of; case 0xe:return zf||(sf!=of); case 0xf:return !(zf||(sf!=of));
    } return 0;
}

static inline uint32_t do_shift(int op,uint32_t v,uint32_t cnt,int sz){
    uint32_t m=SZMASK[sz]; v&=m; cnt &= 31; if(sz<4) {}
    if(cnt==0) return v;
    uint32_t res=v; int cf=0,of=0;
    switch(op){
        case 4: case 6:
            for(uint32_t i=0;i<cnt;i++){ cf=(res&SIGN[sz])?1:0; res=(res<<1)&m; }
            of=cf ^ ((res&SIGN[sz])?1:0); break;
        case 5:
            for(uint32_t i=0;i<cnt;i++){ cf=res&1; res>>=1; }
            of=(v&SIGN[sz])?1:0; break;
        case 7:
            { int neg=(v&SIGN[sz])?1:0;
              for(uint32_t i=0;i<cnt;i++){ cf=res&1; res>>=1; if(neg) res|=SIGN[sz]; }
              of=0; } break;
        case 0:
            for(uint32_t i=0;i<cnt;i++){ cf=(res&SIGN[sz])?1:0; res=((res<<1)|cf)&m; }
            of=cf ^ ((res&SIGN[sz])?1:0); break;
        case 1:
            for(uint32_t i=0;i<cnt;i++){ cf=res&1; res=((res>>1)|((uint32_t)cf*SIGN[sz]))&m; }
            of=((res&SIGN[sz])?1:0) ^ (((res<<1)&SIGN[sz])?1:0); break;
        case 2:
            for(uint32_t i=0;i<cnt;i++){ int nc=(res&SIGN[sz])?1:0; res=((res<<1)|(CPU.eflags&FL_CF?1:0))&m; setf(FL_CF,nc); cf=nc; }
            of=cf ^ ((res&SIGN[sz])?1:0); break;
        case 3:
            for(uint32_t i=0;i<cnt;i++){ int nc=res&1; res=(res>>1)|((CPU.eflags&FL_CF?1u:0u)*SIGN[sz]); res&=m; setf(FL_CF,nc); cf=nc; }
            break;
    }
    if(op>=4) set_szp(res,sz);
    if(op<2||op>3) setf(FL_CF,cf);
    setf(FL_OF,of);
    return res&m;
}

#endif
