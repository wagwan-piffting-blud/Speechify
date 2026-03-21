"""
Extract VDB audio as individual WAV files for voice conversion.

Decodes the XOR-encrypted VDB, converts u-law to PCM16, and saves each
recording as an 8kHz mono WAV. Also generates .lab files from transcripts
or phone sequences where available.

Usage:
    python extract_vdb_wavs.py [voice_name]
    python extract_vdb_wavs.py mara
    python extract_vdb_wavs.py craig
"""
import audioop
import glob
import os
import struct
import wave
import numpy as np

XOR = 0xCE
UNIT_SIZE = 29
N_UNITS = 169579

PHONE_LABELS = [
    'aa', 'ae', 'ah', 'ao', 'aw', 'ax', 'ay', 'b', 'ch', 'dx',
    'd',  'dh', 'eh', 'el', 'er', 'en', 'ey', 'f', 'g',  'hh',
    'ih', 'ix', 'iy', 'jh', 'k',  'l',  'm',  'n', 'ng', 'ow',
    'oy', 'p',  'pau','r',  's',  'sh', 't',  'th','uh', 'uw',
    'v',  'w',  'xx', 'y',  'z',  'zh',
]


def _detect_proj_root():
    """Find project root from cwd/script dir by looking for expected layout."""
    starts = (os.path.abspath(os.getcwd()), os.path.abspath(os.path.dirname(__file__)))
    seen = set()
    for start in starts:
        cur = start
        while cur and cur not in seen:
            seen.add(cur)
            if os.path.isfile(os.path.join(cur, "bin", "spfy_dumpwav32_8khz.exe")):
                return cur
            parent = os.path.dirname(cur)
            if parent == cur:
                break
            cur = parent
    return os.path.abspath(os.path.dirname(__file__))


def load_xor(path):
    raw = bytearray(open(path, 'rb').read())
    for i in range(len(raw)):
        raw[i] ^= XOR
    return bytes(raw)


def riff_chunks(data, start=12, end=None):
    end = end or len(data)
    pos = start
    while pos + 8 <= end:
        tag = data[pos:pos+4]
        sz = struct.unpack_from('<I', data, pos+4)[0]
        yield tag, pos+8, sz
        pos += 8 + sz + (sz & 1)


def parse_filenames(vin):
    for tag, ds, sz in riff_chunks(vin):
        if tag == b'feat':
            feat = vin[ds:ds+sz]
            fn_idx = feat.find(b'filename')
            if fn_idx < 0:
                return {}
            fn_count = struct.unpack_from('<I', feat, fn_idx+8)[0]
            p = fn_idx + 12
            fnames = {}
            for _ in range(fn_count):
                nlen = struct.unpack_from('<H', feat, p)[0]
                name = feat[p+2:p+2+nlen].decode('latin-1', errors='replace').rstrip('\x00')
                sid = struct.unpack_from('<I', feat, p+2+nlen)[0]
                fnames[sid] = name
                p += 2 + nlen + 4
            return fnames
    return {}


def parse_vdb_indx(vdb):
    """Parse VDB indx to get recording name -> (offset, size)."""
    data_ds = None
    indx_ds = None
    for tag, ds, sz in riff_chunks(vdb):
        if tag == b'data':
            data_ds = ds
        elif tag == b'indx':
            indx_ds = ds
    if data_ds is None or indx_ds is None:
        return {}, 0

    count = struct.unpack_from('<I', vdb, indx_ds)[0]
    pos = indx_ds + 4
    entries = []
    for _ in range(count + 1):
        off = struct.unpack_from('<I', vdb, pos)[0]
        nlen = struct.unpack_from('<H', vdb, pos+4)[0]
        name = vdb[pos+6:pos+6+nlen].decode('latin-1', errors='replace').rstrip('\x00')
        entries.append((off, name))
        pos += 6 + nlen

    recs = {}
    for i in range(len(entries) - 1):
        off, name = entries[i]
        next_off = entries[i+1][0]
        sz = next_off - off
        if sz > 0 and name:
            recs[name] = (data_ds + off, sz)
    return recs, data_ds


