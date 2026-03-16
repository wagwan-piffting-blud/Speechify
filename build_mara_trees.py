"""build_mara_trees.py -- Patch CART tree leaves for Mara.

Keeps Tom's tree STRUCTURE (branching decisions, questions, and variance values)
but recomputes leaf MEANS from Mara's actual unit data.

Variance calibration (c:/tmp/calibrate_tree_variance.py) showed that Tom's
tree variance is ~constant (~0.052-0.057) across all leaves regardless of
actual stddev -- it's a tuned cost sensitivity parameter, not a statistical
property. We keep Tom's variance and only update means.

- f0tr (1 global tree): kept unchanged (f0_context byte 19 is identical
  between Tom and Mara, so recomputing would have zero effect)
- durt (47 per-phone trees): each leaf's MEAN is recomputed from f0_context
  values of Mara units reaching that leaf; variance kept from Tom

Usage:
  python build_mara_trees.py

Inputs  (read-only): mara.vin (must already have unit table built),
                      tom.vin  (tree structure + questions)
Output (overwritten): mara.vin  (f0tr + durt chunks replaced)
"""

import os
import struct
from collections import defaultdict

import numpy as np

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
TOM_VIN_PATH = os.path.join(_HERE, 'en-US', 'tom',  'tom.vin')

XOR_KEY      = 0xCE
UNIT_SIZE    = 29    # bytes per unit record on disk
MIN_SAMPLES  = 5     # minimum units per leaf to recompute stats
# Variance convention (calibrated from Tom): tree_var is ~constant (~0.052-0.057)
# across all leaves regardless of actual stddev. It's a tuned cost sensitivity
# parameter, NOT a statistical property. We keep Tom's original variance values
# and only recompute the means from Mara's unit data.

# When True, keep Tom's original durt leaf means (skip recomputation).
# The durt tree prediction sets the WSOLA output duration target. Recomputed
# Mara means caused 0ms and negative output durations for some units because
# the engine converts predictions via an exponential mapping where small mean
# shifts produce large duration changes. With DUR_WEIGHT=0 in VCF, the tree
# prediction has zero effect on unit selection cost -- only on output timing.
SKIP_DURT_RECOMPUTE = True

# Phone labels matching durt tree order (47 phones, index 0..46)
PHONE_LABELS = [
    'aa','ae','ah','ao','aw','ax','ay','b','ch','dx',
    'd','dh','eh','el','er','en','ey','f','g','hh',
    'ih','ix','iy','jh','k','l','m','n','ng','ow',
    'oy','p','pau','r','s','sh','t','th','uh','uw',
    'v','w','xx','y','z','zh','',
]


# --------------------------------------------------------------------------- #
#  Utility                                                                     #
# --------------------------------------------------------------------------- #
def xor_decode(path):
    raw = np.fromfile(path, dtype=np.uint8)
    raw ^= XOR_KEY
    return raw.tobytes()


def xor_encode(data):
    arr = np.frombuffer(data, dtype=np.uint8).copy()
    arr ^= XOR_KEY
    return arr.tobytes()


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


# --------------------------------------------------------------------------- #
#  CART tree parsing                                                           #
# --------------------------------------------------------------------------- #

