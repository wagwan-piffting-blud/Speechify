"""build_mara_rest.py -- Rebuild hash, prsl, and mean chunks in mara.vin.

Patches three chunks atomically:
  1. hash  -- Tom's original LPC-based join costs (better than our MFCC approx)
  2. prsl  -- Tom curated candidates + Mara adjacency/back-off supplements
  3. mean  -- Tom's original per-halfphone stats (matches unit metadata domain)

Usage:
  python build_mara_rest.py

Inputs  (read-only): mara.vin, tom.vin
Output (overwritten): mara.vin
"""

import os
import random
import struct
import sys
from collections import defaultdict

import numpy as np

import psutil
for proc in psutil.process_iter(['pid', 'name']):
    if proc.info['name'] and proc.info['name'].lower() in ("speechify.exe", "speechify"):
        proc.kill()
        proc.wait(timeout=5)
        print("Killed Speechify process (pid %d) to free file locks." % proc.info['pid'])

# --------------------------------------------------------------------------- #
#  Paths / constants                                                            #
# --------------------------------------------------------------------------- #
_HERE        = os.path.dirname(os.path.abspath(__file__))
VIN_PATH     = os.path.join(_HERE, 'en-US', 'mara', 'mara.vin')
TOM_VIN_PATH = os.path.join(_HERE, 'en-US', 'tom',  'tom.vin')

XOR_KEY       = 0xCE
UNIT_SIZE     = 29

_SILENCE_PCS = {32, 255}
HP_SILENCE   = 92

# HP_BASE: 2*pc with 5 confirmed anomalies
HP_BASE = [2 * pc for pc in range(46)]
HP_BASE[9]  = 22   # aw2
HP_BASE[10] = 18   # ax1
HP_BASE[11] = 20   # ax2
HP_BASE[14] = 30   # b1
HP_BASE[15] = 28   # b2

# prsl back-off settings
MIN_BACKOFF_CANDS   = 10
MAX_CANDS_PER_GROUP = 200

# --------------------------------------------------------------------------- #
#  Utility                                                                      #
# --------------------------------------------------------------------------- #
def xor_decode(path):
    raw = np.fromfile(path, dtype=np.uint8)
    raw ^= XOR_KEY
    return raw.tobytes()


def riff_chunks(data, start=0, end=None):
    if end is None:
        end = len(data)
    pos = start
    while pos + 8 <= end:
        tag = bytes(data[pos:pos+4])
        sz  = struct.unpack_from('<I', data, pos+4)[0]
        yield tag, pos+8, sz
        pos += 8 + sz + (sz & 1)


def pack_chunk(tag, body):
    pad = b'\x00' if len(body) & 1 else b''
    return tag.encode('latin1') + struct.pack('<I', len(body)) + body + pad


def unit_hp(pc, is_first_half):
    if pc in _SILENCE_PCS or pc >= 46:
        return HP_SILENCE
    return HP_BASE[pc] + (1 - int(is_first_half))


def compute_context_key(left_hp, pc, is_first_half, right_hp):
    if pc in _SILENCE_PCS or pc >= 46:
        return None
    base = HP_BASE[pc]
    if left_hp == 0:
        center_hp = base
    else:
        center_hp = base + (1 if left_hp == HP_SILENCE and right_hp != HP_SILENCE
                            else left_hp % 2)
    return left_hp * 10000 + center_hp * 100 + right_hp


# =========================================================================== #
#  Step 1: Parse mara.vin (shared across all three patches)                    #
# =========================================================================== #
print("=" * 60)
print("build_mara_rest.py -- hash + prsl + mean patches")
print("=" * 60)

print("\nLoading mara.vin ...")
vin = bytearray(xor_decode(VIN_PATH))
assert bytes(vin[:4]) == b'RIFF' and bytes(vin[8:12]) == b'svin'
vin_chunks = {tag: (ds, sz) for tag, ds, sz in riff_chunks(vin, start=12)}

# -- filenames --
feat_ds, feat_sz = vin_chunks[b'feat']
feat_data = bytes(vin[feat_ds : feat_ds + feat_sz])
fn_idx   = feat_data.find(b'filename')
fn_count = struct.unpack_from('<I', feat_data, fn_idx + 8)[0]
p = fn_idx + 12
filenames = {}
for _ in range(fn_count):
    nlen = struct.unpack_from('<H', feat_data, p)[0]
    name = feat_data[p+2:p+2+nlen].decode('latin-1', errors='replace')
    sid  = struct.unpack_from('<I', feat_data, p+2+nlen)[0]
    filenames[sid] = name
    p += 2 + nlen + 4