def main():
    import sys
    voice_name = sys.argv[1] if len(sys.argv) > 1 else "mara"

    PROJ = _detect_proj_root()
    SOURCE_VIN = os.path.join(PROJ, "en-US", voice_name, voice_name + ".vin")
    SOURCE_VDB = os.path.join(PROJ, "en-US", voice_name, voice_name + "8.vdb")
    OUT_DIR = os.path.join(PROJ, "en-US", voice_name, "output", "decoded_vdb")

    print("[*] Loading %s VDB..." % voice_name.capitalize())
    vdb = load_xor(SOURCE_VDB)
    vdb_recs, vdb_data_ds = parse_vdb_indx(vdb)
    print("  %d recordings in %s VDB" % (len(vdb_recs), voice_name.capitalize))

    print("[*] Loading %s VIN..." % voice_name.capitalize())
    vin = load_xor(SOURCE_VIN)
    filenames = parse_filenames(vin)
    print("  %d filenames in %s VIN" % (len(filenames), voice_name.capitalize()))

    # Extract ALL Tom recordings
    all_names = set(vdb_recs.keys())
    print("  %d total Tom recordings to extract" % len(all_names))

    if not all_names:
        print("No recordings to extract!")
        return

    # Get phone sequences from unit table for .lab files
    print("[*] Parsing unit table for phone sequences...")
    unit_ds = None
    for tag, ds, sz in riff_chunks(vin):
        if tag == b'unit':
            for t2, d2, s2 in riff_chunks(vin, ds, ds+sz):
                if t2 == b'data' and s2 == N_UNITS * UNIT_SIZE:
                    unit_ds = d2
                    break

    rec_phones = {}
    if unit_ds:
        from collections import defaultdict
        phones_by_fidx = defaultdict(list)
        for uid in range(N_UNITS):
            base = unit_ds + uid * UNIT_SIZE
            fidx = struct.unpack_from('<H', vin, base + 4)[0]
            lp = struct.unpack_from('<H', vin, base + 6)[0]
            pc = vin[base + 20]
            is1 = vin[base + 21]
            if is1 and pc < len(PHONE_LABELS):
                phones_by_fidx[fidx].append((lp, PHONE_LABELS[pc]))
        for fidx, phones in phones_by_fidx.items():
            name = filenames.get(fidx, '')
            if name:
                phones.sort()
                rec_phones[name] = ' '.join(p for _, p in phones)

    # Extract and save Tom's audio as WAVs
    os.makedirs(OUT_DIR, exist_ok=True)
    extracted = 0
    with_phones = 0

    for name in sorted(all_names):
        if name not in vdb_recs:
            continue
        off, sz = vdb_recs[name]
        ulaw_bytes = vdb[off:off+sz]

        # Decode u-law to PCM16
        pcm16 = audioop.ulaw2lin(ulaw_bytes, 2)

        # Save as 8kHz WAV
        wav_path = os.path.join(OUT_DIR, name + ".wav")
        with wave.open(wav_path, 'wb') as wf:
            wf.setnchannels(1)
            wf.setsampwidth(2)
            wf.setframerate(8000)
            wf.writeframes(pcm16)
        extracted += 1

        # Generate .lab file from phone sequence if available
        if name in rec_phones:
            lab_path = os.path.join(OUT_DIR, name + ".lab")
            with open(lab_path, 'w') as f:
                f.write(rec_phones[name])
            with_phones += 1

    print("\n" + "=" * 70)
    print("EXTRACTION SUMMARY")
    print("=" * 70)
    print("  Extracted: %d WAVs to %s" % (extracted, OUT_DIR))
    print("  With phone labels: %d" % with_phones)
    print("  No label: %d" % (extracted - with_phones))
    print()
    print("Next steps:")
    print("  1. Run AudioSR upscaling: python audiosr_batch.py %s" % voice_name)
    print("  2. Run RVC batch: python rvc_batch.py %s" % voice_name)
    print("  3. Build voice skin: python build_voice_skin.py %s" % voice_name)


if __name__ == '__main__':
    main()