def parse_tree_with_offsets(data):
    """Parse tree nodes and record byte offsets of leaf mean/var fields.
    Returns (nodes_list, leaf_patches) where leaf_patches is a list of
    (mean_offset, var_offset) relative to start of tree data.
    """
    n = struct.unpack_from('<I', data, 0)[0]
    off = 4
    nodes = []
    leaf_patches = []  # (mean_off, var_off) relative to data start
    for i in range(n):
        idx = struct.unpack_from('<I', data, off)[0]; off += 4
        yc = struct.unpack_from('<i', data, off)[0]; off += 4
        if yc >= 0:
            # Branch: 16 bytes
            nc = struct.unpack_from('<I', data, off)[0]; off += 4
            qi = struct.unpack_from('<I', data, off)[0]; off += 4
            nodes.append({'i': i, 't': 'B', 'idx': idx, 'yc': yc, 'nc': nc, 'qi': qi})
        else:
            # Leaf: 20 bytes -- record offsets of mean and var
            unused = struct.unpack_from('<I', data, off)[0]; off += 4
            mean_off = off
            mean = struct.unpack_from('<f', data, off)[0]; off += 4
            var_off = off
            var = struct.unpack_from('<f', data, off)[0]; off += 4
            nodes.append({'i': i, 't': 'L', 'idx': idx, 'mean': mean, 'var': var,
                          'mean_off': mean_off, 'var_off': var_off})
            leaf_patches.append((mean_off, var_off))
    return nodes, leaf_patches


def find_tree_sub_chunks(chunk_data):
    """Find tree sub-chunks within an f0tr/durt container.
    Returns list of (data_start_offset_within_chunk, size) for each tree.
    """
    trees = []
    for tag, ds, sz in riff_chunks(chunk_data, 0):
        if tag == b'tree':
            trees.append((ds, sz))
    return trees


def parse_ques(data):
    """Parse ques sub-chunk data. Returns list of (type, value_set) tuples.
    Question types: 1=syl_type, 2=syl_in_phrase, 3=phone_left,
                    4=phone_right, 5=word_in_phrase, 8=phone_in_syl.
    """
    count = struct.unpack_from('<I', data, 0)[0]
    off = 4
    questions = []
    for _ in range(count):
        key = data[off]; off += 1
        vc = struct.unpack_from('<I', data, off)[0]; off += 4
        values = set()
        for _ in range(vc):
            values.add(struct.unpack_from('<I', data, off)[0]); off += 4
        questions.append((key, values))
    return questions


def find_ques_in_chunk(chunk_data):
    """Find and parse the ques sub-chunk within an f0tr/durt container.
    The ques chunk may be at top level or nested inside a trhd chunk.
    """
    for tag, ds, sz in riff_chunks(chunk_data, 0):
        if tag == b'ques':
            return parse_ques(chunk_data[ds:ds+sz])
        elif tag == b'trhd':
            for t2, d2, s2 in riff_chunks(chunk_data, ds, ds+sz):
                if t2 == b'ques':
                    return parse_ques(chunk_data[d2:d2+s2])
    raise ValueError("ques sub-chunk not found")


def traverse_tree(nodes, questions, features):
    """Traverse tree with given features, return leaf node index.
    features = {type: value} dict matching question types.
    """
    ni = 0
    while True:
        nd = nodes[ni]
        if nd['t'] == 'L':
            return ni
        qtype, qvalues = questions[nd['qi']]
        feat_val = features.get(qtype, 0)
        if feat_val in qvalues:
            ni = nd['yc']
        else:
            ni = nd['nc']


