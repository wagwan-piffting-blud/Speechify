"""Write MODIFIED COPIES of a VCF for cost-weight experiments.

Never touches the live voice file. spfy_synth takes the VCF path as an
argument, so a sweep can point at copies in a scratch directory and the
byte-exact audit keeps running against the original.

VCF is a NIBBLE cipher, not XOR: each plaintext byte becomes two bytes, high
nibble then low, mapped through ENC.

    from vcf_variant import write_variant
    p = write_variant(src, out_dir, {"JOIN_COST_WEIGHT": "0.9"})
"""
from pathlib import Path

import re

ENC = [0xDD, 0xDC, 0xDF, 0xDE, 0xD9, 0xD8, 0xDB, 0xDA,
       0xD5, 0xD4, 0xAC, 0xAF, 0xAE, 0xA9, 0xA8, 0xAB]
DEC = {c: n for n, c in enumerate(ENC)}


def decrypt(b):
    return bytes(((DEC[b[i]] << 4) | DEC[b[i + 1]])
                 for i in range(0, len(b) - 1, 2))


def encrypt(t):
    out = bytearray()
    for byte in t:
        out.append(ENC[(byte >> 4) & 0xF])
        out.append(ENC[byte & 0xF])
    return bytes(out)


def read_params(src):
    xml = decrypt(Path(src).read_bytes()).decode("latin1")
    return dict(re.findall(
        r'<param name="tts\.voiceCfg\.([A-Za-z0-9_]+)">\s*<value>\s*'
        r'([^\s<]*)\s*</value>', xml))


# ⭐ Keys a SHIPPED voice sets that jill omits. The DTD gate rejects UNKNOWN
# names, not absent ones, so adding one of these is safe -- but only these,
# and only because a real voice file is the evidence. Adding a key on the
# strength of "the engine reads it" is exactly how you get an rc=5 arm that
# still renders fine under spfy and dies under the real server.
ATTESTED_ADDITIONS = {
    # en-US/aimara2 (itself a jill-based clone) ships this immediately before
    # </lang>. jill omits it, so the engine falls back to the config
    # CONSTRUCTOR's default of 50 and prunes four times harder.
    "HALFPHONE_CAND_MAX_UNITS": "en-US/aimara2 sets 200",
    # EVERY shipped voice omits this, so the name is attested from the ENGINE
    # rather than from another voice: SWIttsUSel.dll FUN_08e90dc0 stores
    # 0x447a0000 (1000.0) at cfg+0x84 in the constructor, and the config read
    # at 08e9122b looks the key up BY NAME and jumps past the store when the
    # lookup fails. A parameter the engine reads by name is a parameter the
    # engine has.
    # ⚠ Attested for the ENGINE, not for the real server's DTD -- no shipped
    # VCF carries it, so a server arm using this may still exit rc=5. Fine for
    # spfy_synth renders, which parse the XML directly.
    "MISSING_JOIN_COST": "engine default 1000.0 at cfg+0x84; no voice ships it",
}


def write_variant(src, out_path, params, allow_add=False):
    """Copy `src` with `params` (bare names, no tts.voiceCfg prefix) set.

    `allow_add` permits inserting a key from ATTESTED_ADDITIONS that the
    source VCF does not already carry; anything else is still refused.
    """
    xml = decrypt(Path(src).read_bytes()).decode("latin1")
    for name, val in params.items():
        full = f"tts.voiceCfg.{name}"
        pat = (r'(<param name="' + re.escape(full) + r'">\s*<value>)\s*'
               r'[^\s<]*\s*(</value>)')
        new, n = re.subn(pat, r"\g<1> " + str(val) + r" \g<2>", xml)
        if n == 0:
            if not (allow_add and name in ATTESTED_ADDITIONS):
                raise SystemExit(
                    f"{name} not present in VCF; refusing to add it "
                    f"(unknown names make the server exit rc=5 -- see the DTD "
                    f"gate in DLL_ANALYSIS.md)"
                    + ("" if name not in ATTESTED_ADDITIONS else
                       f"\n  -- but {ATTESTED_ADDITIONS[name]}, so pass "
                       f"allow_add=True if you mean it"))
            # Same element shape and position aimara2 uses: last param in
            # <lang>, two-space indent, four-space <value>.
            ins = (f'  <param name="{full}">\n'
                   f'    <value> {val} </value>\n'
                   f'  </param>\n</lang>')
            new, n = re.subn(r'</lang>', ins, xml, count=1)
            if n == 0:
                raise SystemExit(f"{src}: no </lang> to insert {name} before")
        xml = new
    out_path = Path(out_path)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_bytes(encrypt(xml.encode("latin1")))
    return out_path


def verify_roundtrip(src, tmp):
    """Decrypt->encrypt with no edits must reproduce the file byte for byte."""
    raw = Path(src).read_bytes()
    again = encrypt(decrypt(raw))
    ok = again == raw[:len(again)]
    Path(tmp).write_bytes(again)
    return ok, len(raw), len(again)
