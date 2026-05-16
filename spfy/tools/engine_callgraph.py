"""engine_callgraph.py — static call-graph closure for the K2 reimpl work.

Starting from the 6 SWIttsEngine.dll entry points the FE invokes
(installHookA/B, setPair_E/F/G/H), walk every CALL/JMP target to its
function leaf or to an import-thunk boundary. Produce:

  - dependency_closure: dict {addr: {bytes, len, calls_to, branches_to,
    is_import_thunk, import_target_module, import_target_name}}
  - call_graph: edge list (caller_rva -> callee_rva)
  - per_function: list of leaf-function metadata

This is read-only static analysis. No code is modified.

Usage:
  python spfy/tools/engine_callgraph.py [--json out.json]
"""
from __future__ import annotations

import argparse
import json
from collections import defaultdict, deque
from pathlib import Path

import capstone
import pefile

REPO = Path(__file__).resolve().parents[2]
DLL  = REPO / "bin" / "SWIttsEngine.dll"

ENTRY_POINTS = [
    ('installHookA',  0x4c10),
    ('installHookB',  0x4c40),
    ('setPair_E',     0x4c60),
    ('setPair_F',     0x4ca0),
    ('setPair_G',     0x4cf0),
    ('setPair_H',     0x4d40),
]

MAX_FUNCTION_BYTES = 4096   # safety cap when scanning until ret


def load_pe(path):
    pe = pefile.PE(str(path), fast_load=False)   # full load for imports
    pe.parse_data_directories()
    return pe


def build_section_lookup(pe):
    base = pe.OPTIONAL_HEADER.ImageBase
    sections = []
    for s in pe.sections:
        sections.append({
            'name': s.Name.rstrip(b'\x00').decode('ascii', 'replace'),
            'va_lo': base + s.VirtualAddress,
            'va_hi': base + s.VirtualAddress + s.Misc_VirtualSize,
            'data':  s.get_data(),
            'va':    base + s.VirtualAddress,
        })
    return base, sections


def section_for_va(sections, va):
    for s in sections:
        if s['va_lo'] <= va < s['va_hi']:
            return s
    return None


def read_at_va(sections, va, n):
    s = section_for_va(sections, va)
    if s is None: return None
    off = va - s['va']
    if off + n > len(s['data']): return s['data'][off:]
    return s['data'][off:off + n]


def build_import_map(pe, image_base):
    """RVA-of-import-slot → (dll_name, func_name)."""
    out = {}
    if not hasattr(pe, 'DIRECTORY_ENTRY_IMPORT'):
        return out
    for entry in pe.DIRECTORY_ENTRY_IMPORT:
        dll = entry.dll.decode('ascii', 'replace')
        for imp in entry.imports:
            name = imp.name.decode('ascii', 'replace') if imp.name else f'#ord{imp.ordinal}'
            slot_rva = imp.address - image_base   # absolute IAT VA - base = RVA
            out[image_base + slot_rva] = (dll, name)
    return out


