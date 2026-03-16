"""build_mara_hash.py -- Patch join-cost values in mara.vin hash chunk.

Copies Tom's EXACT hash binary structure (rows, cells_A chain layout, suffix
sharing, trailing sentinels -- everything) and only modifies cells_B cost
values using REAL spectral distances from Mara's VDB audio.

Strategy:
  - Same-recording (uid_left, uid_right) pairs: cost = 0.0 (free join)
  - Cross-recording pairs: spectral boundary distance from Mara's audio
    (12-dim log-magnitude FFT at unit boundaries)

The chain structure (cells_A, rows) is NEVER rebuilt -- it is copied byte-for-
byte from Tom's original hash.

Usage:
  python build_mara_hash.py

Inputs  (read-only):  mara.vin, mara8.vdb, tom.vin
Output (overwritten): mara.vin
"""

import audioop
import os
import struct
import sys
import time

import numpy as np
from collections import defaultdict

import psutil
for proc in psutil.process_iter(['pid', 'name']):
    if proc.info['name'] and proc.info['name'].lower() in ("speechify.exe", "speechify"):
        proc.kill()
        proc.wait(timeout=5)
        print("Killed Speechify process (pid %d) to free file locks." % proc.info['pid'])

# --------------------------------------------------------------------------- #
#  Paths / constants                                                           #
# --------------------------------------------------------------------------- #
_HERE        = os.path.dirname(os.path.abspath(__file__))
VIN_PATH     = os.path.join(_HERE, 'en-US', 'mara', 'mara.vin')
VDB_PATH     = os.path.join(_HERE, 'en-US', 'mara', 'mara8.vdb')
TOM_VIN_PATH = os.path.join(_HERE, 'en-US', 'tom',  'tom.vin')

XOR_KEY        = 0xCE
SENTINEL       = 0xFFFFFFFF
UNIT_SIZE      = 29

# Miss penalty: cost stored at SENTINEL (empty) hash positions.
# NOTE: jne-NOP approach FAILED (cross-cell contamination from perfect hash sharing).
# Kept at 0.0 for now. Needs a different approach to penalize misses.
MISS_PENALTY   = 0.0

# Spectral feature parameters
BOUNDARY_MS    = 8     # ms of audio at each boundary
BOUNDARY_SAMP  = 64    # 8ms * 8000 Hz = 64 samples
N_FFT_BINS     = 33    # BOUNDARY_SAMP // 2 + 1
N_FEATURES     = 12    # number of spectral features to use
# Set > 0 to use Mara's spectral boundary distances for cross-rec costs.
# Set 0 to keep Tom's original costs (best so far: mean=0.97, 35 switches).
COST_SCALE     = 0.0

# Recording spectral clustering: discount cross-rec costs between similar recordings.
# Recordings within CLUSTER_DIST_THRESH of each other get cost *= CLUSTER_DISCOUNT.
# Set CLUSTER_DISCOUNT = 1.0 to disable (no discount).
CLUSTER_DISCOUNT = 0.3   # similar-rec pairs pay 30% of Tom's cost
CLUSTER_DIST_PCTILE = 25  # percentile of pairwise distances to use as threshold

# Run-potential penalty: penalize transitions TO units with short same-rec runs.
# This biases the Viterbi path toward longer runs = less stutter.
RUN_PENALTY_MIN = 6     # units with run_potential < this get penalized
RUN_PENALTY_MULT = 8.0  # max cost multiplier for the worst (run=1) candidates

# --------------------------------------------------------------------------- #
#  Utility                                                                     #
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
    if isinstance(tag, str):
        tag = tag.encode('latin1')
    pad = b'\x00' if len(body) & 1 else b''
    return tag + struct.pack('<I', len(body)) + body + pad


# =========================================================================== #
#  Step 1: Parse mara.vin + mara8.vdb                                         #
# =========================================================================== #
print("=" * 70)
print("build_mara_hash.py -- Patch hash costs (spectral boundary distances)")
print("=" * 70)

print("\nLoading mara.vin ...")
vin = bytearray(xor_decode(VIN_PATH))
assert vin[:4] == b'RIFF' and vin[8:12] == b'svin'
vin_chunks = {tag: (ds, sz) for tag, ds, sz in riff_chunks(vin, start=12)}

