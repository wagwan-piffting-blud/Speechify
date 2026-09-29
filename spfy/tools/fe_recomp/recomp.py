"""Static recompiler: SWIttsFe-en-US.dll x86 -> C for host_emu.

Each selected guest function becomes `int rc_XXXXXXXX(void)` operating on the
interpreter's own CPU struct through the interpreter's own helpers
(cpu_ops.h: do_add, set_szp, cond ...), so a translated instruction does
exactly what cpu.c does for it. Anything not translated is SINGLE-STEPPED by
the interpreter, so coverage is complete and output byte-identical by
construction. On top: ALU ops whose flags are provably dead use flag-free
NF_* variants (eliminate_dead_flags).

Inputs: data/ghidra_export.jsonl.gz (FeRecompExport.java), data/fnset.json
(entry VAs: sampled hot set + every call target the interpreter executed over
the 1,349 wayback transcripts, data/trace_corpus.py) and bin/SWIttsFe-en-US.dll.
Output: spfy/src/host_emu/recomp/{recomp_gen_*.c,recomp_tab.c,recomp_protos.h}.

    python recomp.py
Gates after regenerating: master parity 221/221 and data/equiv_corpus.py
(SPFY_FE_RECOMP=0 vs 1, all 1,349 transcripts byte-identical).
"""
from __future__ import annotations

import argparse
import gzip
import json
import re
import struct
import sys
from pathlib import Path

import pefile

REPO = Path(__file__).resolve().parents[3]
DLL = REPO / "bin" / "SWIttsFe-en-US.dll"
OUT = REPO / "spfy" / "src" / "host_emu" / "recomp"
PREFIXES = {0x66, 0x67, 0xF0, 0xF2, 0xF3, 0x2E, 0x36, 0x3E, 0x26, 0x64, 0x65}
ALU = {0: "do_add", 1: "do_logic_or", 2: "do_adc", 3: "do_sbb", 4: "do_logic_and", 5: "do_sub", 6: "do_logic_xor"}


class Image:
    def __init__(self, path):
        pe = pefile.PE(str(path), fast_load=True)
        self.base = pe.OPTIONAL_HEADER.ImageBase
        self.img = pe.get_memory_mapped_image()

    def b(self, va):
        return self.img[va - self.base]

    def u16(self, va):
        return struct.unpack_from("<H", self.img, va - self.base)[0]

    def u32(self, va):
        return struct.unpack_from("<I", self.img, va - self.base)[0]

    def s8(self, va):
        return struct.unpack_from("<b", self.img, va - self.base)[0]

    def s32(self, va):
        return struct.unpack_from("<i", self.img, va - self.base)[0]


def h32(v):
    return f"0x{v & 0xFFFFFFFF:08x}u"


class Insn:
    """Decode one instruction exactly the way cpu.c's cpu_run does."""

    def __init__(self, im: Image, va: int):
        self.va = va
        p = va
        self.opsz = 4
        self.seg = None
        self.odd_prefix = False
        while im.b(p) in PREFIXES:
            x = im.b(p)
            if x == 0x66:
                self.opsz = 2
            elif x == 0x64:
                self.seg = "CPU.seg_fs_base"
            elif x == 0x65:
                self.seg = "CPU.seg_gs_base"
            else:
                self.odd_prefix = True
            p += 1
        self.op = im.b(p)
        p += 1
        self.op2 = None
        if self.op == 0x0F:
            self.op2 = im.b(p)
            p += 1
        self.im = im
        self.p = p
        self.m = None

    def modrm(self):
        im, p = self.im, self.p
        b = im.b(p)
        p += 1
        mod, reg, rm = b >> 6, (b >> 3) & 7, b & 7
        if mod == 3:
            self.m = {"mem": False, "reg": reg, "rm": rm}
            self.p = p
            return self.m
        parts = []
        if rm == 4:
            sib = im.b(p)
            p += 1
            scale, idx, base = sib >> 6, (sib >> 3) & 7, sib & 7
            if base == 5 and mod == 0:
                parts.append(h32(im.u32(p)))
                p += 4
            else:
                parts.append(f"CPU.r[{base}]")
            if idx != 4:
                parts.append(f"(CPU.r[{idx}]<<{scale})" if scale else f"CPU.r[{idx}]")
        elif rm == 5 and mod == 0:
            parts.append(h32(im.u32(p)))
            p += 4
        else:
            parts.append(f"CPU.r[{rm}]")
        if mod == 1:
            parts.append(h32(im.s8(p)))
            p += 1
        elif mod == 2:
            parts.append(h32(im.s32(p)))
            p += 4
        if self.seg:
            parts.append(self.seg)
        self.m = {"mem": True, "reg": reg, "rm": rm, "addr": "(uint32_t)(" + " + ".join(parts) + ")"}
        self.p = p
        return self.m

    def imm8(self):
        v = self.im.b(self.p)
        self.p += 1
        return v

    def simm8(self):
        v = self.im.s8(self.p)
        self.p += 1
        return v

    def imm16(self):
        v = self.im.u16(self.p)
        self.p += 2
        return v

    def imm32(self):
        v = self.im.u32(self.p)
        self.p += 4
        return v

    def simm32(self):
        v = self.im.s32(self.p)
        self.p += 4
        return v

    def immz(self):
        return self.imm16() if self.opsz == 2 else self.imm32()


