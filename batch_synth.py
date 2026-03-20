#!/usr/bin/env python3
"""
batch_synth.py — Generate a training corpus by synthesizing diverse sentences
through the Speechify engine. Used for Experiment F (train on engine output).

Usage:
    python batch_synth.py --exe "bin\\spfy_dumpwav32_8khz.exe" --output corpus
    python batch_synth.py --exe "bin\\spfy_dumpwav32_8khz.exe" --output corpus --sentences custom.txt
"""

import argparse
import os
import subprocess
import sys
import glob

# Diverse sentence corpus — weather, news, dates, numbers, conversational,
# names, addresses, phonetically varied content
DEFAULT_SENTENCES = [
    # Weather forecasts (Mara's primary domain)
    "The weather today will be partly cloudy with a high near seventy five degrees.",
    "Tonight, expect clear skies with temperatures dropping to around fifty two.",
    "Tomorrow will bring scattered showers with winds from the southwest at fifteen miles per hour.",
    "The extended forecast calls for mostly sunny conditions through the weekend.",
    "A winter storm warning is in effect for the northern mountains through Friday evening.",
    "Temperatures will reach the low nineties across the metro area this afternoon.",
    "There is a forty percent chance of thunderstorms after midnight.",
    "Fog and low clouds will develop along the coast by early morning.",
    "High pressure will build across the region bringing dry conditions for the next several days.",
    "Wind chill values may drop as low as minus fifteen degrees overnight.",

    # Numbers and dates
    "Please call us at five five five, zero one two three.",
    "The date is September fourteenth, two thousand and three.",
    "Winds at twelve to eighteen miles per hour with gusts up to thirty five.",
    "The barometric pressure is twenty nine point nine two inches and rising.",
    "Relative humidity will be between sixty five and eighty percent.",
    "Sunset today is at seven forty three p.m.",
    "The record high for this date is one hundred and four degrees set in nineteen thirty six.",
    "Precipitation amounts of one quarter to one half inch are expected.",
    "The UV index for tomorrow will be eight, which is very high.",
    "Visibility will be reduced to less than one mile in areas of dense fog.",

    # News-style content
    "The National Weather Service has issued a tornado watch for the following counties.",
    "Authorities are advising residents to prepare for possible flooding along the river.",
    "Emergency shelters have been opened at the community center and the high school gymnasium.",
    "Power outages are being reported across the eastern part of the state.",
    "Road conditions are expected to deteriorate rapidly after midnight.",
    "The governor has declared a state of emergency for twelve counties.",
    "Evacuation orders have been issued for low lying areas near the coast.",
    "Search and rescue teams are standing by in the affected areas.",

    # Conversational and varied phonetics
    "Good morning, this is your weather update for the greater metropolitan area.",
    "Thanks for listening, and have a wonderful afternoon.",
    "Stay tuned for the latest traffic and weather together on the eights.",
    "We will continue to monitor this developing situation throughout the evening.",
    "For the most current information, please visit our website.",
    "This has been a special weather statement from the National Weather Service.",
    "Let's take a look at what's happening across the region right now.",
    "Coming up next, your complete seven day forecast.",

    # Phonetically challenging content (varied consonant clusters, diphthongs)
    "The northeastern corridor will experience the brunt of this powerful storm system.",
    "Atmospheric instability will increase significantly through the afternoon hours.",
    "Drought conditions continue to worsen throughout the southwestern United States.",
    "The jet stream will shift northward bringing a much needed warm up by Thursday.",
    "An unusually strong ridge of high pressure is blocking the typical weather pattern.",
    "Freezing rain and sleet will make travel extremely hazardous this evening.",
    "The tropical disturbance strengthened into a tropical depression earlier today.",
    "Widespread frost is expected across rural valleys and sheltered low spots.",

    # Short phrases (for phoneme coverage)
    "Partly cloudy.",
    "Mostly sunny and warm.",
    "Cold front approaching.",
    "Heavy rain likely.",
    "Snow showers ending.",
    "Fair skies tonight.",
    "Breezy and mild.",
    "Hot and humid.",
    "Dense fog advisory.",
    "Severe thunderstorm watch.",
]


def main():
    parser = argparse.ArgumentParser(description="Batch synthesize sentences through Speechify")
    parser.add_argument("--exe", default="bin\\spfy_dumpwav32_8khz.exe",
                        help="Path to Speechify dump executable")
    parser.add_argument("--output", default="corpus",
                        help="Output directory for WAV files")
    parser.add_argument("--sentences", default=None,
                        help="Text file with one sentence per line (uses built-in corpus if omitted)")
    parser.add_argument("--prefix", default="mara_corpus",
                        help="Filename prefix for output WAVs")

    args = parser.parse_args()
    os.makedirs(args.output, exist_ok=True)

    # Load sentences
    if args.sentences:
        with open(args.sentences, "r", encoding="utf-8") as f:
            sentences = [line.strip() for line in f if line.strip()]
        print(f"Loaded {len(sentences)} sentences from {args.sentences}")
    else:
        sentences = DEFAULT_SENTENCES
        print(f"Using built-in corpus: {len(sentences)} sentences")

    print(f"Exe:    {args.exe}")
    print(f"Output: {args.output}")
    print(f"{'='*60}")

    success = 0
    fail = 0

    for i, sentence in enumerate(sentences):
        out_name = f"{args.prefix}_{i:04d}.wav"
        out_path = os.path.join(args.output, out_name)

        if os.path.exists(out_path) and os.path.getsize(out_path) > 100:
            print(f"  [{i+1:3d}/{len(sentences)}] SKIP (exists): {out_name}")
            success += 1
            continue

        print(f"  [{i+1:3d}/{len(sentences)}] {sentence[:60]}...")

        try:
            result = subprocess.run(
                [args.exe, sentence, out_path],
                capture_output=True,
                timeout=30,
            )

            if os.path.exists(out_path) and os.path.getsize(out_path) > 44:
                success += 1
            else:
                print(f"    WARNING: No output or empty file")
                fail += 1

        except subprocess.TimeoutExpired:
            print(f"    TIMEOUT")
            fail += 1
        except Exception as e:
            print(f"    ERROR: {e}")
            fail += 1

    print(f"\n{'='*60}")
    print(f"  Done: {success} success, {fail} failed")
    print(f"  Output: {args.output}")

    # Report total duration
    total_dur = 0
    for wav in glob.glob(os.path.join(args.output, "*.wav")):
        try:
            import soundfile as sf
            info = sf.info(wav)
            total_dur += info.duration
        except:
            pass
    if total_dur > 0:
        print(f"  Total audio: {total_dur:.1f}s ({total_dur/60:.1f} min)")

    if total_dur < 300:
        print(f"\n  NOTE: Aim for 5+ minutes of audio for good RVC training.")
        print(f"  Add more sentences with --sentences custom.txt")


if __name__ == "__main__":
    main()
