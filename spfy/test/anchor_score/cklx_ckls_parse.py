"""Parser for VIN ckls + cklx chunks (PostScoringAdj chunk tables).

Format from reveng/README_TECHNICAL.md (already documented).

cklx: inverted index, key (text) -> posting_ids[]
ckls: per-posting metadata, posting_id -> (span_start_uid, span_end_uid)
      plus filename markers between token records (one per posting).
"""
import struct
import os


def deobfuscate(data: bytes) -> bytes:
    return bytes(b ^ 0xCE for b in data)


def find_chunk(buf: bytes, fourcc: bytes) -> bytes | None:
    """Walk top-level RIFF chunks (after 12-byte RIFF/svin header) and
    return the data of the first chunk matching the fourcc."""
    if len(buf) < 12:
        return None
    p = 12
    while p + 8 <= len(buf):
        cc = buf[p:p+4]
        sz = struct.unpack_from("<I", buf, p + 4)[0]
        if cc == fourcc:
            return buf[p+8:p+8+sz]
        # RIFF chunks are word-aligned
        adv = 8 + sz + (sz & 1)
        p += adv
    return None


def parse_cklx(data: bytes) -> dict[str, dict[str, list[int]]]:
    """Returns {group_name: {key_text: [posting_ids...]}} for _WORD_ / _SYL_."""
    p = 0
    n_groups = struct.unpack_from("<I", data, p)[0]
    p += 4
    out = {}
    for _ in range(n_groups):
        name_len = struct.unpack_from("<H", data, p)[0]
        p += 2
        gname = data[p:p+name_len].decode("ascii")
        p += name_len
        n_entries = struct.unpack_from("<I", data, p)[0]
        p += 4
        entries = {}
        for _ in range(n_entries):
            klen = struct.unpack_from("<H", data, p)[0]
            p += 2
            key = data[p:p+klen].decode("ascii", errors="replace")
            p += klen
            n_post = struct.unpack_from("<I", data, p)[0]
            p += 4
            posts = list(struct.unpack_from(f"<{n_post}I", data, p))
            p += 4 * n_post
            entries[key] = posts
        out[gname] = entries
    return out


def parse_ckls(data: bytes) -> dict[str, list[tuple[int, int, str]]]:
    """Returns {group_name: [(span_start, span_end, token_text), ...]}
    indexed by posting_id.

    Records alternate strictly (token, filename, token, filename, ...).
    Final filename has no trailing u32.
    """
    p = 0
    n_groups = struct.unpack_from("<I", data, p)[0]
    p += 4
    out = {}
    for _ in range(n_groups):
        name_len = struct.unpack_from("<H", data, p)[0]
        p += 2
        gname = data[p:p+name_len].decode("ascii")
        p += name_len
        token_count = struct.unpack_from("<I", data, p)[0]
        p += 4
        _unk0 = struct.unpack_from("<I", data, p)[0]
        p += 4
        spans: list[tuple[int, int, str]] = []
        for k in range(token_count):
            # token record: u16 len + text + u32 ss + u32 se
            tlen = struct.unpack_from("<H", data, p)[0]
            p += 2
            txt = data[p:p+tlen].decode("ascii", errors="replace")
            p += tlen
            ss = struct.unpack_from("<I", data, p)[0]
            p += 4
            se = struct.unpack_from("<I", data, p)[0]
            p += 4
            spans.append((ss, se, txt))
            # filename record: u16 len + text + u32 file_id (optional on final)
            flen = struct.unpack_from("<H", data, p)[0]
            p += 2
            p += flen
            if k + 1 < token_count:
                p += 4   # trailing file_id
        out[gname] = spans
    return out


def main():
    vin_path = os.path.expanduser("~/Documents/Speechify/en-US/tom/tom.vin")
    with open(vin_path, "rb") as f:
        raw = f.read()
    raw = deobfuscate(raw)
    cklx_data = find_chunk(raw, b"cklx")
    ckls_data = find_chunk(raw, b"ckls")
    if not cklx_data or not ckls_data:
        print("missing chunks")
        return

    cklx = parse_cklx(cklx_data)
    ckls = parse_ckls(ckls_data)

    print(f"cklx groups: {list(cklx.keys())}")
    print(f"ckls groups: {list(ckls.keys())}")
    print(f"_SYL_ entries: cklx={len(cklx['_SYL_'])} keys, "
          f"ckls={len(ckls['_SYL_'])} postings")
    print(f"_WORD_ entries: cklx={len(cklx['_WORD_'])} keys, "
          f"ckls={len(ckls['_WORD_'])} postings")

    # Validate cross-check: for a few cklx keys, all posting_ids should
    # point to ckls entries with matching token_text.
    print("\n--- cross-check sanity ---")
    for probe in ("hh_eh", "l_ow", "hello"):
        for grp in ("_SYL_", "_WORD_"):
            if probe in cklx[grp]:
                for pid in cklx[grp][probe][:3]:
                    ss, se, txt = ckls[grp][pid]
                    print(f"  {grp}[{probe!r}] pid={pid}: "
                          f"ss={ss} se={se} ckls_txt={txt!r} "
                          f"{'OK' if txt == probe else 'MISMATCH'}")

    # Now validate against captured viterbi_dp anchor cands.
    # text_001 utt 0 slot 8 = Syllable "_he" tree slot, n_cands=1,
    # uid=107926, join_key=107929. So expected: "hh_eh" syllable in
    # cklx._SYL_, posting whose (ss, se) = (107926, 107929).
    print("\n--- text_001 utt 0 slot 8 anchor cand validation ---")
    found = False
    for pid in cklx["_SYL_"].get("hh_eh", []):
        ss, se, _ = ckls["_SYL_"][pid]
        if ss == 107926 and se == 107929:
            print(f"  hh_eh posting {pid} (ss=107926 se=107929) MATCHES "
                  f"engine cand uid=107926 join_key=107929")
            found = True
            break
    if not found:
        print("  NO MATCH for ss=107926, se=107929 in hh_eh postings!")
        print(f"  hh_eh postings + their spans:")
        for pid in cklx["_SYL_"].get("hh_eh", []):
            ss, se, _ = ckls["_SYL_"][pid]
            print(f"    pid {pid}: ({ss}, {se})")


if __name__ == "__main__":
    main()