def rm_read(m, sz):
    if m["mem"]:
        return {1: "rd8(ea)", 2: "rd16(ea)", 4: "rd32(ea)"}[sz]
    return f"getreg({m['rm']},{sz})"


def rm_write(m, sz, v):
    if m["mem"]:
        return {1: f"wr8(ea,(uint8_t)({v}));", 2: f"wr16(ea,(uint16_t)({v}));", 4: f"wr32(ea,{v});"}[sz]
    return f"setreg({m['rm']},{sz},{v});"


def ea_decl(m):
    return f"ea={m['addr']}; " if m["mem"] else ""


class Fn:
    def __init__(self, rec, im, entries):
        self.rec = rec
        self.im = im
        self.entry = int(rec["fn"], 16)
        self.entries = entries
        self.lines = []
        self.labels = set()
        self.insns = []
        self.computed = {int(c["at"], 16): [int(t, 16) for t in c["targets"]] for c in rec["computed"]}
        self.stats = {"native": 0, "step": 0}

    def decode(self):
        seen = set()
        for blk in self.rec["blocks"]:
            s, e = int(blk["start"], 16), int(blk["end"], 16)
            self.labels.add(s)
            va = s
            while va <= e:
                if va in seen:
                    break
                seen.add(va)
                ins = Insn(self.im, va)
                self.insns.append(ins)
                va = self.measure(ins)
        self.insns.sort(key=lambda i: i.va)
        self.addrs = {i.va for i in self.insns}
        for tgts in self.computed.values():
            self.labels.update(t for t in tgts if t in self.addrs)
        self.labels &= self.addrs

    def measure(self, ins):
        """Return next VA after `ins`, using a throwaway translation pass."""
        saved = (ins.p, ins.m)
        self.translate(ins, dry=True)
        nxt = ins.p
        ins.p, ins.m = saved
        return nxt

    def emit(self, s):
        self.lines.append(s)

    def target(self, t, how):
        """C for control transfer to VA t (jmp/jcc)."""
        if t in self.labels:
            return f"goto L_{t:08x};"
        if t in self.entries:
            return f"{{ CPU.eip={h32(t)}; return rc_{t:08x}(); }}"
        return f"{{ CPU.eip={h32(t)}; return RC_EXIT; }}"

    def step(self, ins, nxt):
        return f"CPU.eip={h32(ins.va)}; if(!rc_step({h32(nxt)})) return RC_EXIT;"

    def call_seq(self, ret, tgt_expr, direct=None):
        s = f"cpu_push32({h32(ret)}); CPU.eip={tgt_expr}; "
        if direct is not None and direct in self.entries:
            s += f"rc=rc_{direct:08x}(); "
        else:
            s += "rc=rc_dispatch(CPU.eip); "
        s += f"if(rc==RC_ABANDON) return RC_ABANDON; if(!(CPU.eip=={h32(ret)} && CPU.r[4]>=spc)){{ if(!rc_run_until({h32(ret)},spc)) return RC_ABANDON; }}"
        # spc = ESP before the push: the callee has returned once EIP is back
        # at `ret` with ESP at or above it (stdcall pops more; a recursive
        # instance returning to the same site sits deeper).
        return "{ uint32_t spc=CPU.r[4]; int rc; " + s + " }"

    def translate(self, ins: Insn, dry=False):
        """Return C for ins (and leave ins.p at the next instruction)."""
        op, sz = ins.op, ins.opsz
        if ins.odd_prefix:
            return self._step_generic(ins, dry)
        try:
            c = self._translate(ins, op, sz, dry)
        except _Step:
            ins.p, ins.m = ins.p, None
            return self._step_generic(ins, dry)
        if c is None:
            return self._step_generic(ins, dry)
        return c

    def _step_generic(self, ins, dry):
        """Length via capstone-free rule: re-decode generically."""
        n = generic_length(self.im, ins.va)
        ins.p = ins.va + n
        ins.stepped = True
        if dry:
            return ""
        self.stats["step"] += 1
        return self.step(ins, ins.va + n)

    def _translate(self, ins, op, sz, dry):
        n = self.stats
        if ins.op2 is None:
            if op <= 0x3D and (op & 7) <= 5 and (op >> 3) <= 7:
                grp, form = op >> 3, op & 7
                if form <= 3:
                    m = ins.modrm()
                    s = 1 if form in (0, 2) else sz
                    if grp == 7:
                        a, b = (rm_read(m, s), f"getreg({m['reg']},{s})") if form in (0, 1) else (f"getreg({m['reg']},{s})", rm_read(m, s))
                        c = ea_decl(m) + f"do_sub({a},{b},{s});"
                    elif form in (0, 1):
                        c = ea_decl(m) + f"{{ uint32_t a={rm_read(m, s)}, b=getreg({m['reg']},{s}); {rm_write(m, s, f'{ALU[grp]}(a,b,{s})')} }}"
                    else:
                        c = ea_decl(m) + f"{{ uint32_t a=getreg({m['reg']},{s}), b={rm_read(m, s)}; setreg({m['reg']},{s},{ALU[grp]}(a,b,{s})); }}"
                else:
                    s = 1 if form == 4 else sz
                    v = ins.imm8() if form == 4 else ins.immz()
                    if grp == 7:
                        c = f"do_sub(getreg(0,{s}),{h32(v)},{s});"
                    else:
                        c = f"setreg(0,{s},{ALU[grp]}(getreg(0,{s}),{h32(v)},{s}));"
                n["native"] += 1
                return c
            if 0x40 <= op <= 0x4F:
                r = op & 7
                fn = "do_inc" if op < 0x48 else "do_dec"
                n["native"] += 1
                return f"setreg({r},{sz},{fn}(getreg({r},{sz}),{sz}));"
            if 0x50 <= op <= 0x57:
                n["native"] += 1
                return f"cpu_push32(CPU.r[{op - 0x50}]);"
            if 0x58 <= op <= 0x5F:
                n["native"] += 1
                return f"CPU.r[{op - 0x58}]=cpu_pop32();"
            if op == 0x68:
                n["native"] += 1
                return f"cpu_push32({h32(ins.imm32())});"
            if op == 0x6A:
                n["native"] += 1
                return f"cpu_push32({h32(ins.simm8())});"
            if op in (0x69, 0x6B):
                m = ins.modrm()
                b = ins.simm8() if op == 0x6B else (struct.unpack("<h", struct.pack("<H", ins.imm16()))[0] if sz == 2 else ins.simm32())
                n["native"] += 1
                return ea_decl(m) + (f"{{ int32_t a=(int32_t){rm_read(m, sz)}; int32_t b=(int32_t){h32(b)}; int64_t r=(int64_t)a*b; "
                                     f"setreg({m['reg']},{sz},(uint32_t)r); setf(FL_CF,(int64_t)(int32_t)r!=r); setf(FL_OF,(int64_t)(int32_t)r!=r); }}")
            if 0x70 <= op <= 0x7F:
                d = ins.simm8()
                t = (ins.p + d) & 0xFFFFFFFF
                n["native"] += 1
                return f"if(cond({op - 0x70})) {self.target(t, 'jcc')}"
            if op in (0x80, 0x81, 0x83):
                m = ins.modrm()
                s = 1 if op == 0x80 else sz
                b = ins.imm8() if op == 0x80 else (ins.immz() if op == 0x81 else ins.simm8())
                g = m["reg"]
                n["native"] += 1
                if g == 7:
                    return ea_decl(m) + f"do_sub({rm_read(m, s)},{h32(b)},{s});"
                return ea_decl(m) + f"{{ uint32_t a={rm_read(m, s)}; {rm_write(m, s, f'{ALU[g]}(a,{h32(b)},{s})')} }}"
            if op in (0x84, 0x85):
                m = ins.modrm()
                s = 1 if op == 0x84 else sz
                n["native"] += 1
                return ea_decl(m) + f"do_logic({rm_read(m, s)}&getreg({m['reg']},{s}),{s});"
            if op in (0x86, 0x87):
                m = ins.modrm()
                s = 1 if op == 0x86 else sz
                n["native"] += 1
                return ea_decl(m) + f"{{ uint32_t a={rm_read(m, s)}, b=getreg({m['reg']},{s}); {rm_write(m, s, 'b')} setreg({m['reg']},{s},a); }}"
            if op in (0x88, 0x89):
                m = ins.modrm()
                s = 1 if op == 0x88 else sz
                n["native"] += 1
                return ea_decl(m) + rm_write(m, s, f"getreg({m['reg']},{s})")
            if op in (0x8A, 0x8B):
                m = ins.modrm()
                s = 1 if op == 0x8A else sz
                n["native"] += 1
                return ea_decl(m) + f"setreg({m['reg']},{s},{rm_read(m, s)});"
            if op == 0x8D:
                m = ins.modrm()
                if not m["mem"]:
                    raise _Step
                n["native"] += 1
                return f"setreg({m['reg']},{sz},{m['addr']});"
            if op == 0x90 and ins.va + 1 == ins.p:
                n["native"] += 1
                return ";"
            if op == 0x98:
                n["native"] += 1
                return ("CPU.r[0]=(int32_t)(int16_t)(CPU.r[0]&0xffff);" if sz == 4 else
                        "CPU.r[0]=(CPU.r[0]&~0xffffu)|((uint32_t)(int16_t)(int8_t)(CPU.r[0]&0xff)&0xffff);")
            if op == 0x99:
                n["native"] += 1
                return ("CPU.r[2]=(CPU.r[0]&0x80000000)?0xffffffff:0;" if sz == 4 else
                        "setreg(2,2,(getreg(0,2)&0x8000)?0xffff:0);")
            if op in (0xA0, 0xA1, 0xA2, 0xA3):
                a = h32(ins.imm32()) + (f"+{ins.seg}" if ins.seg else "")
                n["native"] += 1
                if op == 0xA0:
                    return f"setreg(0,1,rd8({a}));"
                if op == 0xA1:
                    return f"setreg(0,{sz},{'rd16' if sz == 2 else 'rd32'}({a}));"
                if op == 0xA2:
                    return f"wr8({a},(uint8_t)getreg(0,1));"
                return f"wr16({a},(uint16_t)getreg(0,2));" if sz == 2 else f"wr32({a},getreg(0,4));"
            if op in (0xA8, 0xA9):
                s = 1 if op == 0xA8 else sz
                v = ins.imm8() if op == 0xA8 else ins.immz()
                n["native"] += 1
                return f"do_logic(getreg(0,{s})&{h32(v)},{s});"
            if 0xB0 <= op <= 0xB7:
                n["native"] += 1
                return f"setreg({op - 0xB0},1,{h32(ins.imm8())});"
            if 0xB8 <= op <= 0xBF:
                n["native"] += 1
                return f"setreg({op - 0xB8},{sz},{h32(ins.immz())});"
            if op in (0xC0, 0xC1, 0xD0, 0xD1, 0xD2, 0xD3):
                m = ins.modrm()
                s = 1 if op in (0xC0, 0xD0, 0xD2) else sz
                cnt = str(ins.imm8()) if op in (0xC0, 0xC1) else ("1" if op in (0xD0, 0xD1) else "(CPU.r[1]&0xff)")
                n["native"] += 1
                return ea_decl(m) + f"{{ uint32_t v={rm_read(m, s)}; {rm_write(m, s, f'do_shift({m['reg']},v,{cnt},{s})')} }}"
            if op == 0xC3:
                n["native"] += 1
                return "CPU.eip=cpu_pop32(); return RC_OK;"
            if op == 0xC2:
                imm = ins.imm16()
                n["native"] += 1
                return f"{{ uint32_t ra=cpu_pop32(); CPU.r[4]+={imm}u; CPU.eip=ra; return RC_OK; }}"
            if op in (0xC6, 0xC7):
                m = ins.modrm()
                s = 1 if op == 0xC6 else sz
                v = ins.imm8() if op == 0xC6 else ins.immz()
                n["native"] += 1
                return ea_decl(m) + rm_write(m, s, h32(v))
            if op == 0xC9:
                n["native"] += 1
                return "CPU.r[4]=CPU.r[5]; CPU.r[5]=cpu_pop32();"
            if op == 0xE8:
                d = ins.simm32()
                ret = ins.p
                t = (ret + d) & 0xFFFFFFFF
                n["native"] += 1
                if t == ret:
                    return f"cpu_push32({h32(ret)});"
                return self.call_seq(ret, h32(t), direct=t)
            if op in (0xE9, 0xEB):
                d = ins.simm32() if op == 0xE9 else ins.simm8()
                t = (ins.p + d) & 0xFFFFFFFF
                n["native"] += 1
                return self.target(t, "jmp")
            if op in (0xF6, 0xF7):
                m = ins.modrm()
                s = 1 if op == 0xF6 else sz
                g = m["reg"]
                if g in (0, 1):
                    v = ins.imm8() if op == 0xF6 else ins.immz()
                    n["native"] += 1
                    return ea_decl(m) + f"do_logic({rm_read(m, s)}&{h32(v)},{s});"
                if g == 2:
                    n["native"] += 1
                    return ea_decl(m) + f"{{ uint32_t a={rm_read(m, s)}; {rm_write(m, s, '~a')} }}"
                if g == 3:
                    n["native"] += 1
                    return ea_decl(m) + f"{{ uint32_t a={rm_read(m, s)}; {rm_write(m, s, f'do_sub(0,a,{s})')} }}"
                if g == 4 and s == 4:
                    n["native"] += 1
                    return ea_decl(m) + (f"{{ uint32_t a={rm_read(m, 4)}; uint64_t r=(uint64_t)CPU.r[0]*a; CPU.r[0]=(uint32_t)r; "
                                         f"CPU.r[2]=(uint32_t)(r>>32); setf(FL_CF,CPU.r[2]!=0); setf(FL_OF,CPU.r[2]!=0); }}")
                if g == 5 and s == 4:
                    n["native"] += 1
                    return ea_decl(m) + (f"{{ uint32_t a={rm_read(m, 4)}; int64_t r=(int64_t)(int32_t)CPU.r[0]*(int32_t)a; CPU.r[0]=(uint32_t)r; "
                                         f"CPU.r[2]=(uint32_t)((uint64_t)r>>32); setf(FL_CF,(int64_t)(int32_t)r!=r); setf(FL_OF,(int64_t)(int32_t)r!=r); }}")
                raise _Step
            if op == 0xFE:
                m = ins.modrm()
                if m["reg"] > 1:
                    raise _Step
                fn = "do_inc" if m["reg"] == 0 else "do_dec"
                n["native"] += 1
                return ea_decl(m) + f"{{ uint32_t a={rm_read(m, 1)}; {rm_write(m, 1, f'{fn}(a,1)')} }}"
            if op == 0xFF:
                m = ins.modrm()
                g = m["reg"]
                if g in (0, 1):
                    fn = "do_inc" if g == 0 else "do_dec"
                    n["native"] += 1
                    return ea_decl(m) + f"{{ uint32_t a={rm_read(m, sz)}; {rm_write(m, sz, f'{fn}(a,{sz})')} }}"
                if g == 2 and sz == 4:
                    n["native"] += 1
                    return ea_decl(m) + f"{{ uint32_t t={rm_read(m, 4)}; " + self.call_seq(ins.p, "t") + " }"
                if g == 4 and sz == 4:
                    n["native"] += 1
                    tg = [t for t in self.computed.get(ins.va, []) if t in self.labels]
                    sw = "".join(f"case {h32(t)}: goto L_{t:08x}; " for t in sorted(set(tg)))
                    return ea_decl(m) + f"{{ uint32_t t={rm_read(m, 4)}; switch(t){{ {sw}default: CPU.eip=t; return rc_tail(t); }} }}"
                if g == 6 and sz == 4:
                    n["native"] += 1
                    return ea_decl(m) + f"cpu_push32({rm_read(m, 4)});"
                raise _Step
            return None
        op2 = ins.op2
        if 0x80 <= op2 <= 0x8F:
            d = (struct.unpack("<h", struct.pack("<H", ins.imm16()))[0]) if sz == 2 else ins.simm32()
            t = (ins.p + d) & 0xFFFFFFFF
            n["native"] += 1
            return f"if(cond({op2 - 0x80})) {self.target(t, 'jcc')}"
        if 0x90 <= op2 <= 0x9F:
            m = ins.modrm()
            n["native"] += 1
            return ea_decl(m) + rm_write(m, 1, f"cond({op2 - 0x90})?1:0")
        if 0x40 <= op2 <= 0x4F:
            m = ins.modrm()
            n["native"] += 1
            return ea_decl(m) + f"{{ uint32_t v={rm_read(m, sz)}; if(cond({op2 - 0x40})) setreg({m['reg']},{sz},v); }}"
        if op2 in (0xB6, 0xB7, 0xBE, 0xBF):
            m = ins.modrm()
            src = rm_read(m, 1 if op2 in (0xB6, 0xBE) else 2)
            v = {0xB6: f"{src}&0xff", 0xB7: f"{src}&0xffff",
                 0xBE: f"(uint32_t)(int32_t)(int8_t){src}", 0xBF: f"(uint32_t)(int32_t)(int16_t){src}"}[op2]
            n["native"] += 1
            return ea_decl(m) + f"setreg({m['reg']},{sz},{v});"
        if op2 == 0xAF:
            m = ins.modrm()
            n["native"] += 1
            return ea_decl(m) + (f"{{ int32_t a=(int32_t)getreg({m['reg']},{sz}), b=(int32_t){rm_read(m, sz)}; int64_t r=(int64_t)a*b; "
                                 f"setreg({m['reg']},{sz},(uint32_t)r); setf(FL_CF,(int64_t)(int32_t)r!=r); setf(FL_OF,(int64_t)(int32_t)r!=r); }}")
        return None

    def generate(self):
        self.decode()
        self.stats = {"native": 0, "step": 0}
        name = f"rc_{self.entry:08x}"
        self.emit(f"static int {name}(void){{")
        self.emit("    uint32_t ea; (void)ea;")
        if self.entry not in self.addrs:
            self.emit("    return RC_EXIT;\n}")
            return
        order = sorted(self.insns, key=lambda i: i.va)
        if order[0].va != self.entry:
            self.labels.add(self.entry)
        recs = []
        for ins in order:
            ins2 = Insn(self.im, ins.va)
            ins2.stepped = False
            c = self.translate(ins2)
            recs.append([ins.va, c, ins2.p, ins2])
        self.stats["nf"] = eliminate_dead_flags(self.im, recs)
        body = []
        prev_end = None
        first = True
        for va, c, nxt, _ in recs:
            if first and va != self.entry:
                body.append(f"    goto L_{self.entry:08x};")
            first = False
            if prev_end is not None and va != prev_end:
                body.append(f"    CPU.eip={h32(prev_end)}; return RC_EXIT;")
            lab = f"@@{va:08x}@@" if va in self.labels else ""
            body.append(f"    {lab}{c}")
            prev_end = nxt
        body.append(f"    CPU.eip={h32(prev_end)}; return RC_EXIT;")
        text = "\n".join(body)
        used = set(re.findall(r"goto L_([0-9a-f]{8});", text))
        text = re.sub(r"@@([0-9a-f]{8})@@",
                      lambda mo: f"L_{mo.group(1)}: if(CPU.halted) return RC_ABANDON; " if mo.group(1) in used else "",
                      text)
        self.lines.append(text)
        self.emit("}")