print(f"  {fn_count} filenames ({len(filenames)} distinct stored_ids)")

# -- cnts: read unit count dynamically --
cnts_ds, cnts_sz = vin_chunks[b'cnts']
N_UNITS = struct.unpack_from('<I', vin, cnts_ds + 8)[0]

# -- unit table --
unit_ds, unit_sz = vin_chunks[b'unit']
unit_data_ds = unit_data_sz = None
for tag, ds, sz in riff_chunks(vin, start=unit_ds, end=unit_ds+unit_sz):
    if tag == b'data':
        unit_data_ds, unit_data_sz = ds, sz
        break
assert unit_data_ds is not None and unit_data_sz == N_UNITS * UNIT_SIZE

uid_fidx = np.empty(N_UNITS, dtype=np.uint16)
uid_lp   = np.empty(N_UNITS, dtype=np.uint16)
uid_dl   = np.empty(N_UNITS, dtype=np.uint16)
uid_pc   = np.empty(N_UNITS, dtype=np.uint8)
uid_is1  = np.empty(N_UNITS, dtype=np.uint8)
unit_raw = bytes(vin[unit_data_ds : unit_data_ds + N_UNITS * UNIT_SIZE])
for i in range(N_UNITS):
    base = i * UNIT_SIZE
    uid_fidx[i] = struct.unpack_from('<H', unit_raw, base +  4)[0]
    uid_lp[i]   = struct.unpack_from('<H', unit_raw, base +  6)[0]
    uid_dl[i]   = struct.unpack_from('<H', unit_raw, base + 10)[0]
    uid_pc[i]   = unit_raw[base + 20]
    uid_is1[i]  = unit_raw[base + 21]
print(f"  {N_UNITS} units parsed")

# Group units by file_idx (shared)
units_by_fidx = defaultdict(list)
for uid in range(N_UNITS):
    units_by_fidx[int(uid_fidx[uid])].append(uid)

# -- Load Tom VIN (shared across hash, prsl, mean patches) --
print("\nLoading tom.vin ...")
tom_raw = bytearray(xor_decode(TOM_VIN_PATH))
assert bytes(tom_raw[:4]) == b'RIFF' and bytes(tom_raw[8:12]) == b'svin'
tom_chunks = {tag: (ds, sz) for tag, ds, sz in riff_chunks(tom_raw, start=12)}
print(f"  {len(tom_raw):,} bytes")

# (VDB no longer needed -- hash and mean use Tom's original chunks)

# =========================================================================== #
#  PATCH 1: hash -- Use Tom's original join cost hash                          #
# =========================================================================== #
# Tom's hash was computed by SpeechWorks' original tools using LPC/autocorr-
# based spectral distance.  Our mel-MFCC recomputation doesn't match the
# engine's method (raw median 95.77 vs Tom's 0.87 = 110x scale mismatch).
# Since Mara's remapped units share Tom's phonetic content, Tom's original
# pair costs guide better selection than our approximation.
print("\n" + "=" * 60)
print("PATCH 1/3: hash (Tom's original join costs)")
print("=" * 60)

hash_ds, hash_sz = vin_chunks[b'hash']

# Copy Tom's hash chunk verbatim
tom_hash_ds, tom_hash_sz = tom_chunks[b'hash']
tom_hash_body = bytes(tom_raw[tom_hash_ds : tom_hash_ds + tom_hash_sz])
new_hash_chunk = pack_chunk('hash', tom_hash_body)
print(f"  Using Tom's original hash ({tom_hash_sz:,} bytes)")
print(f"  Hash chunk: {hash_sz + 8:,} -> {len(new_hash_chunk):,} bytes")

# =========================================================================== #
#  PATCH 2: prsl -- Mara candidates + Tom fallback                             #
# =========================================================================== #
print("\n" + "=" * 60)
print("PATCH 2/3: prsl (Mara preselection candidates)")
print("=" * 60)

prsl_ds, prsl_sz = vin_chunks[b'prsl']
old_prsl_count = struct.unpack_from('<I', vin, prsl_ds)[0]
print(f"  Old prsl: {old_prsl_count:,} groups")

# Sort each recording by lp
by_fidx_sorted = {}
for fidx, uids in units_by_fidx.items():
    by_fidx_sorted[fidx] = sorted(uids, key=lambda uid: int(uid_lp[uid]))

