#!/usr/bin/env python3
"""
Write a .rhy sidecar next to each .mp3 for the ESP32 rhythm menu (see rhythm_mp3.cpp).

The firmware uses ONE fixed BPM grid for the whole song. Live / DJ mixes with tempo drift
or big BPM changes cannot be "detected" as multiple grids on-device — pick a reference BPM
on the PC (dominant section, doubled/halved time, or a tool average) and tune offset_ms so
the first beat you care about lines up.

Sidecar format (UTF-8 text, one key per line):
  bpm 128
  offset_ms 240
  difficulty 3

# comments   d and difficulty are aliases (1–9)

Usage:
  python gen_rhythm_sidecar.py --bpm 128 --offset-ms 0 --difficulty 3 song.mp3
  python gen_rhythm_sidecar.py --template ./rhythm_folder   # writes song.rhy from ID3 if mutagen installed

Optional: pip install mutagen  (read TBPM from tags into template files)
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path


def write_rhy(path: Path, bpm: int, offset_ms: int, difficulty: int | None) -> None:
    lines = [f"bpm {bpm}", f"offset_ms {offset_ms}"]
    if difficulty is not None:
        lines.append(f"difficulty {difficulty}")
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"Wrote {path}")


def id3_bpm(mp3: Path) -> int | None:
    try:
        from mutagen.mp3 import MP3
        from mutagen.id3 import ID3NoHeaderError
    except ImportError:
        return None
    try:
        audio = MP3(mp3)
    except (ID3NoHeaderError, OSError):
        return None
    if audio.tags is None:
        return None
    for key in ("TBPM", "TBP"):
        frame = audio.tags.get(key)
        if frame is None:
            continue
        try:
            raw = frame.text[0] if hasattr(frame, "text") else str(frame)
            v = float(str(raw).strip().split()[0])
            if 40 <= v <= 320:
                return int(round(v))
        except (ValueError, IndexError, TypeError):
            continue
    return None


def main() -> int:
    p = argparse.ArgumentParser(description="Generate .rhy sidecars for rhythm SD card")
    p.add_argument("inputs", nargs="*", type=Path, help=".mp3 files or directories")
    p.add_argument("--bpm", type=int, default=None, help="40–320")
    p.add_argument("--offset-ms", type=int, default=0)
    p.add_argument("--difficulty", type=int, default=None, help="1–9")
    p.add_argument(
        "--template",
        action="store_true",
        help="With directories: create missing .rhy using mutagen TBPM when possible, else bpm 120",
    )
    args = p.parse_args()
    if not args.inputs:
        p.print_help()
        return 1

    mp3s: list[Path] = []
    for x in args.inputs:
        if x.is_dir():
            mp3s.extend(sorted(x.glob("*.mp3")))
            mp3s.extend(sorted(x.glob("*.MP3")))
        elif x.suffix.lower() == ".mp3":
            mp3s.append(x)
        else:
            print(f"Skip (not .mp3): {x}", file=sys.stderr)

    if not mp3s:
        print("No .mp3 files found.", file=sys.stderr)
        return 1

    if args.difficulty is not None and not (1 <= args.difficulty <= 9):
        print("difficulty must be 1–9", file=sys.stderr)
        return 1

    for mp3 in mp3s:
        out = mp3.with_suffix(".rhy")
        if args.template and args.bpm is None:
            bpm = id3_bpm(mp3) or 120
            write_rhy(out, bpm, args.offset_ms, args.difficulty)
        else:
            if args.bpm is None:
                print(f"Need --bpm or --template for {mp3}", file=sys.stderr)
                return 1
            if not (40 <= args.bpm <= 320):
                print("bpm must be 40–320", file=sys.stderr)
                return 1
            write_rhy(out, args.bpm, args.offset_ms, args.difficulty)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
