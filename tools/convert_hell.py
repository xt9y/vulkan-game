"""Blender (4.x) offline Hell2025 FBX/Blend -> glTF 2.0 GLB converter.

Usage:
    blender -b --python tools/convert_hell.py -- /path/to/Hell2025/Hell2025/res assets/models

This tool is offline. Neither the game server nor the client links Blender.
It checks action rig compatibility instead of silently retargeting mismatched bones.
"""
import bpy
import sys
from pathlib import Path


def source(root, name):
    matches = sorted(root.rglob(name), key=lambda p: (
        "skinned_models_raw" not in str(p),
        "blender" not in str(p),
        len(p.parts),
    ))
    if not matches:
        raise FileNotFoundError(f"Missing Hell2025 asset: {name}")
    return matches[0]


def load(path):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    if path.suffix.lower() == ".blend":
        bpy.ops.wm.open_mainfile(filepath=str(path))
    else:
        bpy.ops.import_scene.fbx(filepath=str(path), use_anim=True,
                                 automatic_bone_orientation=False)


def animated_bones(action):
    if not hasattr(action, "fcurves"):
        return set()
    names = set()
    for fcurve in action.fcurves:
        name = fcurve.data_path
        if name.startswith('pose.bones["'):
            names.add(name.split('"')[1])
    return names


def append_clips(root, prefix):
    armatures = [obj for obj in bpy.data.objects if obj.type == "ARMATURE"]
    if len(armatures) != 1:
        raise ValueError(f"{prefix}: expected exactly one target armature")
    target = armatures[0]
    target_bones = {b.name for b in target.data.bones}
    targets = sorted((root / "animations").glob(f"{prefix}_*.fbx"))
    failures = []
    if target.animation_data is None:
        target.animation_data_create()
    for filename in targets:
        before_objects = set(bpy.data.objects)
        before_actions = set(bpy.data.actions)
        bpy.ops.import_scene.fbx(filepath=str(filename), use_anim=True,
                                 automatic_bone_orientation=False)
        imported_objects = set(bpy.data.objects) - before_objects
        actions = set(bpy.data.actions) - before_actions
        named = [action for action in actions if animated_bones(action)]
        compatible = [a for a in named if animated_bones(a) <= target_bones]
        if len(compatible) != 1:
            failures.append(filename.name)
        else:
            action = compatible[0]
            action.name = filename.stem
            track = target.animation_data.nla_tracks.new()
            track.name = filename.stem
            start, end = action.frame_range
            track.strips.new(filename.stem, int(start), action)
        for obj in imported_objects:
            bpy.data.objects.remove(obj, do_unlink=True)
    if failures:
        raise ValueError(
            f"{prefix}: clips need manual bone mapping, not silently imported: "
            + ", ".join(failures)
        )


def convert(root, target, model, clips):
    original = source(root, model)
    load(original)
    if clips:
        append_clips(root, clips)
    target.parent.mkdir(parents=True, exist_ok=True)
    bpy.ops.export_scene.gltf(
        filepath=str(target),
        export_format="GLB",
        use_selection=False,
        export_animations=True,
        export_nla_strips=True,
        export_skins=True,
        export_materials="EXPORT",
        export_image_format="AUTO",
        export_extras=True,
    )
    print(f"GLB: {original} -> {target}")


def main():
    args = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    if len(args) != 2:
        raise SystemExit("Usage: blender -b --python tools/convert_hell.py -- RES OUTPUT")
    root, out = (Path(arg).expanduser().resolve() for arg in args)
    convert(root, out / "glock.glb", "Glock.fbx", "Glock")
    convert(root, out / "kangaroo.glb", "Kangaroo.fbx", "Kangaroo")
    convert(root, out / "arms.glb", "Arms.blend", None)
    convert(root, out / "mermaid.glb", "Mermaid.blend", None)


if __name__ == "__main__":
    main()