# Compute context keys
print("  Computing context keys ...")
uid_context_key = np.full(N_UNITS, -1, dtype=np.int64)
for fidx, uids in by_fidx_sorted.items():
    n = len(uids)
    for j, uid in enumerate(uids):
        pc  = int(uid_pc[uid])
        is1 = int(uid_is1[uid])
        left_hp = 0 if j == 0 else unit_hp(int(uid_pc[uids[j-1]]), int(uid_is1[uids[j-1]]))
        right_hp = HP_SILENCE if j == n - 1 else unit_hp(int(uid_pc[uids[j+1]]), int(uid_is1[uids[j+1]]))
        ck = compute_context_key(left_hp, pc, is1, right_hp)
        if ck is not None:
            uid_context_key[uid] = ck

n_keyed = int((uid_context_key >= 0).sum())
print(f"  {n_keyed:,} units assigned a context_key")

# Compute "run potential" for each unit: how many same-recording neighbors
# surround it in the sorted-by-lp order.  Units with high run potential
# give the engine longer same-recording stretches = less stutter.
MIN_RUN_POTENTIAL = 3  # minimum neighbors to be a "preferred" candidate
print("  Computing per-unit run potential ...")
uid_run_potential = np.zeros(N_UNITS, dtype=np.int32)
for fidx, uids in by_fidx_sorted.items():
    n = len(uids)
    for j in range(n):
        uid = uids[j]
        if uid_dl[uid] <= 0:
            continue
        # Count consecutive active neighbors (including self)
        run_len = 1
        # Look left
        for k in range(j - 1, -1, -1):
            if uid_dl[uids[k]] > 0:
                run_len += 1
            else:
                break
        # Look right
        for k in range(j + 1, n):
            if uid_dl[uids[k]] > 0:
                run_len += 1
            else:
                break
        uid_run_potential[uid] = run_len

n_preferred = int((uid_run_potential >= MIN_RUN_POTENTIAL).sum())
n_short_run = int(((uid_run_potential > 0) & (uid_run_potential < MIN_RUN_POTENTIAL)).sum())
print(f"  Preferred (run>={MIN_RUN_POTENTIAL}): {n_preferred:,}  Short-run (<{MIN_RUN_POTENTIAL}): {n_short_run:,}")

# Group Mara units by adjacency-derived context_key (best matches)
# Sort candidates within each key by run_potential (descending) so the engine
# sees high-run candidates first in the prsl.
mara_ck = defaultdict(list)
for uid in range(N_UNITS):
    ck = int(uid_context_key[uid])
    if ck >= 0 and uid_dl[uid] > 0:
        mara_ck[ck].append(uid)
# Sort each group: preferred (high run potential) first
for ck in mara_ck:
    mara_ck[ck].sort(key=lambda uid: -uid_run_potential[uid])
print(f"  {len(mara_ck):,} distinct Mara adjacency context_keys")

# Group ALL Mara units by center phone (for populating non-adjacency keys)
units_by_pc = defaultdict(list)
for uid in range(N_UNITS):
    pc = int(uid_pc[uid])
    if pc < 46 and pc != 255 and uid_dl[uid] > 0:
        units_by_pc[pc].append(uid)
# Sort each phone pool by run potential (preferred first), then shuffle within tiers
rng = random.Random(42)
for pc in units_by_pc:
    units_by_pc[pc].sort(key=lambda uid: -uid_run_potential[uid])
print(f"  {sum(len(v) for v in units_by_pc.values()):,} usable units across {len(units_by_pc)} phones")

# Reverse map: center_hp -> phone index (pc)
_hp_to_pc = {}
for pc in range(46):
    _hp_to_pc[HP_BASE[pc]]     = pc
    _hp_to_pc[HP_BASE[pc] + 1] = pc

# Build back-off indexes for graduated candidate selection.
# For a target key (L, C, R), candidates are selected in priority order:
#   1. Exact adjacency match: units whose context_key == target key
#   2. Left+Center match: same (L, C), any R -> similar left boundary
#   3. Center+Right match: same (C, R), any L -> similar right boundary
#   4. Center-phone fill: any unit with matching center phone
# This gives the engine much better candidates than random phone fill.
print("  Building back-off indexes ...")
lc_index = defaultdict(list)   # (left_hp, center_hp) -> [uid, ...]
cr_index = defaultdict(list)   # (center_hp, right_hp) -> [uid, ...]

