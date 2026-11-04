#!/usr/bin/env python3
"""Download the original Hell2025 blood decals and VAT datasets.

Run from the vulkan-game root:
    python3 tools/assets/blood.py
    python3 tools/assets/blood.py --convert

Preserves all existing GLBs; conversion only creates absent blood20/blood22 GLBs.
Requires network access, and Blender only when --convert is given.
"""

import argparse
import hashlib
import json
import shutil
import struct
import subprocess
import sys
import urllib.error
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DEST = ROOT / "assets" / "blood"
BASE = "https://raw.githubusercontent.com/livinamuk/Hell2025/main/Hell2025/Hell2025/res"
DECALS = ("BloodDecal_0.png", "BloodDecal4.png",
          "BloodDecal6.png", "BloodDecal7.png", "BloodDecal9.png")
VARIANTS = (19, 20, 22)
SUFFIXES = ("json", "lookup.png", "pos.exr", "rot.exr", "mesh.fbx")
PROTECTED = ("blood19.glb", "blood_mesh.glb")


def digest(path: Path) -> str:
    hash_ = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1 << 20), b""):
            hash_.update(chunk)
    return hash_.hexdigest()


def verify(path: Path) -> None:
    with path.open("rb") as stream:
        header = stream.read(12)
    if path.suffix == ".png" and not header.startswith(b"\x89PNG\r\n\x1a\n"):
        raise ValueError(f"Invalid PNG: {path}")
    if path.suffix == ".exr" and not header.startswith(b"\x76\x2f\x31\x01"):
        raise ValueError(f"Invalid OpenEXR: {path}")
    if path.suffix == ".glb":
        if len(header) != 12 or header[:4] != b"glTF" or struct.unpack_from("<I", header, 4)[0] != 2:
            raise ValueError(f"Invalid GLB: {path}")
        if struct.unpack_from("<I", header, 8)[0] != path.stat().st_size:
            raise ValueError(f"GLB declared size mismatch: {path}")
    if path.stat().st_size == 0:
        raise ValueError(f"Empty asset: {path}")


def get(url: str, destination: Path) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    if destination.exists():
        verify(destination)
        print(f"Keep {destination.relative_to(ROOT)}")
        return
    temporary = destination.with_name(destination.name + ".part")
    try:
        request = urllib.request.Request(url, headers={"User-Agent": "vulkan-game-blood-assets"})
        with urllib.request.urlopen(request, timeout=120) as response, temporary.open("wb") as output:
            shutil.copyfileobj(response, output, length=1 << 20)
        verify(temporary.with_suffix(destination.suffix)) if False else None
        temporary.rename(destination)
        verify(destination)
        print(f"Downloaded {destination.relative_to(ROOT)}")
    except Exception:
        temporary.unlink(missing_ok=True)
        destination.unlink(missing_ok=True)
        raise


BLENDER = r'''
import bpy
import sys
from pathlib import Path

args = sys.argv[sys.argv.index("--") + 1:]
source, target = map(Path, args)
bpy.ops.object.select_all(action='SELECT')
bpy.ops.object.delete(use_global=False)
bpy.ops.import_scene.fbx(filepath=str(source), use_anim=False)
meshes = [obj for obj in bpy.context.scene.objects if obj.type == "MESH"]
if not meshes or not all(obj.data.uv_layers for obj in meshes):
    raise RuntimeError("VAT conversion needs original mesh UVs")
bpy.ops.export_scene.gltf(
    filepath=str(target),
    export_format='GLB',
    export_apply=False
)
'''


def convert(variant: int) -> None:
    target = DEST / f"blood{variant}.glb"
    if target.exists():
        verify(target)
        print(f"Preserve existing {target.relative_to(ROOT)}")
        return
    blender = shutil.which("blender")
    if not blender:
        raise RuntimeError("Blender not found. Install Blender before --convert")
    source = DEST / "vat" / f"Blood{variant}" / f"Blood{variant}_mesh.fbx"
    subprocess.run(
        [blender, "--background", "--python-expr", BLENDER, "--", str(source), str(target)],
        check=True
    )
    verify(target)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--convert", action="store_true",
                        help="Create missing Blood20/22 GLBs using Blender")
    args = parser.parse_args()

    protected_before = {
        name: digest(DEST / name)
        for name in PROTECTED if (DEST / name).is_file()
    }
    manifest = {"upstream": BASE, "assets": {}, "vat": {}}
    for name in DECALS:
        destination = DEST / "decals" / name
        get(f"{BASE}/textures/decals/{name}", destination)
        manifest["assets"][str(destination.relative_to(ROOT))] = {
            "sha256": digest(destination), "bytes": destination.stat().st_size
        }

    for variant in VARIANTS:
        prefix = f"Blood{variant}"
        folder = DEST / "vat" / prefix
        for suffix in SUFFIXES:
            name = f"{prefix}_{suffix}" if suffix != "json" else f"{prefix}.json"
            source = f"{BASE}/VAT/{name}"
            destination = folder / name
            get(source, destination)
            manifest["assets"][str(destination.relative_to(ROOT))] = {
                "sha256": digest(destination), "bytes": destination.stat().st_size
            }
        metadata = json.loads((folder / f"{prefix}.json").read_text())
        for key in ("positionTexture", "rotationTexture", "lookupTexture", "model"):
            if not (folder / metadata[key]).is_file():
                raise ValueError(f"Missing {prefix}: {metadata[key]}")
        if int(metadata["frameCount"]) < 1 or float(metadata["fps"]) <= 0:
            raise ValueError(f"Invalid VAT frame metadata: {prefix}")
        manifest["vat"][prefix] = {
            "frameCount": int(metadata["frameCount"]),
            "fps": float(metadata["fps"]),
            "boundsMin": metadata["boundsMin"],
            "boundsMax": metadata["boundsMax"]
        }

    if args.convert:
        for variant in (20, 22):
            convert(variant)

    for name, previous in protected_before.items():
        if digest(DEST / name) != previous:
            raise RuntimeError(f"Protected GLB changed: {name}")

    for name in (*PROTECTED, "blood20.glb", "blood22.glb"):
        if (DEST / name).exists():
            verify(DEST / name)
            manifest["assets"][str((DEST / name).relative_to(ROOT))] = {
                "sha256": digest(DEST / name), "bytes": (DEST / name).stat().st_size
            }
    manifest_path = DEST / "manifest.json"
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"Verified {len(manifest['assets'])} files; manifest: {manifest_path.relative_to(ROOT)}")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, urllib.error.URLError, subprocess.CalledProcessError) as exc:
        print(f"blood assets: {exc}", file=sys.stderr)
        sys.exit(1)
