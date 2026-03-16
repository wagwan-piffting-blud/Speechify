#!/usr/bin/env python3
from __future__ import annotations

import argparse
import contextlib
import csv
import io
import sys
from pathlib import Path
from typing import Dict, List, Optional

import soundfile as sf
import torch
from qwen_tts import Qwen3TTSModel
from tqdm import tqdm

import os
import logging
from transformers.utils import logging as hf_logging

os.environ["TRANSFORMERS_NO_ADVISORY_WARNINGS"] = "1"
hf_logging.set_verbosity_error()
logging.getLogger("transformers").setLevel(logging.ERROR)
logging.getLogger("transformers.generation.utils").setLevel(logging.ERROR)

ERROR_PREFIX = "[ERROR"


def read_csv(path: Path) -> List[Dict[str, str]]:
    with path.open("r", encoding="utf-8", newline="") as f:
        return list(csv.DictReader(f))


def write_csv(path: Path, rows: List[Dict[str, str]], fieldnames: List[str]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(rows)


def parse_dtype(name: str):
    name = name.strip().lower()
    if name in ("bfloat16", "bf16"):
        return torch.bfloat16

    if name in ("float16", "fp16", "half"):
        return torch.float16

    if name in ("float32", "fp32"):
        return torch.float32

    raise ValueError(f"Unsupported dtype: {name}")


def transcript_is_usable(text: str) -> bool:
    text = (text or "").strip()
    return bool(text) and not text.startswith(ERROR_PREFIX)


def build_rows(
    transcript_rows: List[Dict[str, str]],
    output_root: Path,
    flat_output: bool,
    categories: Optional[set[str]],
    skip_existing: bool,
):
    out: List[Dict[str, str]] = []
    skipped_existing = 0
    usable_rows = 0
    category_filtered = 0

    for row in transcript_rows:
        rec = (row.get("recording_name") or row.get("name") or "").strip()
        cat = (row.get("category") or "unknown").strip()
        transcript = (row.get("transcript") or "").strip()
        hint = (row.get("hint") or "").strip()
        duration_s = (row.get("duration_s") or "").strip()

        if not rec:
            continue

        if categories and cat not in categories:
            category_filtered += 1
            continue

        if not transcript_is_usable(transcript):
            continue

        usable_rows += 1
        out_path = (output_root / f"{rec}.wav") if flat_output else (output_root / cat / f"{rec}.wav")
        if skip_existing and out_path.exists():
            skipped_existing += 1
            continue

        out.append(
            {
                "recording_name": rec,
                "category": cat,
                "duration_s": duration_s,
                "transcript": transcript,
                "hint": hint,
                "wav_target_path": str(out_path),
            }
        )

    return {
        "rows": out,
        "usable_rows": usable_rows,
        "skipped_existing": skipped_existing,
        "category_filtered": category_filtered,
    }


@contextlib.contextmanager
def suppress_stderr(enabled: bool = True):
    if not enabled:
        yield
        return

    # Use devnull to completely discard output rather than buffering
    with open(os.devnull, "w") as devnull:
        with contextlib.redirect_stderr(devnull):
            yield


def main() -> None:
    ap = argparse.ArgumentParser(description="Batch synthesize WAVs directly from transcripts CSV using Qwen3-TTS")
    ap.add_argument("--transcripts-csv", required=True)
    ap.add_argument("--model", required=True)
    ap.add_argument("--ref-audio", required=True)
    ap.add_argument("--ref-text", required=True)
    ap.add_argument("--output-root", required=True)
    ap.add_argument("--language", default="English")
    ap.add_argument("--device", default="cuda:0")
    ap.add_argument("--dtype", default="bfloat16", choices=["bfloat16", "float16", "float32"])
    ap.add_argument("--attn-implementation", default="flash_attention_2")
    ap.add_argument("--batch-size", type=int, default=4)
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--skip-existing", action="store_true")
    ap.add_argument("--flat-output", action="store_true")
    ap.add_argument("--category", nargs="+", default=None)
    ap.add_argument("--out-log", required=True)
    ap.add_argument("--show-stderr", action="store_true")
    args = ap.parse_args()

    transcript_rows = read_csv(Path(args.transcripts_csv))
    categories = set(args.category) if args.category else None
    prep = build_rows(
        transcript_rows=transcript_rows,
        output_root=Path(args.output_root),
        flat_output=args.flat_output,
        categories=categories,
        skip_existing=args.skip_existing,
    )
    synth_rows: List[Dict[str, str]] = prep["rows"]

    pre_limit_count = len(synth_rows)
    if args.limit and args.limit > 0:
        synth_rows = synth_rows[: args.limit]

    print(f"Transcripts loaded      : {len(transcript_rows)}")
    print(f"Usable transcript rows  : {prep['usable_rows']}")
    if categories:
        print(f"Category filtered out   : {prep['category_filtered']}")

    print(f"Skipped existing WAVs   : {prep['skipped_existing']}")
    print(f"Remaining before limit  : {pre_limit_count}")
    print(f"Remaining to synthesize : {len(synth_rows)}")

    print(f"Loading model: {args.model}")
    chosen_dtype = parse_dtype(args.dtype)
    with suppress_stderr(not args.show_stderr):
        model = Qwen3TTSModel.from_pretrained(
            args.model,
            device_map=args.device,
            dtype=chosen_dtype,
            attn_implementation=args.attn_implementation,
        )

    print("Creating reusable voice clone prompt...")
    with suppress_stderr(not args.show_stderr):
        voice_clone_prompt = model.create_voice_clone_prompt(
            ref_audio=args.ref_audio,
            ref_text=args.ref_text,
        )

    total_batches = (len(synth_rows) + args.batch_size - 1) // args.batch_size if synth_rows else 0
    print(f"Ready. Starting synthesis for {len(synth_rows)} rows in {total_batches} batches...")

    log_rows: List[Dict[str, str]] = []
    starts = list(range(0, len(synth_rows), args.batch_size))

    # Adjusted tqdm for PowerShell stability:
    # fixed width (ncols) and simplified ASCII bar.
    pbar = tqdm(
        starts,
        total=len(starts),
        desc="Synth",
        unit="batch",
        file=sys.stdout,
        ncols=80,
        mininterval=1.0,
        ascii=" #",
        leave=True
    )

    import time as _time
    import threading

    BATCH_TIMEOUT = 60  # seconds; skip batch if synthesis takes longer

    for batch_num, start in enumerate(pbar, start=1):
        batch = synth_rows[start:start + args.batch_size]
        texts = [r["transcript"] for r in batch]
        langs = [args.language for _ in batch]
        names = [r["recording_name"] for r in batch]

        # Log which recordings are about to be synthesized
        names_str = ", ".join(names)
        tqdm.write(f"[batch {batch_num}/{total_batches}] {names_str}")
        t0 = _time.time()

        # Run synthesis in a thread with timeout to avoid hangs
        result_holder = [None, None]  # [wavs, sr] or stays None on timeout
        error_holder = [None]

        def _synth_worker():
            try:
                with suppress_stderr(not args.show_stderr):
                    if len(batch) == 1:
                        wavs, sr = model.generate_voice_clone(
                            text=texts[0],
                            language=langs[0],
                            voice_clone_prompt=voice_clone_prompt,
                        )
                        if not isinstance(wavs, (list, tuple)):
                            wavs = [wavs]
                    else:
                        wavs, sr = model.generate_voice_clone(
                            text=texts,
                            language=langs,
                            voice_clone_prompt=voice_clone_prompt,
                        )
                result_holder[0] = wavs
                result_holder[1] = sr
            except Exception as e:
                error_holder[0] = e

        worker = threading.Thread(target=_synth_worker, daemon=True)
        worker.start()
        worker.join(timeout=BATCH_TIMEOUT)

        elapsed = _time.time() - t0

        if worker.is_alive():
            # Timeout -- skip this batch
            tqdm.write(f"  -> TIMEOUT ({elapsed:.1f}s) -- skipping batch")
            for row in batch:
                log_rows.append(
                    {
                        "recording_name": row["recording_name"],
                        "category": row["category"],
                        "status": "error",
                        "error": f"timeout after {BATCH_TIMEOUT}s",
                        "wav_target_path": row["wav_target_path"],
                        "transcript": row["transcript"],
                        "hint": row["hint"],
                    }
                )
            # Don't wait for the hung thread -- it's a daemon, will die with process
            continue

        if error_holder[0] is not None:
            err = str(error_holder[0])
            tqdm.write(f"  -> FAILED ({elapsed:.1f}s): {err}")
            for row in batch:
                log_rows.append(
                    {
                        "recording_name": row["recording_name"],
                        "category": row["category"],
                        "status": "error",
                        "error": err,
                        "wav_target_path": row["wav_target_path"],
                        "transcript": row["transcript"],
                        "hint": row["hint"],
                    }
                )
            continue

        try:
            wavs, sr = result_holder[0], result_holder[1]
            for row, wav in zip(batch, wavs):
                out_path = Path(row["wav_target_path"])
                out_path.parent.mkdir(parents=True, exist_ok=True)
                sf.write(out_path, wav, sr)
                lab_path = out_path.with_suffix(".lab")
                lab_path.write_text(row["transcript"], encoding="utf-8")
                log_rows.append(
                    {
                        "recording_name": row["recording_name"],
                        "category": row["category"],
                        "status": "ok",
                        "error": "",
                        "wav_target_path": str(out_path),
                        "transcript": row["transcript"],
                        "hint": row["hint"],
                    }
                )
            tqdm.write(f"  -> ok ({elapsed:.1f}s)")

        except Exception as e:
            err = str(e)
            tqdm.write(f"  -> FAILED saving ({elapsed:.1f}s): {err}")
            for row in batch:
                log_rows.append(
                    {
                        "recording_name": row["recording_name"],
                        "category": row["category"],
                        "status": "error",
                        "error": err,
                        "wav_target_path": row["wav_target_path"],
                        "transcript": row["transcript"],
                        "hint": row["hint"],
                    }
                )

    write_csv(
        Path(args.out_log),
        log_rows,
        [
            "recording_name",
            "category",
            "status",
            "error",
            "wav_target_path",
            "transcript",
            "hint",
        ],
    )

    ok = sum(1 for r in log_rows if r["status"] == "ok")
    err = sum(1 for r in log_rows if r["status"] == "error")
    print("\033[2J\033[H", end="")
    print(f"Done. ok={ok} error={err} skipped_existing={prep['skipped_existing']} log={args.out_log}")


if __name__ == "__main__":
    main()