def disasm_function(md, sections, va):
    """Linear sweep from `va`. A function ends at:
      - `ret` or `ret N` followed by alignment padding (`int3` × N or `nop`)
      - Unconditional `jmp imm` followed by alignment padding
    This handles early-return branches inside a function correctly:
    they don't end the function unless they're at a true boundary.
    Returns (instructions, calls, branches, ended_with_ret)."""
    s = section_for_va(sections, va)
    if s is None or s['name'] != '.text':
        return [], [], [], False
    off = va - s['va']
    code = s['data'][off:off + MAX_FUNCTION_BYTES]
    ins_list = []
    calls   = []
    branches = []
    pending_terminator_at = -1  # index of ret/jmp candidate
    is_first_ins = True
    for ins in md.disasm(code, va):
        m = ins.mnemonic
        # Standalone JMP-thunk pattern: a function whose ONLY instruction
        # is `jmp dword ptr [iat_slot]`. These are packed contiguously
        # in MSVC import-thunk blocks WITHOUT int3 padding between them.
        # If our entry point IS such a thunk, end the function after this
        # single instruction.
        if is_first_ins and m == 'jmp' and ins.operands:
            op = ins.operands[0]
            if (op.type == capstone.x86.X86_OP_MEM
                    and op.mem.base == 0 and op.mem.index == 0):
                ins_list.append(ins)
                calls.append((ins.address, ('mem', op.mem.disp & 0xffffffff)))
                pending_terminator_at = 0
                break
        is_first_ins = False
        # Check if previous candidate was the real boundary
        if pending_terminator_at >= 0:
            if m == 'int3' or m == 'nop':
                # Alignment padding confirms function ended
                break
            # Otherwise the "ret" was an early return inside a larger fn
            pending_terminator_at = -1
        ins_list.append(ins)
        if m == 'call' and ins.operands:
            op = ins.operands[0]
            if op.type == capstone.x86.X86_OP_IMM:
                calls.append((ins.address, op.imm & 0xffffffff))
            elif op.type == capstone.x86.X86_OP_MEM and op.mem.base == 0 and op.mem.index == 0:
                calls.append((ins.address, ('mem', op.mem.disp & 0xffffffff)))
        elif m == 'jmp' and ins.operands:
            op = ins.operands[0]
            if op.type == capstone.x86.X86_OP_IMM:
                tgt = op.imm & 0xffffffff
                branches.append((ins.address, tgt))
                # Tail call out of function = call edge
                if tgt < va or tgt >= va + MAX_FUNCTION_BYTES:
                    calls.append((ins.address, tgt))
            elif op.type == capstone.x86.X86_OP_MEM and op.mem.base == 0:
                calls.append((ins.address, ('mem', op.mem.disp & 0xffffffff)))
            pending_terminator_at = len(ins_list) - 1
        elif m.startswith('j'):
            # Conditional branch — record local target
            if ins.operands and ins.operands[0].type == capstone.x86.X86_OP_IMM:
                branches.append((ins.address, ins.operands[0].imm & 0xffffffff))
        elif m == 'ret':
            pending_terminator_at = len(ins_list) - 1
    ended = pending_terminator_at >= 0 or (ins_list and ins_list[-1].mnemonic == 'ret')
    return ins_list, calls, branches, ended