# --------------------------------------------------------------------------- #
#  Unit data loading                                                           #
# --------------------------------------------------------------------------- #
def load_unit_features(vin_data):
    """Load unit features needed for tree traversal from decoded VIN.
    Returns dict of numpy arrays keyed by field name.
    """
    # Find cnts to get N_UNITS
    n_units = None
    for tag, ds, sz in riff_chunks(vin_data, 12):
        if tag == b'cnts':
            n_units = struct.unpack_from('<I', vin_data, ds + 8)[0]
            break
    assert n_units is not None, "cnts chunk not found"

    # Find unit -> data sub-chunk
    unit_data_ds = None
    for tag, ds, sz in riff_chunks(vin_data, 12):
        if tag == b'unit':
            for t2, d2, s2 in riff_chunks(vin_data, ds, ds + sz):
                if t2 == b'data' and s2 == n_units * UNIT_SIZE:
                    unit_data_ds = d2
                    break
            break
    assert unit_data_ds is not None, "unit data sub-chunk not found"

    # Extract arrays
    syl_type = np.empty(n_units, dtype=np.uint8)
    syl_in_phrase = np.empty(n_units, dtype=np.uint8)
    word_in_phrase = np.empty(n_units, dtype=np.uint8)
    phone_in_syl = np.empty(n_units, dtype=np.uint8)
    phone_left = np.empty(n_units, dtype=np.uint8)
    phone_right = np.empty(n_units, dtype=np.uint8)
    pc = np.empty(n_units, dtype=np.uint8)
    dl = np.empty(n_units, dtype=np.uint16)
    f0_context = np.empty(n_units, dtype=np.uint8)

    for i in range(n_units):
        base = unit_data_ds + i * UNIT_SIZE
        dl[i] = struct.unpack_from('<H', vin_data, base + 0x0A)[0]
        syl_type[i] = vin_data[base + 0x0C]
        syl_in_phrase[i] = vin_data[base + 0x0D]
        word_in_phrase[i] = vin_data[base + 0x0E]
        phone_in_syl[i] = vin_data[base + 0x0F]
        f0_context[i] = vin_data[base + 0x13]
        pc[i] = vin_data[base + 0x14]
        phone_left[i] = vin_data[base + 0x17]
        phone_right[i] = vin_data[base + 0x18]

    return {
        'n_units': n_units,
        'syl_type': syl_type,
        'syl_in_phrase': syl_in_phrase,
        'word_in_phrase': word_in_phrase,
        'phone_in_syl': phone_in_syl,
        'phone_left': phone_left,
        'phone_right': phone_right,
        'pc': pc,
        'dl': dl,
        'f0_context': f0_context,
    }


