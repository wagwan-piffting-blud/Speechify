#!/usr/bin/env python
"""Derive the trigger word for each cohort record.

Inputs:
  --cohort   c:/tmp/03-09-cohort-9a.jsonl   (or any cohort JSONL)
  --corpus   spfy/test/oracle/corpus.jsonl
  --dumpwav  bin/spfy_dumpwav.exe
  --out      c:/tmp/03-09-cohort-words.jsonl

Behaviour: for each record, runs `dumpwav --g2p "<token>"` for every
word-token in the corpus entry's `text` field, building a cumulative
phoneme-offset map. The token whose [offset, offset+N) range covers
`phoneme_idx = slot_idx // 2` is the word.

Caches per-(corpus_id, token) ARPAbet output so we only invoke dumpwav
once per unique (corpus, word). ASCII-only corpus assumption.
"""
import argparse
import json
import pathlib
import re
import subprocess
import sys


def tokenize(text):
    parts = re.split(r'\s+', text.strip())
    out = []
    for p in parts:
        p2 = p.strip(".,;:!?\"'()[]{}<>")
        if p2:
            out.append(p2)
    return out


def g2p_phonemes(dumpwav, text):
    res = subprocess.run(
        [dumpwav, '--g2p', text],
        capture_output=True, text=True, timeout=20)
    out = res.stdout + res.stderr
    for line in out.splitlines():
        if line.startswith('ARPAbet:'):
            toks = line[len('ARPAbet:'):].strip().split()
            toks = [re.sub(r'\(\d+\)$', '', t) for t in toks]
            toks = [t for t in toks if t != 'pau']
            return toks
    return []


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--cohort', required=True)
    ap.add_argument('--corpus', required=True)
    ap.add_argument('--dumpwav', required=True)
    ap.add_argument('--out', required=True)
    a = ap.parse_args()

    corpus = {}
    with open(a.corpus) as f:
        for line in f:
            if not line.strip():
                continue
            e = json.loads(line)
            corpus[e['id']] = e

    cohort = []
    with open(a.cohort) as f:
        for line in f:
            if not line.strip():
                continue
            cohort.append(json.loads(line))

    token_cache = {}
    out_records = []
    for r in cohort:
        cid = r['corpus_id']
        if cid not in corpus:
            sys.stderr.write(f'WARN: corpus_id {cid} not in corpus\n')
            continue
        text = corpus[cid].get('text', '')
        tokens = tokenize(text)
        ph_idx = int(r['slot_idx']) // 2
        cum = 0
        found_word = None
        found_widx = None
        found_phinword = None
        found_total = None
        for widx, tok in enumerate(tokens):
            key = (cid, tok)
            if key not in token_cache:
                token_cache[key] = g2p_phonemes(a.dumpwav, tok)
            phs = token_cache[key]
            n = len(phs)
            if cum <= ph_idx < cum + n:
                found_word = tok
                found_widx = widx
                found_phinword = ph_idx - cum
                found_total = n
                break
            cum += n
        if found_word is None:
            if tokens:
                found_word = tokens[-1]
                found_widx = len(tokens) - 1
                found_phinword = max(0, ph_idx - cum)
                found_total = len(token_cache.get((cid, tokens[-1]), [])) or 1
            else:
                sys.stderr.write(f'WARN: no tokens for {cid}\n')
                continue
        out_records.append({
            'corpus_id': cid,
            'utt_idx': r['utt_idx'],
            'slot_idx': r['slot_idx'],
            'word': found_word,
            'word_idx': found_widx,
            'phoneme_idx_in_word': found_phinword,
            'phonemes_in_word': found_total,
            'text': text,
        })

    with open(a.out, 'w') as f:
        for rec in out_records:
            f.write(json.dumps(rec) + '\n')
    print(f'wrote {len(out_records)} cohort-word records to {a.out}')


if __name__ == '__main__':
    main()
