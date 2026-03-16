"""vcf_edit.py -- Decrypt a .vcf file, optionally patch weights, re-encrypt.

Usage:
  python vcf_edit.py --in PATH.vcf --out PATH.vcf [--dry-run]
  python vcf_edit.py --in PATH.vcf --dump        (just print plaintext)

Cipher: nibble expansion (2:1)
  Each plaintext byte -> 2 cipher bytes (high nibble then low nibble).
  Substitution table maps nibble 0-F to a specific cipher byte.
"""
import argparse
import re

import psutil
for proc in psutil.process_iter(['pid', 'name']):
    if proc.info['name'] and proc.info['name'].lower() in ("speechify.exe", "speechify"):
        proc.kill()
        proc.wait(timeout=5)
        print("Killed Speechify process (pid %d) to free file locks." % proc.info['pid'])

# Nibble -> cipher byte (high nibble first, then low nibble)
ENC_TABLE = [
    0xDD, 0xDC, 0xDF, 0xDE,   # nibbles 0-3
    0xD9, 0xD8, 0xDB, 0xDA,   # nibbles 4-7
    0xD5, 0xD4, 0xAC, 0xAF,   # nibbles 8-B
    0xAE, 0xA9, 0xA8, 0xAB,   # nibbles C-F
]
DEC_TABLE = {v: i for i, v in enumerate(ENC_TABLE)}


def decrypt(data: bytes) -> bytes:
    if len(data) % 2 != 0:
        raise ValueError("VCF data length must be even (nibble pairs)")
    out = bytearray(len(data) // 2)
    for i in range(0, len(data), 2):
        hi = DEC_TABLE[data[i]]
        lo = DEC_TABLE[data[i + 1]]
        out[i // 2] = (hi << 4) | lo
    return bytes(out)


def encrypt(text: bytes) -> bytes:
    out = bytearray(len(text) * 2)
    for i, b in enumerate(text):
        out[i * 2]     = ENC_TABLE[(b >> 4) & 0xF]
        out[i * 2 + 1] = ENC_TABLE[b & 0xF]
    return bytes(out)


def patch_weight(xml: str, name: str, new_val: str) -> str:
    """Replace <value> X </value> immediately after <param name="NAME">.
    If the param doesn't exist, insert it before </lang>."""
    pattern = r'(<param name="' + re.escape(name) + r'">\s*<value>)\s*[^\s<]+\s*(</value>)'
    replacement = r'\g<1> ' + new_val + r' \2'
    new_xml, n = re.subn(pattern, replacement, xml)
    if n == 0:
        # Insert new param before closing </lang>
        insert = '  <param name="' + name + '">\n    <value> ' + new_val + ' </value>\n  </param>\n'
        new_xml = xml.replace('</lang>', insert + '</lang>', 1)
        if new_xml != xml:
            print(f"  ADDED {name} = {new_val}")
        else:
            print(f"  WARNING: could not add '{name}' to VCF")
    else:
        print(f"  Patched {name} = {new_val}")
    return new_xml


# Patches to apply.  Keys are full param names from the VCF.
PATCHES = {
    'tts.voiceCfg.gender':                 'female',
    'tts.voiceCfg.JOIN_COST_WEIGHT':       '0.7',    # Tom's original (weight*0=0 for misses, ineffective)
    'tts.voiceCfg.JOIN_COST_OFFSET':       '0.2',    # Tom's original (was 0.15)
    'tts.voiceCfg.ABS_F0_WEIGHT':         '0.2',     # Tom's original
    'tts.voiceCfg.DUR_WEIGHT':            '0.3',     # Tom's original (was 0.2)
    'tts.voiceCfg.CHUNK_BIAS_WEIGHT':     '0.25',    # Tom's original (was 0.5)
    'tts.voiceCfg.UNIT_BIAS_WEIGHT':      '0.25',    # Tom's original (was 0.5)
    'tts.voiceCfg.HALFPHONE_CAND_PRUNE_THRESH': '0.8',  # Tom's original
    'tts.voiceCfg.HALFPHONE_CAND_MAX_UNITS': '50',    # engine default (200 made stutter worse)
    'tts.voiceCfg.CONTEXT_COST_WEIGHT':    '1.0',    # Tom's original
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--in',   required=True, dest='inp', help='Input .vcf file')
    ap.add_argument('--out',  dest='out',  default=None, help='Output .vcf file (re-encrypted)')
    ap.add_argument('--dump', action='store_true', help='Just print plaintext, no output')
    ap.add_argument('--dry-run', action='store_true', help='Print patched XML, do not write')
    args = ap.parse_args()

    with open(args.inp, 'rb') as f:
        raw = f.read()
    xml = decrypt(raw).decode('iso-8859-1')

    if args.dump:
        print(xml)
        return

    print(f"Decrypted {args.inp}  ({len(raw)} bytes -> {len(xml)} chars)")

    # Show current weight values
    print("\nCurrent weights:")
    show = list(PATCHES.keys()) + ['tts.voiceCfg.DUR_WEIGHT', 'tts.voiceCfg.CONTEXT_COST_WEIGHT']
    for name in show:
        m = re.search(r'<param name="' + re.escape(name) + r'">\s*<value>\s*([^\s<]+)\s*</value>', xml)
        print(f"  {name:45s} = {m.group(1) if m else '(not found)'}")

    # Apply patches
    print("\nApplying patches:")
    for name, val in PATCHES.items():
        xml = patch_weight(xml, name, val)

    if args.dry_run:
        print("\n--- Patched XML (first 3000 chars) ---")
        print(xml[:3000])
        return

    if not args.out:
        print("No --out specified; use --dry-run to preview or --out to write.")
        return

    enc = encrypt(xml.encode('iso-8859-1'))
    with open(args.out, 'wb') as f:
        f.write(enc)
    print(f"\nWrote re-encrypted VCF -> {args.out}  ({len(enc)} bytes)")


if __name__ == '__main__':
    main()