def is_jmp_thunk(ins_list):
    """Classify if this function is just a single `jmp dword ptr [iat]`."""
    if len(ins_list) != 1: return False
    return ins_list[0].mnemonic == 'jmp' and 'dword ptr' in ins_list[0].op_str


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--json', default=None)
    args = ap.parse_args()

    pe = load_pe(DLL)
    image_base, sections = build_section_lookup(pe)
    imports = build_import_map(pe, image_base)
    print(f'image base: 0x{image_base:08x}')
    print(f'imports:    {len(imports)} slots from {len({d for d,_ in imports.values()})} DLLs')

    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    md.detail = True

    # Entry points are RVAs; convert to VAs
    work_q = deque()
    for name, rva in ENTRY_POINTS:
        va = image_base + rva
        work_q.append((va, [name]))

    closure = {}   # va -> dict
    call_edges = []
    visited = set()

    while work_q:
        va, path_via = work_q.popleft()
        if va in visited:
            continue
        visited.add(va)

        # Is this a known import-slot read? (call dword ptr [iat])
        # We won't disasm into imports; they're leaves.
        ins, calls, branches, ended = disasm_function(md, sections, va)
        is_thunk = is_jmp_thunk(ins)

        # Resolve indirect-mem targets to import names if known
        thunk_target = None
        if is_thunk:
            # Parse "jmp dword ptr [0xXXXX]"
            op_str = ins[0].op_str
            try:
                addr_hex = op_str.split('[')[1].rstrip(']').strip()
                imp_va = int(addr_hex, 16)
                thunk_target = imports.get(imp_va)
            except Exception:
                thunk_target = None

        sec = section_for_va(sections, va)
        closure[va] = {
            'va': va,
            'rva': va - image_base,
            'name_path': path_via,
            'section': sec['name'] if sec else None,
            'n_ins': len(ins),
            'n_bytes': sum(len(i.bytes) for i in ins),
            'is_jmp_thunk': is_thunk,
            'thunk_target': {'dll': thunk_target[0], 'func': thunk_target[1]}
                            if thunk_target else None,
            'calls_to':  [{'from_va': c[0], 'to_va': c[1]} for c in calls],
            'ended_with_ret': ended,
        }
        for from_va, to in calls:
            if isinstance(to, tuple):       # ('mem', addr) — indirect import
                _, slot_va = to
                imp = imports.get(slot_va)
                callee_label = ('IMPORT', slot_va, imp)
            else:
                callee_label = to
                # Schedule to disasm if local .text and not seen
                tgt_sec = section_for_va(sections, to)
                if tgt_sec and tgt_sec['name'] == '.text' and to not in visited:
                    new_path = path_via + [f'+0x{to-image_base:x}']
                    work_q.append((to, new_path))
            call_edges.append({'from': from_va, 'to': callee_label})

    # ---- Reporting ----
    print(f'\n=== STATIC CLOSURE ===')
    print(f'Functions reached:        {len(closure)}')
    n_local  = sum(1 for f in closure.values() if f['section'] == '.text' and not f['is_jmp_thunk'])
    n_thunks = sum(1 for f in closure.values() if f['is_jmp_thunk'])
    n_other  = len(closure) - n_local - n_thunks
    print(f'Local .text functions:    {n_local}')
    print(f'Import jmp-thunks:        {n_thunks}')
    print(f'Other (?):                {n_other}')

    # Total bytes of code we'd need to reimplement (local only)
    total_local_bytes = sum(f['n_bytes'] for f in closure.values()
                             if f['section'] == '.text' and not f['is_jmp_thunk'])
    print(f'Total local code bytes:   {total_local_bytes}  (~{total_local_bytes / 4:.0f} 32-bit words)')

    # Cross-DLL imports actually reached through the call closure
    imp_seen = set()
    for ed in call_edges:
        if isinstance(ed['to'], tuple) and ed['to'][0] == 'IMPORT':
            _, _, imp = ed['to']
            if imp:
                imp_seen.add(imp)
    by_dll = defaultdict(list)
    for dll, fn in imp_seen:
        by_dll[dll].append(fn)
    print(f'\n=== CROSS-DLL IMPORTS REACHED ({len(imp_seen)} unique) ===')
    for dll in sorted(by_dll):
        fns = sorted(by_dll[dll])
        print(f'  {dll}: {len(fns)} fn  ({", ".join(fns[:5])}{"..." if len(fns) > 5 else ""})')

    # Print per-function summary sorted by RVA
    print(f'\n=== PER-FUNCTION SUMMARY (local code only) ===')
    print(f'{"rva":>8}  {"size":>6}  {"calls":>5}  notes')
    for f in sorted(closure.values(), key=lambda f: f['rva']):
        if f['section'] != '.text': continue
        if f['is_jmp_thunk']:
            t = f.get('thunk_target')
            tag = (f"thunk -> {t['dll']}!{t['func']}" if t
                   else f"thunk -> ?")
            print(f'  +{f["rva"]:6x}  {f["n_bytes"]:>6}  {len(f["calls_to"]):>5}  {tag}')
        else:
            label = '/'.join(f['name_path'][-2:])
            ret = '' if f['ended_with_ret'] else 'NO-RET'
            print(f'  +{f["rva"]:6x}  {f["n_bytes"]:>6}  {len(f["calls_to"]):>5}  {label}  {ret}')

    if args.json:
        with open(args.json, 'w', encoding='utf-8') as fp:
            json.dump({
                'image_base': image_base,
                'closure': {f'{va:#x}': v for va, v in closure.items()},
                'call_edges': [
                    {'from': e['from'],
                     'to': (e['to'] if not isinstance(e['to'], tuple)
                            else {'kind': 'IMPORT',
                                  'slot_va': e['to'][1],
                                  'dll': e['to'][2][0] if e['to'][2] else None,
                                  'func': e['to'][2][1] if e['to'][2] else None})}
                    for e in call_edges],
                'imports_reached': sorted([{'dll': d, 'func': f} for d, f in imp_seen],
                                          key=lambda x: (x['dll'], x['func'])),
            }, fp, indent=2, default=int)
        print(f'\n# wrote {args.json}')


if __name__ == '__main__':
    main()