# -- cnts: read unit count dynamically --
cnts_ds, cnts_sz = vin_chunks[b'cnts']
N_UNITS = struct.unpack_from('<I', vin, cnts_ds + 8)[0]

# -- unit table: extract file_idx, lp, dl for each unit --
unit_ds, unit_sz = vin_chunks[b'unit']
unit_data_ds = None
for tag, ds, sz in riff_chunks(vin, start=unit_ds, end=unit_ds+unit_sz):
    if tag == b'data':
        unit_data_ds = ds
        break
assert unit_data_ds is not None

uid_fidx = np.empty(N_UNITS, dtype=np.uint16)
uid_lp   = np.empty(N_UNITS, dtype=np.uint16)
uid_dl   = np.empty(N_UNITS, dtype=np.uint16)
unit_raw = bytes(vin[unit_data_ds : unit_data_ds + N_UNITS * UNIT_SIZE])
for i in range(N_UNITS):
    base = i * UNIT_SIZE
    uid_fidx[i] = struct.unpack_from('<H', unit_raw, base + 4)[0]
    uid_lp[i]   = struct.unpack_from('<H', unit_raw, base + 6)[0]
    uid_dl[i]   = struct.unpack_from('<H', unit_raw, base + 10)[0]
print("  %d units parsed" % N_UNITS)

# -- Compute per-unit run potential (how many active same-rec neighbors) --
print("  Computing run potential ...")
uid_run_potential = np.zeros(N_UNITS, dtype=np.int32)
units_by_fidx_hash = defaultdict(list)
for uid in range(N_UNITS):
    if uid_dl[uid] > 0:
        units_by_fidx_hash[int(uid_fidx[uid])].append(uid)
for fidx, uids in units_by_fidx_hash.items():
    uids_sorted = sorted(uids, key=lambda u: int(uid_lp[u]))
    n = len(uids_sorted)
    for j in range(n):
        run_len = 1
        for k in range(j - 1, -1, -1):
            if uid_dl[uids_sorted[k]] > 0:
                run_len += 1
            else:
                break
        for k in range(j + 1, n):
            if uid_dl[uids_sorted[k]] > 0:
                run_len += 1
            else:
                break
        uid_run_potential[uids_sorted[j]] = run_len
n_highrun = int((uid_run_potential >= RUN_PENALTY_MIN).sum())
print("  Run potential: %d units >= %d (high-run), %d < %d (penalized)" % (
    n_highrun, RUN_PENALTY_MIN,
    int(((uid_run_potential > 0) & (uid_run_potential < RUN_PENALTY_MIN)).sum()),
    RUN_PENALTY_MIN))

# -- Parse feat -> filenames for VDB lookup --
feat_ds, feat_sz = vin_chunks[b'feat']
feat = vin[feat_ds : feat_ds + feat_sz]
fn_idx = feat.find(b'filename')
fn_count = struct.unpack_from('<I', feat, fn_idx + 8)[0]
p = fn_idx + 12
filenames = {}
for _ in range(fn_count):
    nlen = struct.unpack_from('<H', feat, p)[0]
    name = feat[p+2 : p+2+nlen].decode('latin-1', errors='replace')
    stored_id = struct.unpack_from('<I', feat, p+2+nlen)[0]
    filenames[stored_id] = name
    p += 2 + nlen + 4
print("  %d filenames" % len(filenames))

# -- Load VDB audio data --
print("Loading mara8.vdb ...")
vdb_raw = xor_decode(VDB_PATH)
vdb_chunks = {tag: (ds, sz) for tag, ds, sz in riff_chunks(vdb_raw, start=12)}
vdb_data_ds, vdb_data_sz = vdb_chunks[b'data']

# Parse VDB indx to get per-recording byte offsets
vdb_indx_ds, vdb_indx_sz = vdb_chunks[b'indx']
vp = vdb_indx_ds
vdb_count = struct.unpack_from('<I', vdb_raw, vp)[0]
vp += 4
vdb_entries = []
for _ in range(vdb_count + 1):
    boff = struct.unpack_from('<I', vdb_raw, vp)[0]
    vp += 4
    nlen = struct.unpack_from('<H', vdb_raw, vp)[0]
    vp += 2
    name = vdb_raw[vp:vp+nlen].decode('ascii', errors='replace')
    vp += nlen
    vdb_entries.append((boff, name))

