#!/usr/bin/env python3
"""Prepare GPU-ready RGBA16F VAT textures from the original EXRs.

Run from any directory: python3 tools/assets/pack_vat.py
Requires Blender 4.x with NumPy only for offline asset preparation.
Original EXRs/lookup PNGs and existing GLBs are never modified.
"""
import argparse
import shutil
import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DATA = ROOT / "assets" / "blood" / "vat"
VARIANTS = (19, 20, 22)
CHANNELS = ("pos", "rot")
MAGIC = b"VTX1"


def output_for(variant, channel):
    folder = DATA / f"Blood{variant}"
    return folder / f"Blood{variant}_{channel}.rgba16f"


def valid(path):
    if not path.is_file() or path.stat().st_size < 12:
        return False
    with path.open("rb") as stream:
        h = stream.read(12)
    magic, width, height = struct.unpack("<4sII", h)
    return (magic == MAGIC and 0 < width <= 8192 and 0 < height <= 8192
            and path.stat().st_size == 12 + width * height * 8)


def convert_in_blender():
    import bpy
    import numpy as np

    for variant in VARIANTS:
        folder = DATA / f"Blood{variant}"
        for channel in CHANNELS:
            source = folder / f"Blood{variant}_{channel}.exr"
            target = output_for(variant, channel)
            if not source.is_file():
                raise FileNotFoundError(source)
            if valid(target) and target.stat().st_mtime >= source.stat().st_mtime:
                print(f"VAT: using {target.relative_to(ROOT)}")
                continue

            image = bpy.data.images.load(str(source), check_existing=False)
            try:
                image.colorspace_settings.name = "Non-Color"
                width, height = map(int, image.size)
                if not (0 < width <= 8192 and 0 < height <= 8192):
                    raise RuntimeError(f"Unsupported EXR dimensions: {source}")
                pixels = np.empty(width * height * 4, dtype=np.float32)
                image.pixels.foreach_get(pixels)
                rows = pixels.reshape((height, width, 4))
                temporary = target.with_suffix(".rgba16f.part")
                with temporary.open("wb") as stream:
                    stream.write(struct.pack("<4sII", MAGIC, width, height))
                    # Blender stores the first image row at the bottom.
                    # Match the top-to-bottom convention of PNG/Stb uploads.
                    for y in range(height - 1, -1, -1):
                        stream.write(rows[y].astype("<f2", copy=False).tobytes())
                temporary.replace(target)
                if not valid(target):
                    raise RuntimeError(f"Bad VAT output: {target}")
                print(f"VAT: prepared {target.relative_to(ROOT)} ({width}x{height})")
            finally:
                bpy.data.images.remove(image)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--worker", action="store_true",
                        help=argparse.SUPPRESS)
    args = parser.parse_args()

    if args.worker:
        convert_in_blender()
        return

    pending = [
        (n, channel) for n in VARIANTS for channel in CHANNELS
        if not valid(output_for(n, channel)) or
        output_for(n, channel).stat().st_mtime <
        (DATA / f"Blood{n}" / f"Blood{n}_{channel}.exr").stat().st_mtime
    ]
    if not pending:
        print("VAT: all GPU texture caches are valid")
        return
    blender = shutil.which("blender")
    if not blender:
        for candidate in (
            Path("/Applications/Blender.app/Contents/MacOS/Blender"),
            Path("C:/Program Files/Blender Foundation/Blender 4.5/blender.exe")
        ):
            if candidate.is_file():
                blender = str(candidate)
                break
    if not blender:
        raise RuntimeError("Blender required once to pack VAT EXRs. "
                           "Install Blender and run python3 tools/assets/pack_vat.py")
    subprocess.run([blender, "--background", "--python", str(Path(__file__).resolve()),
                    "--", "--worker"], cwd=ROOT, check=True)


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, subprocess.CalledProcessError) as exc:
        print(f"VAT preparation failed: {exc}", file=sys.stderr)
        sys.exit(1)