class _Step(Exception):
    pass


# ---- dead-flag elimination ------------------------------------------------
# EFLAGS bits as cpu.c uses them. An ALU op whose written flags are all dead
# (overwritten before any read on every path) gets the NF_* variant, which
# returns the same value without touching CPU.eflags. Anything that leaves
# the function -- ret, call, bail, step, jump out -- counts as reading ALL
# flags, except call and ret (see flag_rw), so only flags provably
# overwritten are dropped.
CF, PF, AF, ZF, SF, OF = 0x1, 0x4, 0x10, 0x40, 0x80, 0x800
ALLF = CF | PF | AF | ZF | SF | OF
COND_R = {0: OF, 1: OF, 2: CF, 3: CF, 4: ZF, 5: ZF, 6: CF | ZF, 7: CF | ZF, 8: SF, 9: SF,
          10: PF, 11: PF, 12: SF | OF, 13: SF | OF, 14: ZF | SF | OF, 15: ZF | SF | OF}
NF_RE = re.compile(r"\bdo_(add|adc|sub|sbb|inc|dec|logic_or|logic_and|logic_xor|logic)\(")
SETF_RE = re.compile(r"setf\(FL_(?:CF|OF),[^;]*\);")


def flag_rw(im, ins):
    """(flags read, flags written, rewritable) for a translated instruction,
    mirroring cpu.c's semantics for each opcode."""
    if getattr(ins, "stepped", False) or ins.odd_prefix:
        return ALLF, 0, False
    op = ins.op
    d = Insn(im, ins.va)
    if d.op2 is None:
        if op <= 0x3D and (op & 7) <= 5:
            return (CF if (op >> 3) in (2, 3) else 0), ALLF, True
        if 0x40 <= op <= 0x4F:
            return 0, ALLF & ~CF, True
        if op in (0x69, 0x6B):
            return 0, CF | OF, True
        if 0x70 <= op <= 0x7F:
            return COND_R[op - 0x70], 0, False
        if op in (0x80, 0x81, 0x83):
            g = d.modrm()["reg"]
            return (CF if g in (2, 3) else 0), ALLF, True
        if op in (0x84, 0x85, 0xA8, 0xA9):
            return 0, ALLF, True
        if op in (0xC0, 0xC1, 0xD0, 0xD1):
            g = d.modrm()["reg"]
            cnt = (d.imm8() if op in (0xC0, 0xC1) else 1) & 31
            if cnt == 0:
                return 0, 0, False
            if g >= 4:
                return 0, CF | OF | PF | ZF | SF, False
            if g in (0, 1):
                return 0, CF | OF, False
            return CF, CF | OF, False
        if op in (0xD2, 0xD3):
            return ALLF, 0, False
        if op in (0xF6, 0xF7):
            g = d.modrm()["reg"]
            if g in (0, 1, 3):
                return 0, ALLF, True
            if g in (4, 5):
                return 0, CF | OF, True
            return 0, 0, False
        if op in (0xFE, 0xFF):
            g = d.modrm()["reg"]
            if g in (0, 1):
                return 0, ALLF & ~CF, True
            if g == 2:
                return 0, ALLF, False
            if g == 4:
                return ALLF, 0, False
            return 0, 0, False
        if op in (0xE8, 0xC2, 0xC3):
            # MSVC ABI: flags are never passed into a callee or back out of
            # one, so call and ret CLOBBER them. Verified, not assumed: the
            # 1,349-transcript on/off comparison is byte-identical with this.
            return 0, ALLF, False
        return 0, 0, False
    op2 = d.op2
    if 0x80 <= op2 <= 0x8F:
        return COND_R[op2 - 0x80], 0, False
    if 0x90 <= op2 <= 0x9F:
        return COND_R[op2 - 0x90], 0, False
    if 0x40 <= op2 <= 0x4F:
        return COND_R[op2 - 0x40], 0, False
    if op2 == 0xAF:
        return 0, CF | OF, True
    return 0, 0, False