# Build name -> (abs_offset, nbytes) for VDB audio
vdb_rec_info = {}
for i in range(len(vdb_entries) - 1):
    name = vdb_entries[i][1]
    if name:
        abs_off = vdb_data_ds + vdb_entries[i][0]
        nbytes = vdb_entries[i+1][0] - vdb_entries[i][0]
        vdb_rec_info[name] = (abs_off, nbytes)
print("  %d VDB recordings" % len(vdb_rec_info))

# =========================================================================== #
#  Step 2: Compute boundary spectral features for all units                    #
# =========================================================================== #
print("\nComputing boundary spectral features (%d-dim, %dms window) ..."
      % (N_FEATURES, BOUNDARY_MS))
t0 = time.time()

# Pre-compute Hann window for FFT
hann = np.hanning(BOUNDARY_SAMP).astype(np.float32)

# Features: left_feat[uid] = spectral features at unit start (left boundary)
#            right_feat[uid] = spectral features at unit end (right boundary)
left_feat  = np.zeros((N_UNITS, N_FEATURES), dtype=np.float32)
right_feat = np.zeros((N_UNITS, N_FEATURES), dtype=np.float32)
feat_valid = np.zeros(N_UNITS, dtype=bool)

# Group units by recording for efficient VDB access
units_by_fidx = defaultdict(list)
for uid in range(N_UNITS):
    units_by_fidx[int(uid_fidx[uid])].append(uid)

n_computed = 0
n_skipped = 0

for fidx, uids in units_by_fidx.items():
    rec_name = filenames.get(fidx, '')
    if rec_name not in vdb_rec_info:
        n_skipped += len(uids)
        continue
    abs_off, nbytes = vdb_rec_info[rec_name]

    for uid in uids:
        lp = int(uid_lp[uid])
        dl = int(uid_dl[uid])
        if dl < 2:
            n_skipped += 1
            continue

        # Byte offsets in VDB
        bo_start = lp * 8
        bo_end = (lp + dl) * 8

        if bo_start + BOUNDARY_SAMP > nbytes or bo_end > nbytes:
            n_skipped += 1
            continue

        # Extract boundary audio (mu-law -> PCM16 -> float)
        # Left boundary: first BOUNDARY_SAMP bytes from unit start
        left_ulaw = vdb_raw[abs_off + bo_start : abs_off + bo_start + BOUNDARY_SAMP]
        # Right boundary: last BOUNDARY_SAMP bytes before unit end
        right_start = max(bo_start, bo_end - BOUNDARY_SAMP)
        right_ulaw = vdb_raw[abs_off + right_start : abs_off + right_start + BOUNDARY_SAMP]

        if len(left_ulaw) < BOUNDARY_SAMP or len(right_ulaw) < BOUNDARY_SAMP:
            n_skipped += 1
            continue

        try:
            left_pcm = np.frombuffer(audioop.ulaw2lin(bytes(left_ulaw), 2), dtype='<i2').astype(np.float32)
            right_pcm = np.frombuffer(audioop.ulaw2lin(bytes(right_ulaw), 2), dtype='<i2').astype(np.float32)
        except Exception:
            n_skipped += 1
            continue

        # Apply window and compute log-magnitude FFT
        left_fft = np.abs(np.fft.rfft(left_pcm * hann))
        right_fft = np.abs(np.fft.rfft(right_pcm * hann))

        # Log magnitude (add small epsilon to avoid log(0))
        left_log = np.log(left_fft[:N_FEATURES] + 1e-8)
        right_log = np.log(right_fft[:N_FEATURES] + 1e-8)

        left_feat[uid] = left_log
        right_feat[uid] = right_log
        feat_valid[uid] = True
        n_computed += 1

elapsed = time.time() - t0
print("  Computed: %d units (%.1fs)" % (n_computed, elapsed))
print("  Skipped:  %d units (no audio / too short)" % n_skipped)

# =========================================================================== #
#  Step 2b: Compute per-recording spectral fingerprints                        #
# =========================================================================== #
print("\nComputing per-recording spectral fingerprints ...")
max_fidx = int(uid_fidx.max()) + 1
rec_feat_sum = np.zeros((max_fidx, N_FEATURES), dtype=np.float64)
rec_feat_cnt = np.zeros(max_fidx, dtype=np.int32)

