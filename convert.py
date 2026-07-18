#!/usr/bin/env python3
"""
Open CYD Player — video converter
Turns any video (mp4/mkv/avi/webm/...) into the three files the player needs:

    movie.mjpeg   concatenated JPEG frames, 320x240
    movie.mp3     audio track (44.1 kHz stereo)
    movie.idx     frame index for instant seeking (format "CYD1")

Requires: ffmpeg on PATH  (https://ffmpeg.org)

Usage:
    python convert.py input.mp4                     # defaults: 320x240 @ 24fps, q=8
    python convert.py input.mp4 --fps 30 --quality 6
    python convert.py input.mp4 -o /media/sd/videos/movie

Quality: ffmpeg mjpeg q scale, 2 (best/biggest) .. 31 (worst/smallest).
6-10 is a good range for the CYD; lower q if your SD card is fast.

License: MIT
"""
import argparse
import shutil
import struct
import subprocess
import sys
from pathlib import Path

SOI = b"\xff\xd8"


def run(cmd):
    print("  $", " ".join(str(c) for c in cmd))
    r = subprocess.run(cmd)
    if r.returncode != 0:
        sys.exit(f"ffmpeg failed (exit {r.returncode})")


def build_index(mjpeg_path: Path, idx_path: Path, fps: float):
    data = mjpeg_path.read_bytes()
    offsets = []
    pos = 0
    while True:
        pos = data.find(SOI, pos)
        if pos < 0:
            break
        offsets.append(pos)
        pos += 2
    offsets.append(len(data))  # sentinel: EOF

    frames = len(offsets) - 1
    if frames <= 0:
        sys.exit("No JPEG frames found in mjpeg output — conversion failed?")

    with idx_path.open("wb") as f:
        f.write(b"CYD1")
        f.write(struct.pack("<I", int(round(fps * 1000))))
        f.write(struct.pack("<I", frames))
        f.write(struct.pack(f"<{len(offsets)}I", *offsets))

    dur = frames / fps
    print(f"  index: {frames} frames, {fps:g} fps, {dur/60:.1f} min, "
          f"avg frame {len(data)//frames/1024:.1f} KB")


def main():
    ap = argparse.ArgumentParser(description="Convert video for the Open CYD Player")
    ap.add_argument("input", type=Path)
    ap.add_argument("-o", "--output", type=Path,
                    help="output base path (no extension); default = input name")
    ap.add_argument("--fps", type=float, default=24.0, help="target fps (default 24)")
    ap.add_argument("--width", type=int, default=320)
    ap.add_argument("--height", type=int, default=240)
    ap.add_argument("--quality", type=int, default=8,
                    help="JPEG q, 2=best 31=smallest (default 8)")
    ap.add_argument("--no-audio", action="store_true")
    args = ap.parse_args()

    if not shutil.which("ffmpeg"):
        sys.exit("ffmpeg not found on PATH — install it from https://ffmpeg.org")
    if not args.input.exists():
        sys.exit(f"input not found: {args.input}")

    base = args.output or args.input.with_suffix("")
    base.parent.mkdir(parents=True, exist_ok=True)
    mjpeg, mp3, idx = (base.with_suffix(s) for s in (".mjpeg", ".mp3", ".idx"))

    scale = (f"scale={args.width}:{args.height}:force_original_aspect_ratio=decrease,"
             f"pad={args.width}:{args.height}:(ow-iw)/2:(oh-ih)/2:black")

    print(f"[1/3] video -> {mjpeg.name}")
    run(["ffmpeg", "-y", "-i", str(args.input),
         "-vf", f"fps={args.fps},{scale}",
         "-c:v", "mjpeg", "-q:v", str(args.quality),
         "-an", "-f", "mjpeg", str(mjpeg)])

    if not args.no_audio:
        print(f"[2/3] audio -> {mp3.name}")
        run(["ffmpeg", "-y", "-i", str(args.input),
             "-vn", "-c:a", "libmp3lame", "-b:a", "128k",
             "-ar", "44100", "-ac", "2", str(mp3)])
    else:
        print("[2/3] audio skipped")

    print(f"[3/3] index -> {idx.name}")
    build_index(mjpeg, idx, args.fps)

    print("\nDone. Copy these to the SD card's /videos folder:")
    for p in (mjpeg, mp3 if not args.no_audio else None, idx):
        if p and p.exists():
            print(f"  {p}  ({p.stat().st_size/1_048_576:.1f} MB)")


if __name__ == "__main__":
    main()