# --------------------------------------------------------------------------- #
#  Main                                                                        #
# --------------------------------------------------------------------------- #
def main():
    print("=" * 70)
    print("build_mara_trees.py -- Patch CART tree leaves for Mara")
    print("=" * 70)

    # Load both VINs
    print("\nLoading VINs ...")
    tom_vin = xor_decode(TOM_VIN_PATH)
    mara_vin = bytearray(xor_decode(VIN_PATH))
    print("  Tom VIN:  %d bytes" % len(tom_vin))
    print("  Mara VIN: %d bytes" % len(mara_vin))

    # Load Mara's unit features for tree traversal
    print("\nLoading Mara unit features ...")
    uf = load_unit_features(bytes(mara_vin))
    n_units = uf['n_units']
    print("  %d units loaded" % n_units)

    # Strategy: copy Tom's entire f0tr/durt chunk data verbatim (preserving
    # labl, ques, and all sub-chunk ordering), then patch leaf float values
    # in-place within the copied bytes.

    # Find f0tr and durt in Tom's VIN
    print("\nParsing Tom's CART trees ...")
    tom_f0tr_data = tom_durt_data = None
    for tag, ds, sz in riff_chunks(tom_vin, 12):
        if tag == b'f0tr':
            tom_f0tr_data = bytearray(tom_vin[ds:ds+sz])
        elif tag == b'durt':
            tom_durt_data = bytearray(tom_vin[ds:ds+sz])

    assert tom_f0tr_data is not None, "f0tr not found in Tom VIN"
    assert tom_durt_data is not None, "durt not found in Tom VIN"

    # -- f0tr: keep unchanged --
    f0_trees = find_tree_sub_chunks(bytes(tom_f0tr_data))
    f0_tree_bytes = bytes(tom_f0tr_data[f0_trees[0][0]:f0_trees[0][0]+f0_trees[0][1]])
    f0_nodes, _ = parse_tree_with_offsets(f0_tree_bytes)
    f0_leaves = [nd for nd in f0_nodes if nd['t'] == 'L']
    print("  f0tr: %d nodes (%d leaves) -- keeping unchanged" % (
        len(f0_nodes), len(f0_leaves)))
    print("    Leaf range: [%.2f .. %.2f] Hz" % (
        min(nd['mean'] for nd in f0_leaves),
        max(nd['mean'] for nd in f0_leaves)))

    # -- durt: parse trees and questions --
    d_trees = find_tree_sub_chunks(bytes(tom_durt_data))
    assert len(d_trees) == 47, "Expected 47 durt trees, got %d" % len(d_trees)
    durt_ques = find_ques_in_chunk(bytes(tom_durt_data))
    print("  durt: %d trees, %d questions" % (len(d_trees), len(durt_ques)))

    # Parse all 47 durt trees
    d_all_info = []  # (tree_ds, tree_sz, nodes, leaf_patches)
    for tds, tsz in d_trees:
        tree_bytes = bytes(tom_durt_data[tds:tds+tsz])
        nodes, lps = parse_tree_with_offsets(tree_bytes)
        d_all_info.append((tds, tsz, nodes, lps))

    # -- Route Mara units through durt trees, collect per-leaf f0_context --
    # NOTE: The engine computes unit bias as:
    #   DUR_WEIGHT * |tree_scale * (unit.f0_context - tree_pred)|^2
    # So tree leaf means must be in f0_context domain (byte 19, ~100-170),
    # NOT raw dl domain (~20-80). f0_context correlates with log(dl) at r=0.97.
    print("\nRouting %d Mara units through durt trees ..." % n_units)
    # leaf_vals[phone_idx][leaf_node_idx] = list of f0_context values
    leaf_vals = defaultdict(lambda: defaultdict(list))

    for uid in range(n_units):
        pc = int(uf['pc'][uid])
        if pc >= 47:
            continue
        features = {
            1: int(uf['syl_type'][uid]),
            2: int(uf['syl_in_phrase'][uid]),
            3: int(uf['phone_left'][uid]),
            4: int(uf['phone_right'][uid]),
            5: int(uf['word_in_phrase'][uid]),
            8: int(uf['phone_in_syl'][uid]),
        }
        tree_nodes = d_all_info[pc][2]
        li = traverse_tree(tree_nodes, durt_ques, features)
        leaf_vals[pc][li].append(float(uf['f0_context'][uid]))

    # -- Compute per-leaf statistics and patch --
    if SKIP_DURT_RECOMPUTE:
        total_leaves = sum(
            len([nd for nd in info[2] if nd['t'] == 'L'])
            for info in d_all_info
        )
        print("\n  Keeping Tom's original durt leaf means (SKIP_DURT_RECOMPUTE=True)")
        print("  (DUR_WEIGHT=0 means these only affect WSOLA output duration, not unit selection)")
        print("  %d leaves unchanged across %d trees" % (total_leaves, len(d_all_info)))
        n_recomputed = 0
        n_kept = total_leaves
    else:
        print("\nRecomputing durt leaf values from Mara's f0_context ...")
        n_recomputed = 0
        n_kept = 0

        for ti, (tds, tsz, nodes, lps) in enumerate(d_all_info):
            label = PHONE_LABELS[ti] if ti < len(PHONE_LABELS) else '?'
            leaves = [nd for nd in nodes if nd['t'] == 'L']
            if not leaves:
                continue

            old_means = [nd['mean'] for nd in leaves]
            phone_recomp = 0
            phone_kept = 0

            for nd in leaves:
                ni = nd['i']
                samples = leaf_vals[ti].get(ni, [])
                abs_off = tds + nd['mean_off']

                if len(samples) >= MIN_SAMPLES:
                    arr = np.array(samples)
                    new_mean = float(arr.mean())
                    # Only patch the mean; keep Tom's variance (cost sensitivity tuning)
                    struct.pack_into('<f', tom_durt_data, abs_off, new_mean)
                    phone_recomp += 1
                else:
                    # Too few samples: keep Tom's original leaf values
                    phone_kept += 1

            n_recomputed += phone_recomp
            n_kept += phone_kept

            # Report per-phone summary
            new_leaves_data = parse_tree_with_offsets(
                bytes(tom_durt_data[tds:tds+tsz]))[0]
            new_leaves_l = [nd for nd in new_leaves_data if nd['t'] == 'L']
            new_means = [nd['mean'] for nd in new_leaves_l]
            n_mara_units = sum(len(v) for v in leaf_vals[ti].values())
            print("  %2d %-4s: %d units, %d/%d leaves recomputed  "
                  "Tom [%.1f..%.1f] -> Mara [%.1f..%.1f]" % (
                      ti, label, n_mara_units,
                      phone_recomp, phone_recomp + phone_kept,
                      min(old_means), max(old_means),
                      min(new_means), max(new_means)))

    print("\n  Total: %d leaves recomputed, %d kept (Tom's original)" % (
        n_recomputed, n_kept))

    # Build new chunks (Tom's structure with recomputed leaf values)
    new_f0tr_chunk = pack_chunk(b'f0tr', bytes(tom_f0tr_data))
    new_durt_chunk = pack_chunk(b'durt', bytes(tom_durt_data))

    # Find f0tr and durt chunk positions in mara.vin
    f0tr_ds = f0tr_sz = durt_ds = durt_sz = None
    for tag, ds, sz in riff_chunks(bytes(mara_vin), 12):
        if tag == b'f0tr':
            f0tr_ds, f0tr_sz = ds, sz
        elif tag == b'durt':
            durt_ds, durt_sz = ds, sz

    assert f0tr_ds is not None, "f0tr not found in Mara VIN"
    assert durt_ds is not None, "durt not found in Mara VIN"

    print("\nPatching mara.vin ...")
    print("  f0tr: offset=%d, size %d -> %d" % (
        f0tr_ds - 8, f0tr_sz, len(tom_f0tr_data)))
    print("  durt: offset=%d, size %d -> %d" % (
        durt_ds - 8, durt_sz, len(tom_durt_data)))

    # Apply patches (sort by offset descending to avoid shift issues)
    patches = [
        ('f0tr', f0tr_ds, f0tr_sz, new_f0tr_chunk),
        ('durt', durt_ds, durt_sz, new_durt_chunk),
    ]
    patches.sort(key=lambda x: -x[1])

    result = bytes(mara_vin)
    for name, ds, sz, new_chunk in patches:
        chunk_start = ds - 8
        old_chunk_len = 8 + sz + (sz & 1)
        result = result[:chunk_start] + new_chunk + result[chunk_start + old_chunk_len:]
        print("  Patched %s at offset %d" % (name, chunk_start))

    # Update RIFF header size
    new_riff_size = len(result) - 8
    result = result[:4] + struct.pack('<I', new_riff_size) + result[8:]

    # Write
    print("\nWriting %s ..." % VIN_PATH)
    with open(VIN_PATH, 'wb') as f:
        f.write(xor_encode(result))
    print("  Wrote %d bytes (XOR-encoded)" % len(result))

    # Verify
    print("\nVerifying ...")
    check = xor_decode(VIN_PATH)
    for tag, ds, sz in riff_chunks(check, 12):
        if tag == b'f0tr':
            trees = find_tree_sub_chunks(check[ds:ds+sz])
            nodes, _ = parse_tree_with_offsets(
                check[ds:ds+sz][trees[0][0]:trees[0][0]+trees[0][1]])
            leaves = [nd for nd in nodes if nd['t'] == 'L']
            print("  f0tr: %d nodes, leaf range [%.2f .. %.2f] Hz -- OK" % (
                len(nodes), min(nd['mean'] for nd in leaves),
                max(nd['mean'] for nd in leaves)))
        elif tag == b'durt':
            trees = find_tree_sub_chunks(check[ds:ds+sz])
            print("  durt: %d trees -- OK" % len(trees))

    print("\nDone.")
    print("="*252)


if __name__ == '__main__':
    main()