for uid in range(N_UNITS):
    if feat_valid[uid]:
        fi = int(uid_fidx[uid])
        # Average of left + right boundary features = recording spectral character
        rec_feat_sum[fi] += (left_feat[uid].astype(np.float64) +
                             right_feat[uid].astype(np.float64)) * 0.5
        rec_feat_cnt[fi] += 1

rec_has_feat = rec_feat_cnt > 0
rec_mean = np.zeros((max_fidx, N_FEATURES), dtype=np.float32)
rec_mean[rec_has_feat] = (rec_feat_sum[rec_has_feat] /
                          rec_feat_cnt[rec_has_feat, np.newaxis]).astype(np.float32)

n_recs_with_feat = int(rec_has_feat.sum())
print("  %d recordings have spectral fingerprints" % n_recs_with_feat)

# Compute distance threshold from a sample of pairwise distances
if CLUSTER_DISCOUNT < 1.0 and n_recs_with_feat > 10:
    valid_fidxs = np.where(rec_has_feat)[0]
    # Sample up to 5000 random pairs to estimate distribution
    rng = np.random.RandomState(42)
    n_sample = min(5000, n_recs_with_feat * (n_recs_with_feat - 1) // 2)
    idx_a = rng.choice(valid_fidxs, size=n_sample)
    idx_b = rng.choice(valid_fidxs, size=n_sample)
    # Avoid self-pairs
    mask = idx_a != idx_b
    idx_a, idx_b = idx_a[mask], idx_b[mask]
    diffs = rec_mean[idx_a] - rec_mean[idx_b]
    sample_dists = np.sqrt(np.sum(diffs ** 2, axis=1))
    CLUSTER_DIST_THRESH = float(np.percentile(sample_dists, CLUSTER_DIST_PCTILE))
    print("  Pairwise distance stats: mean=%.2f  std=%.2f  p25=%.2f  p50=%.2f  p75=%.2f" % (
        float(np.mean(sample_dists)), float(np.std(sample_dists)),
        float(np.percentile(sample_dists, 25)),
        float(np.percentile(sample_dists, 50)),
        float(np.percentile(sample_dists, 75))))
    print("  Cluster threshold (p%d): %.2f" % (CLUSTER_DIST_PCTILE, CLUSTER_DIST_THRESH))
else:
    CLUSTER_DIST_THRESH = 0.0

# =========================================================================== #
#  Step 3: Load Tom's hash, patch costs in-place                               #
# =========================================================================== #
# Hash is a compressed perfect hash (confirmed 2026-03-16 via disassembly).
# Lookup: cell[rows[uid_right] + uid_left].  Single indexed access, no chains.
# We keep Tom's structure intact and modify cells_B costs.  For extra units,
# we append a small extension region and give all extras a shared offset into it.
TOM_N_UNITS = 169579

print("\nLoading tom.vin hash structure ...")
tom_raw = xor_decode(TOM_VIN_PATH)
tom_chunks = {tag: (ds, sz) for tag, ds, sz in riff_chunks(tom_raw, start=12)}
tom_hash_ds, tom_hash_sz = tom_chunks[b'hash']

tom_hash_sub = {}
for tag, ds, sz in riff_chunks(tom_raw, start=tom_hash_ds, end=tom_hash_ds+tom_hash_sz):
    tom_hash_sub[tag] = (ds, sz)

tom_head_ds, _ = tom_hash_sub[b'head']
n_rows  = struct.unpack_from('<I', tom_raw, tom_head_ds    )[0]
n_cells = struct.unpack_from('<I', tom_raw, tom_head_ds + 4)[0]
print("  n_rows=%d  n_cells=%d" % (n_rows, n_cells))

tom_rows_ds, tom_rows_sz = tom_hash_sub[b'rows']
rows = np.frombuffer(tom_raw[tom_rows_ds : tom_rows_ds + tom_rows_sz],
                     dtype=np.uint32).copy()

tom_cell_ds, tom_cell_sz = tom_hash_sub[b'cell']
cells_A = np.frombuffer(tom_raw[tom_cell_ds : tom_cell_ds + n_cells*4],
                        dtype=np.uint32).copy()
cells_B = np.frombuffer(tom_raw[tom_cell_ds + n_cells*4 : tom_cell_ds + n_cells*8],
                        dtype=np.float32).copy()

data_mask = cells_A != SENTINEL
n_data = int(data_mask.sum())
print("  %d data entries, %d sentinels" % (n_data, n_cells - n_data))

tom_data_costs = cells_B[data_mask]
print("  Tom cost stats: mean=%.2f std=%.2f max=%.2f" % (
    float(np.mean(tom_data_costs)), float(np.std(tom_data_costs)),
    float(np.max(tom_data_costs))))

# =========================================================================== #
#  Step 4: Patch Tom's cells_B costs (same-rec, clustering, run penalty)       #
# =========================================================================== #
# Use the group structure to identify uid_rights sharing each offset.
print("\nComputing cost modifications on Tom's cells ...")
t0 = time.time()

nz_mask = rows > 0
uid_right_vals = np.where(nz_mask)[0]
start_vals = rows[uid_right_vals].astype(np.int64)

sort_order = np.argsort(start_vals, kind='stable')
sorted_starts = start_vals[sort_order]
sorted_rights = uid_right_vals[sort_order]

unique_starts, inverse = np.unique(sorted_starts, return_inverse=True)
n_groups = len(unique_starts)
print("  %d distinct offsets, %d uid_rights" % (n_groups, len(uid_right_vals)))

# For each group, collect fidx set
group_fidx_sets = [set() for _ in range(n_groups)]
group_rep_fidx = np.zeros(n_groups, dtype=np.int64)
for i in range(len(sorted_rights)):
    ur = int(sorted_rights[i])
    gi = inverse[i]
    if ur < N_UNITS:
        fi = int(uid_fidx[ur])
        group_fidx_sets[gi].add(fi)
        if group_rep_fidx[gi] == 0 and rec_has_feat[fi]:
            group_rep_fidx[gi] = fi

data_indices = np.where(data_mask)[0]
gi_for_data = np.searchsorted(unique_starts, data_indices, side='right') - 1

uid_left_all = cells_A[data_indices]
valid_left = uid_left_all < N_UNITS
fidx_left_all = np.zeros(len(data_indices), dtype=np.uint16)
fidx_left_all[valid_left] = uid_fidx[uid_left_all[valid_left]]

new_cells_B = cells_B.copy()

sort_by_group = np.argsort(gi_for_data, kind='stable')
sorted_gi = gi_for_data[sort_by_group]
sorted_di = data_indices[sort_by_group]
sorted_fl = fidx_left_all[sort_by_group]
sorted_vl = valid_left[sort_by_group]

group_boundaries = np.searchsorted(sorted_gi, np.arange(n_groups))
group_boundaries = np.append(group_boundaries, len(sorted_gi))

n_same_rec = 0
n_similar_rec = 0
n_tom_fallback = 0

for gi in range(n_groups):
    lo = int(group_boundaries[gi])
    hi = int(group_boundaries[gi + 1])
    if lo >= hi:
        continue
    fidx_set = group_fidx_sets[gi]
    chunk_di = sorted_di[lo:hi]
    chunk_fl = sorted_fl[lo:hi]
    chunk_vl = sorted_vl[lo:hi]

    fidx_arr = np.array(list(fidx_set), dtype=np.uint16)
    same = chunk_vl & np.isin(chunk_fl, fidx_arr)
    new_cells_B[chunk_di[same]] = 0.0
    n_same_rec += int(same.sum())

    cross = ~same
    n_cross_rec = int(cross.sum())

    if CLUSTER_DISCOUNT < 1.0 and CLUSTER_DIST_THRESH > 0:
        rep_fi = int(group_rep_fidx[gi])
        if rep_fi > 0 and rec_has_feat[rep_fi]:
            right_vec = rec_mean[rep_fi]
            cross_valid = cross & chunk_vl
            cross_indices = np.where(cross_valid)[0]
            for ci in cross_indices:
                fl = int(chunk_fl[ci])
                if rec_has_feat[fl]:
                    diff = rec_mean[fl] - right_vec
                    rec_dist = float(np.sqrt(np.dot(diff, diff)))
                    if rec_dist < CLUSTER_DIST_THRESH:
                        orig = float(new_cells_B[chunk_di[ci]])
                        new_cells_B[chunk_di[ci]] = orig * CLUSTER_DISCOUNT
                        n_similar_rec += 1
                    else:
                        n_tom_fallback += 1
                else:
                    n_tom_fallback += 1
            n_tom_fallback += n_cross_rec - int(cross_valid.sum())
        else:
            n_tom_fallback += n_cross_rec
    else:
        n_tom_fallback += n_cross_rec

    if gi % 20000 == 0 and gi > 0:
        print("    %d/%d groups (%.0f%%)" % (gi, n_groups, 100.0 * gi / n_groups))

# Run-potential penalty: walk consecutive cells from each uid_right's offset.
# In the perfect hash, this broadly penalizes the dense region around each
# low-run uid_right -- exactly what reduces recording-switch stutter.
n_penalized = 0
if RUN_PENALTY_MULT > 1.0:
    print("  Applying run-potential penalty ...")
    # Pre-filter to penalized uid_rights only
    max_ur = min(n_rows, N_UNITS)
    pen_mask = ((uid_run_potential[:max_ur] > 0) &
                (uid_run_potential[:max_ur] < RUN_PENALTY_MIN) &
                (rows[:max_ur] > 0))
    pen_uids = np.where(pen_mask)[0]
    print("    %d uid_rights to penalize" % len(pen_uids))

    # Pre-compute penalty per run-potential level
    for ur in pen_uids:
        rp = int(uid_run_potential[ur])
        penalty = 1.0 + (RUN_PENALTY_MULT - 1.0) * (RUN_PENALTY_MIN - rp) / RUN_PENALTY_MIN
        start = int(rows[ur])
        # Find next SENTINEL using numpy (vectorized scan)
        rest = cells_A[start:]
        sentinel_hits = np.where(rest == SENTINEL)[0]
        end = start + int(sentinel_hits[0]) if len(sentinel_hits) > 0 else n_cells
        # Apply penalty to all non-zero costs in [start, end) vectorized
        region = new_cells_B[start:end]
        pos_mask = region > 0
        n_pos = int(pos_mask.sum())
        if n_pos > 0:
            region[pos_mask] = np.minimum(12.0, region[pos_mask] * penalty)
            n_penalized += n_pos

elapsed = time.time() - t0
print("  Done in %.1fs" % elapsed)
print("  Same-recording:    %d (cost=0.0)" % n_same_rec)
print("  Similar-recording: %d (cost=%.0f%% of Tom)" % (n_similar_rec, CLUSTER_DISCOUNT * 100))
print("  Tom original cost: %d" % n_tom_fallback)
print("  Run-penalized:     %d" % n_penalized)

# =========================================================================== #
#  Step 5: Extend hash for extra units (shared-offset extension)               #
# =========================================================================== #
# All extra uid_rights from every recording share ONE offset pointing into
# a new extension region appended to the cell array.  All same-rec uid_lefts
# get cost=0.0 entries.  Different recordings can share the same cells since
# the stored uid_left value is the same regardless of which recording wrote it.
extra_uids = [uid for uid in range(N_UNITS) if uid >= TOM_N_UNITS and uid_dl[uid] > 0]
n_extra = len(extra_uids)

new_rows = rows.copy()
extra_offset = n_cells  # extension region starts right after Tom's cells

if n_extra > 0:
    print("\n  Adding %d extra units to hash (shared-offset extension) ..." % n_extra)

    # Collect ALL uid_lefts that any extra uid_right needs
    extra_uid_lefts = set()
    for uid_right in extra_uids:
        fidx_r = int(uid_fidx[uid_right])
        for uid_left in units_by_fidx_hash.get(fidx_r, []):
            if uid_left != uid_right:
                extra_uid_lefts.add(uid_left)

    max_extra_ul = max(extra_uid_lefts) if extra_uid_lefts else 0
    ext_size = max_extra_ul + 1  # extension region size

    # Build extension arrays
    ext_cells_A = np.full(ext_size, SENTINEL, dtype=np.uint32)
    ext_cells_B = np.zeros(ext_size, dtype=np.float32)

    for uid_left in extra_uid_lefts:
        ext_cells_A[uid_left] = uid_left
        ext_cells_B[uid_left] = 0.0  # same-rec = free join

    # Set rows for all extra uid_rights to the shared offset
    for uid_right in extra_uids:
        if uid_right < n_rows:
            new_rows[uid_right] = extra_offset

    n_ext_data = int((ext_cells_A != SENTINEL).sum())
    print("  Extension: %d cells (%d data, %d empty)" % (
        ext_size, n_ext_data, ext_size - n_ext_data))
    print("  Shared offset: %d (all %d extra uid_rights)" % (extra_offset, n_extra))

    # Also ensure Tom uid_rights can safely access extra uid_lefts without OOB.
    # Max Tom access: max(rows[tom_ur]) + max(extra_uid_left).
    max_tom_rows = int(rows[:TOM_N_UNITS].max())
    max_access = max_tom_rows + max_extra_ul
    if max_access >= extra_offset + ext_size:
        # Need more padding
        extra_padding = max_access - (extra_offset + ext_size) + 1
        print("  Adding %d padding cells for OOB safety" % extra_padding)
        ext_cells_A = np.concatenate([ext_cells_A,
            np.full(extra_padding, SENTINEL, dtype=np.uint32)])
        ext_cells_B = np.concatenate([ext_cells_B,
            np.zeros(extra_padding, dtype=np.float32)])

    # Merge: Tom's cells + extension
    final_cells_A = np.concatenate([cells_A, ext_cells_A])
    final_cells_B = np.concatenate([new_cells_B, ext_cells_B])
    new_n_cells = len(final_cells_A)
else:
    print("\nNo extra units.")
    final_cells_A = cells_A
    final_cells_B = new_cells_B
    new_n_cells = n_cells

# Fill SENTINEL positions with MISS_PENALTY in cells_B.
# Combined with Frida jne-NOP patch, this makes the engine read MISS_PENALTY
# as the join cost for any transition without a precomputed hash entry.
sentinel_mask = final_cells_A == SENTINEL
final_cells_B[sentinel_mask] = MISS_PENALTY
n_total_data = int((~sentinel_mask).sum())
n_penalty = int(sentinel_mask.sum())
print("  Final: n_cells=%d (%d data, %d penalty=%.1f) -- was %d" % (
    new_n_cells, n_total_data, n_penalty, MISS_PENALTY, n_cells))

# =========================================================================== #
#  Step 6: Rebuild hash chunk                                                  #
# =========================================================================== #
print("\nRebuilding hash chunk ...")

head_body = struct.pack('<II', n_rows, new_n_cells)
head_chunk = pack_chunk(b'head', head_body)

rows_chunk = pack_chunk(b'rows', new_rows.tobytes())

cell_body = final_cells_A.tobytes() + final_cells_B.tobytes()
cell_chunk = pack_chunk(b'cell', cell_body)

new_hash_payload = head_chunk + rows_chunk + cell_chunk
new_hash_chunk = pack_chunk(b'hash', new_hash_payload)

print("  Hash chunk: %d bytes (%.1f MB)" % (len(new_hash_chunk), len(new_hash_chunk) / 1e6))

# =========================================================================== #
#  Step 7: Patch mara.vin (hash chunk may change size)                         #
# =========================================================================== #
print("\nPatching mara.vin ...")

mara_hash_ds, mara_hash_sz = vin_chunks[b'hash']
hash_chunk_start = mara_hash_ds - 8
hash_chunk_end   = mara_hash_ds + mara_hash_sz + (mara_hash_sz & 1)

new_vin = bytes(vin[:hash_chunk_start]) + new_hash_chunk + bytes(vin[hash_chunk_end:])

new_riff_size = len(new_vin) - 8
new_vin = new_vin[:4] + struct.pack('<I', new_riff_size) + new_vin[8:]

encoded = bytes(np.frombuffer(new_vin, dtype=np.uint8) ^ XOR_KEY)
tmp_path = VIN_PATH + '.hash_tmp'
with open(tmp_path, 'wb') as f:
    f.write(encoded)
os.replace(tmp_path, VIN_PATH)

print("  Wrote %s  (%d bytes, %.1f MB)" % (VIN_PATH, len(encoded), len(encoded) / 1e6))
print("\nDone -- hash rebuilt with perfect hash placement.")
print("  Includes %d extra units with same-rec entries." % n_extra)
print("Restart Speechify server and run synthesis test.")
print("="*252)