def eliminate_dead_flags(im, recs):
    """recs: [va, code, next_va, ins]. Rewrites code in place; returns count."""
    idx = {r[0]: i for i, r in enumerate(recs)}
    n = len(recs)
    rw = [flag_rw(im, r[3]) for r in recs]
    succ = []
    for i, (va, c, nxt, ins) in enumerate(recs):
        s = [idx[int(t, 16)] for t in re.findall(r"goto L_([0-9a-f]{8});", c) if int(t, 16) in idx]
        op = ins.op if ins.op2 is None else None
        ends = op in (0xE9, 0xEB, 0xC2, 0xC3) or (op == 0xFF and not getattr(ins, "stepped", False)
                                                  and Insn(im, va).modrm()["reg"] == 4)
        if not ends and nxt in idx:
            s.append(idx[nxt])
        exits = "return" in c or (not ends and nxt not in idx)
        succ.append((s, exits))
    live_in = [0] * n
    changed = True
    while changed:
        changed = False
        for i in range(n - 1, -1, -1):
            s, exits = succ[i]
            out = ALLF if exits else 0
            for j in s:
                out |= live_in[j]
            r, w, _ = rw[i]
            li = r | (out & ~w)
            if li != live_in[i]:
                live_in[i] = li
                changed = True
    count = 0
    for i, rec in enumerate(recs):
        r, w, ok = rw[i]
        s, exits = succ[i]
        out = ALLF if exits else 0
        for j in s:
            out |= live_in[j]
        if ok and w and not (w & out):
            rec[1] = SETF_RE.sub("", NF_RE.sub(r"NF_\1(", rec[1]))
            count += 1
    return count