for ck, uids in mara_ck.items():
    left_hp   = ck // 10000
    center_hp = (ck // 100) % 100
    right_hp  = ck % 100
    lc_index[(left_hp, center_hp)].extend(uids)
    cr_index[(center_hp, right_hp)].extend(uids)

# De-duplicate and sort by run potential (preferred first)
for key in lc_index:
    lc_index[key] = list(set(lc_index[key]))
    lc_index[key].sort(key=lambda uid: -uid_run_potential[uid])
for key in cr_index:
    cr_index[key] = list(set(cr_index[key]))
    cr_index[key].sort(key=lambda uid: -uid_run_potential[uid])
print(f"  LC pairs: {len(lc_index):,}  CR pairs: {len(cr_index):,}")

# Load Tom's full prsl (keys AND candidate UIDs)
# Tom's candidates are UIDs 0-169578 which exist in mara.vin and point to
# Mara's audio.  They were curated by SpeechWorks' original tools to have
# low cat cost, so we use them as the foundation for each group.
print("  Loading Tom's prsl (keys + candidates) ...")
tom_prsl_ds, tom_prsl_sz = tom_chunks[b'prsl']
tom_prsl = {}   # ck -> [uid, ...]
tom_all_keys = set()  # ALL keys Tom had, even if candidates all have dl=0
pos = tom_prsl_ds
n_tom_groups = struct.unpack_from('<I', tom_raw, pos)[0]
pos += 4
tom_total_cands = 0
tom_empty_keys = 0
for _ in range(n_tom_groups):
    grp_n  = struct.unpack_from('<I', tom_raw, pos)[0]
    grp_ck = struct.unpack_from('<I', tom_raw, pos + 4)[0]
    n_cands = grp_n - 1
    cands = list(struct.unpack_from('<%dI' % n_cands, tom_raw, pos + 8))
    pos += 4 + grp_n * 4
    tom_all_keys.add(grp_ck)
    # Filter to UIDs that exist in Mara's unit table and have audio (dl > 0)
    # Sort by run_potential descending so high-run candidates come first
    valid = [uid for uid in cands if uid < N_UNITS and uid_dl[uid] > 0]
    valid.sort(key=lambda uid: -uid_run_potential[uid])
    if valid:
        tom_prsl[grp_ck] = valid
        tom_total_cands += len(valid)
    else:
        tom_empty_keys += 1
print(f"  {n_tom_groups:,} Tom groups, {tom_total_cands:,} valid candidates kept")
if tom_empty_keys:
    print(f"  {tom_empty_keys:,} Tom keys lost ALL candidates (dl=0) -- will back-fill")

# Build merged prsl: Tom curated candidates first, then Mara back-off.
# For each key, the engine gets Tom's low-cost candidates (which play Mara
# audio from mara8.vdb) plus Mara-specific candidates for extra coverage.
# CRITICAL: include ALL Tom keys, even those with 0 valid Tom candidates.
# The engine can request any key Tom originally had; missing keys cause
# "no valid units found" runtime errors.
all_target_keys = tom_all_keys | set(mara_ck.keys())
print(f"  Target key space: {len(all_target_keys):,} keys")

merged_ck = {}
n_tom_only = 0
n_mara_only = 0
n_hybrid = 0
empty = 0
stats_source = [0, 0, 0, 0, 0]  # tom_curated, exact, lc, cr, phone-fill

for ck in all_target_keys:
    left_hp   = ck // 10000
    center_hp = (ck // 100) % 100
    right_hp  = ck % 100
    pc = _hp_to_pc.get(center_hp)
    if pc is None:
        continue

    cands = []
    existing = set()

    # Foundation: Tom's curated candidates (low cat cost, Mara audio)
    for uid in tom_prsl.get(ck, []):
        if uid not in existing:
            cands.append(uid)
            existing.add(uid)
    n_tom = len(cands)

    # Supplement: Mara exact adjacency match
    for uid in mara_ck.get(ck, []):
        if len(cands) >= MAX_CANDS_PER_GROUP:
            break
        if uid not in existing:
            cands.append(uid)
            existing.add(uid)
    n_exact = len(cands) - n_tom

    # Supplement: Mara left+center back-off
    for uid in lc_index.get((left_hp, center_hp), []):
        if len(cands) >= MAX_CANDS_PER_GROUP:
            break
        if uid not in existing:
            cands.append(uid)
            existing.add(uid)
    n_lc = len(cands) - n_tom - n_exact

    # Supplement: Mara center+right back-off
    for uid in cr_index.get((center_hp, right_hp), []):
        if len(cands) >= MAX_CANDS_PER_GROUP:
            break
        if uid not in existing:
            cands.append(uid)
            existing.add(uid)
    n_cr = len(cands) - n_tom - n_exact - n_lc

    # Supplement: Mara phone-fill (only if we still need more)
    if len(cands) < MIN_BACKOFF_CANDS:
        pool = units_by_pc.get(pc, [])
        for uid in pool:
            if len(cands) >= MAX_CANDS_PER_GROUP:
                break
            if uid not in existing:
                cands.append(uid)
                existing.add(uid)
    n_phone = len(cands) - n_tom - n_exact - n_lc - n_cr

    if cands:
        merged_ck[ck] = cands
        has_tom  = n_tom > 0
        has_mara = (n_exact + n_lc + n_cr + n_phone) > 0
        if has_tom and has_mara:
            n_hybrid += 1
        elif has_tom:
            n_tom_only += 1
        else:
            n_mara_only += 1
        stats_source[0] += n_tom
        stats_source[1] += n_exact
        stats_source[2] += n_lc
        stats_source[3] += n_cr
        stats_source[4] += n_phone
    else:
        empty += 1

print(f"  Populated: {len(merged_ck):,} keys "
      f"({n_hybrid:,} hybrid, {n_tom_only:,} Tom-only, "
      f"{n_mara_only:,} Mara-only, {empty:,} empty/skipped)")
print(f"  Candidate sources: tom_curated={stats_source[0]:,}  "
      f"exact={stats_source[1]:,}  left+center={stats_source[2]:,}  "
      f"center+right={stats_source[3]:,}  phone-fill={stats_source[4]:,}")

# Encode prsl
sorted_cks = sorted(merged_ck.keys())
parts = [struct.pack('<I', len(merged_ck))]
for ck in sorted_cks:
    cands = merged_ck[ck]
    n = 1 + len(cands)
    parts.append(struct.pack('<I', n))
    parts.append(struct.pack('<I', ck))
    for uid in cands:
        parts.append(struct.pack('<I', uid))

new_prsl_chunk = pack_chunk('prsl', b''.join(parts))
print(f"  Prsl chunk: {prsl_sz + 8:,} -> {len(new_prsl_chunk):,} bytes")

# =========================================================================== #
#  PATCH 3: mean -- Use Tom's original per-halfphone statistics                #
# =========================================================================== #
# Tom's mean values match the unit metadata (f0_context, f0_start, byte 28)
# which are unchanged from Tom in the remapped units.  Using Tom's stats
# gives correct normalization for the engine's target cost evaluation.
print("\n" + "=" * 60)
print("PATCH 3/3: mean (Tom's original per-halfphone stats)")
print("=" * 60)

mean_ds, mean_sz = vin_chunks[b'mean']

# Copy Tom's mean chunk verbatim
tom_mean_ds, tom_mean_sz = tom_chunks[b'mean']
tom_mean_body = bytes(tom_raw[tom_mean_ds : tom_mean_ds + tom_mean_sz])
new_mean_chunk = pack_chunk('mean', tom_mean_body)
print(f"  Using Tom's original mean ({tom_mean_sz:,} bytes)")
print(f"  Mean chunk: {mean_sz + 8:,} -> {len(new_mean_chunk):,} bytes")

# =========================================================================== #
#  Apply all three patches atomically                                          #
# =========================================================================== #
print("\n" + "=" * 60)
print("Applying all patches to mara.vin ...")
print("=" * 60)

# We need to splice three chunks. Since splicing changes offsets, we process
# chunks in reverse order of their position in the file.
patches = [
    ('hash', hash_ds, hash_sz, new_hash_chunk),
    ('prsl', prsl_ds, prsl_sz, new_prsl_chunk),
    ('mean', mean_ds, mean_sz, new_mean_chunk),
]
# Sort by offset descending so later splices don't shift earlier offsets
patches.sort(key=lambda x: -x[1])

new_vin = bytes(vin)
for name, ds, sz, chunk in patches:
    chunk_start = ds - 8
    chunk_end   = ds + sz + (sz & 1)
    new_vin = new_vin[:chunk_start] + chunk + new_vin[chunk_end:]
    print(f"  Patched {name}")

# Fix RIFF size
new_riff_size = len(new_vin) - 8
new_vin = new_vin[:4] + struct.pack('<I', new_riff_size) + new_vin[8:]

# XOR-encode and write
encoded = bytes(np.frombuffer(new_vin, dtype=np.uint8) ^ XOR_KEY)
tmp = VIN_PATH + '.rest_tmp'
with open(tmp, 'wb') as f:
    f.write(encoded)
os.replace(tmp, VIN_PATH)

print(f"\n  Wrote {VIN_PATH}  ({len(encoded):,} bytes)")
print("\nDone -- hash + prsl + mean rebuilt from Mara's audio.")
print("Run synthesis test to verify.")
print("="*252)
