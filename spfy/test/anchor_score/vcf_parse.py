"""Decrypt VCF (nibble cipher) + parse XML to extract proscost matrices.

Cipher table from reveng/vcf_edit.py (DO NOT import that file - it
kills the engine on load).
"""
import re
from xml.etree import ElementTree as ET
import os

# Nibble -> cipher byte (forward).
_TABLE = [
    0xDD, 0xDC, 0xDF, 0xDE,
    0xD9, 0xD8, 0xDB, 0xDA,
    0xD5, 0xD4, 0xAC, 0xAF,
    0xAE, 0xA9, 0xA8, 0xAB,
]
# Cipher byte -> nibble (inverse).
_DEC = [0] * 256
for nib, cb in enumerate(_TABLE):
    _DEC[cb] = nib


def decrypt_vcf(data: bytes) -> bytes:
    if len(data) % 2 != 0:
        raise ValueError("VCF length must be even")
    out = bytearray(len(data) // 2)
    for i in range(0, len(data), 2):
        out[i // 2] = (_DEC[data[i]] << 4) | _DEC[data[i + 1]]
    return bytes(out)


def parse_proscost(vcf_xml: str) -> dict[str, dict]:
    """Returns {matrix_name: {row_name: {col_name: value}}}.

    VCF stores proscost as:
      <param name="tts.voiceCfg.proscost.<Matrix>.<Row>">
        <namedValue name="<Col>"> int_or_float </namedValue>
        ...
      </param>
    """
    # Standalone XML parse may fail if root not single. Wrap in fake root.
    xml = "<root>" + vcf_xml + "</root>"
    try:
        root = ET.fromstring(xml)
    except ET.ParseError:
        # Fall back to regex extraction
        return _proscost_via_regex(vcf_xml)

    out: dict[str, dict[str, dict[str, float]]] = {}
    for param in root.iter("param"):
        nm = param.get("name") or ""
        m = re.match(r"tts\.voiceCfg\.proscost\.([^.]+)\.([^.]+)$", nm)
        if not m:
            continue
        matrix_name, row_name = m.group(1), m.group(2)
        out.setdefault(matrix_name, {}).setdefault(row_name, {})
        for nv in param.iter("namedValue"):
            col = nv.get("name") or ""
            txt = (nv.text or "").strip()
            try:
                v = float(txt)
            except ValueError:
                continue
            out[matrix_name][row_name][col] = v
    return out


def _proscost_via_regex(vcf_text: str) -> dict:
    """Tolerant fallback: extract <param name="tts.voiceCfg.proscost.X.Y">
    blocks and their <namedValue name="C"> Z </namedValue> children."""
    out: dict = {}
    pat = re.compile(
        r'<param[^>]*name="tts\.voiceCfg\.proscost\.([^."]+)\.([^."]+)"'
        r'[^>]*>(.*?)</param>',
        re.DOTALL)
    nv_pat = re.compile(
        r'<namedValue[^>]*name="([^"]+)"[^>]*>\s*([\-\d.]+)\s*</namedValue>')
    for m in pat.finditer(vcf_text):
        mat, row, body = m.group(1), m.group(2), m.group(3)
        out.setdefault(mat, {}).setdefault(row, {})
        for n in nv_pat.finditer(body):
            try:
                out[mat][row][n.group(1)] = float(n.group(2))
            except ValueError:
                pass
    return out


def matrices_to_arrays(proscost: dict, kind_to_engine_idx: dict[str, int]):
    """Convert {matrix_name: {row: {col: val}}} into engine-indexed arrays.

    Returns dict of {engine_matrix_idx: {row_names: [...], col_names: [...],
                                          data: [[float,...], ...]}}.
    """
    out = {}
    for mname, row_dict in proscost.items():
        # All rows should have same column set (canonical ordering).
        all_cols = []
        seen = set()
        for r, cols in row_dict.items():
            for c in cols:
                if c not in seen:
                    seen.add(c)
                    all_cols.append(c)
        rows = list(row_dict.keys())
        data = []
        for r in rows:
            row_data = []
            for c in all_cols:
                row_data.append(row_dict[r].get(c, 0.0))
            data.append(row_data)
        out[mname] = {
            "row_names": rows,
            "col_names": all_cols,
            "data": data,
        }
    return out


if __name__ == "__main__":
    vcf_path = os.path.expanduser("~/Documents/Speechify/en-US/tom/tom.vcf")
    with open(vcf_path, "rb") as f:
        cipher = f.read()
    plain = decrypt_vcf(cipher)
    text = plain.decode("utf-8", errors="replace")
    proscost = parse_proscost(text)
    print(f"matrices found: {list(proscost.keys())}")
    for mname, rows in proscost.items():
        cols = next(iter(rows.values()), {})
        print(f"  {mname}: {len(rows)} rows x {len(cols)} cols, "
              f"first row={list(rows.keys())[0]}")