_CS = None


def generic_length(im, va):
    global _CS
    if _CS is None:
        import capstone
        _CS = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    for i in _CS.disasm(bytes(im.img[va - im.base:va - im.base + 16]), va):
        return i.size
    raise SystemExit(f"capstone cannot decode {va:08x}")


def main():
    ap = argparse.ArgumentParser()
    here = Path(__file__).resolve().parent / "data"
    ap.add_argument("--export", default=str(here / "ghidra_export.jsonl.gz"))
    ap.add_argument("--functions", default=str(here / "fnset.json"),
                    help="JSON list of entry VAs to translate; '' = every function")
    ap.add_argument("--out", default=str(OUT))
    # A 32 KB rule function becomes one C function with ~1000 labels, and the
    # 32-bit cc1 runs out of memory on a few of those at -O3. Skipped
    # functions simply stay interpreted.
    ap.add_argument("--max-bytes", type=int, default=8192)
    ap.add_argument("--include-big", action="store_true")
    args = ap.parse_args()
    im = Image(DLL)
    opener = gzip.open if args.export.endswith(".gz") else open
    with opener(args.export, "rt", encoding="utf-8") as fh:
        recs = [json.loads(l) for l in fh]
    if args.functions:
        want = set(json.load(open(args.functions)))
        recs = [r for r in recs if r["fn"] in want]
    size = lambda r: sum(int(b, 16) - int(a, 16) + 1 for a, b in r["ranges"])
    if not args.include_big:
        n0 = len(recs)
        recs = [r for r in recs if size(r) <= args.max_bytes]
        print(f"left {n0 - len(recs)} functions over {args.max_bytes} bytes interpreted (--include-big to translate)")
    entries = {int(r["fn"], 16) for r in recs}
    out_dir = Path(args.out)
    out_dir.mkdir(parents=True, exist_ok=True)
    for old in out_dir.glob("recomp_*.c"):
        old.unlink()
    (out_dir / "recomp_protos.h").write_text(
        "/* GENERATED by spfy/tools/fe_recomp/recomp.py - do not edit. */\n#include <stdint.h>\n" +
        "".join(f"int rc_{e:08x}(void);\n" for e in sorted(entries)), encoding="ascii")
    prologue = ("/* GENERATED by spfy/tools/fe_recomp/recomp.py - do not edit. */\n"
                "#include \"emu.h\"\n#include \"emu_fast_mem.h\"\n#include \"cpu_ops.h\"\n"
                "#include \"recomp.h\"\n#include \"recomp_protos.h\"\n"
                "/* Exact copies of cpu.c's cpu_push32/cpu_pop32, inlined per shard. */\n"
                "static inline void rc_push32(uint32_t v){ CPU.r[ESP]-=4; wr32(CPU.r[ESP], v); }\n"
                "static inline uint32_t rc_pop32(void){ uint32_t v=rd32(CPU.r[ESP]); CPU.r[ESP]+=4; return v; }\n"
                "/* Same result as cpu_ops.h's do_* for ops whose flags are dead. */\n"
                "static inline uint32_t NF_add(uint32_t a,uint32_t b,int sz){ return (a+b)&SZMASK[sz]; }\n"
                "static inline uint32_t NF_adc(uint32_t a,uint32_t b,int sz){ return (a+b+((CPU.eflags&FL_CF)?1u:0u))&SZMASK[sz]; }\n"
                "static inline uint32_t NF_sub(uint32_t a,uint32_t b,int sz){ return (a-b)&SZMASK[sz]; }\n"
                "static inline uint32_t NF_sbb(uint32_t a,uint32_t b,int sz){ return (a-b-((CPU.eflags&FL_CF)?1u:0u))&SZMASK[sz]; }\n"
                "static inline uint32_t NF_logic(uint32_t r,int sz){ return r&SZMASK[sz]; }\n"
                "static inline uint32_t NF_logic_or(uint32_t a,uint32_t b,int sz){ return (a|b)&SZMASK[sz]; }\n"
                "static inline uint32_t NF_logic_and(uint32_t a,uint32_t b,int sz){ return (a&b)&SZMASK[sz]; }\n"
                "static inline uint32_t NF_logic_xor(uint32_t a,uint32_t b,int sz){ return (a^b)&SZMASK[sz]; }\n"
                "static inline uint32_t NF_inc(uint32_t a,int sz){ return (a+1)&SZMASK[sz]; }\n"
                "static inline uint32_t NF_dec(uint32_t a,int sz){ return (a-1)&SZMASK[sz]; }\n")
    tot = {"native": 0, "step": 0, "nf": 0}
    shard, shard_bytes, n_shard, n_big = [], 0, 0, 0

    def flush(name):
        text = "\n".join(shard).replace("cpu_push32(", "rc_push32(").replace("cpu_pop32()", "rc_pop32()")
        text = text.replace("static int rc_", "int rc_")
        (out_dir / name).write_text(prologue + text + "\n", encoding="ascii")

    for r in recs:
        f = Fn(r, im, entries)
        f.generate()
        for k in tot:
            tot[k] += f.stats[k]
        if size(r) > args.max_bytes:
            shard_save = shard
            shard = f.lines
            flush(f"recomp_big_{n_big:03d}.c")
            n_big += 1
            shard = shard_save
            continue
        shard += f.lines
        shard_bytes += sum(len(l) for l in f.lines)
        if shard_bytes > 2_500_000:
            flush(f"recomp_gen_{n_shard:03d}.c")
            n_shard += 1
            shard, shard_bytes = [], 0
    if shard:
        flush(f"recomp_gen_{n_shard:03d}.c")
        n_shard += 1
    pe = pefile.PE(str(DLL), fast_load=True)
    ents = sorted(entries)
    tab = ["/* GENERATED by spfy/tools/fe_recomp/recomp.py - do not edit. */",
           "#include \"recomp.h\"", "#include \"recomp_protos.h\"",
           f"const uint32_t rc_image_base = {h32(pe.OPTIONAL_HEADER.ImageBase)};",
           f"const uint32_t rc_image_size = {h32(pe.OPTIONAL_HEADER.SizeOfImage)};",
           f"const uint32_t rc_image_stamp = {h32(pe.FILE_HEADER.TimeDateStamp)};",
           f"const int rc_n_entries = {len(ents)};",
           "const uint32_t rc_entry_va[] = {" + ",".join(h32(e) for e in ents) + "};",
           "int (*const rc_entry_fn[])(void) = {" + ",".join(f"rc_{e:08x}" for e in ents) + "};"]
    (out_dir / "recomp_tab.c").write_text("\n".join(tab) + "\n", encoding="ascii")
    print(f"{len(recs)} functions -> {out_dir} ({n_shard} shards + {n_big} big): {tot['native']} native, "
          f"{tot['step']} stepped, {tot['nf']} flag-free ALU ops ({100 * tot['native'] / max(1, tot['native'] + tot['step']):.1f}% native)")


if __name__ == "__main__":
    sys.exit(main())
