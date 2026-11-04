# Blood asset sources and offline preparation

The five decals and the complete Blood19, Blood20 and Blood22 VAT datasets
come from [livinamuk/Hell2025](https://github.com/livinamuk/Hell2025), revision
`156a3d225bbdf896481408c249372e958eadc55b`.
See [manifest.json](manifest.json) for individual upstream paths, byte counts,
Git blob identities, SHA256 hashes, original metadata, FPS, frame counts,
conversion settings and performed validation.

Run from the repository root:

```sh
python3 tools/assets/prepare_blood.py
python3 tools/assets/prepare_blood.py --verify-only
```

Preparation uses Python's standard library, `curl` and headless Blender's Python
API (Blender 4.5 or later, using the native `bpy.ops.wm.fbx_import` importer for
the original ASCII FBX files). Blender is detected on PATH or at `/Applications/Blender.app`; use
`--blender /path/to/blender` to override. If unavailable on macOS, install it
with `brew install --cask blender`. These are offline authoring tools, with no
changes to the game's runtime dependencies or build.

The script downloads only missing sources, checks their pinned upstream Git
blob hashes and sizes, and refuses changed files. Once the manifest exists,
it also checks recorded SHA256 hashes. It exports only missing `blood20.glb`
and `blood22.glb`; existing exports receive fresh Blender round-trip validation.
`--verify-only` uses local files and writes no assets or manifest. Both modes
hash the protected `blood19.glb` and `blood_mesh.glb` before and after; these
files are never passed to Blender or written by the script.

Original PNG, EXR, JSON and FBX files remain byte-for-byte copies. Position
and rotation EXRs are linear animation data: load as **Non-Color**, never
apply sRGB conversion or quantize them. Lookup PNGs are also data textures;
preserve their original channels and bit depth. Decal color-space decisions
belong to the eventual rendering implementation. Blender's validation loads
all images as Non-Color without saving or processing them.

The new GLBs contain static indexed triangle geometry, normals and both source
UV sets. The VAT animation remains in the external EXRs, lookup PNG and JSON.
The normal Blender glTF export uses Y-up coordinates and maps UV `(u,v)` to
`(u,1-v)`. No manual UV flips, unwraps, modifiers, simplification or compression
are applied. Fresh FBX-to-GLB-to-Blender comparisons check world-space triangle
corners with every UV layer. Triangles and corners are matched by UVs rounded
to 6 decimal places, then unrounded world positions and UVs are compared with
absolute tolerances of `1e-6` and `2e-7`, respectively. This allows glTF vertex
splitting and reordering while checking corner association. The manifest records
maximum measured errors, matching UV triangle fingerprints and mesh counts.

Validation checks all source hashes, PNG signatures and every chunk CRC, EXR
signatures and channel precision, metadata filenames/timing/bounds, image
decoding in Blender, and GLB headers, buffers, accessors, finite attributes and
index bounds. Image decoding reads the first sample; it is not an all-pixel
scan. FPS and frame counts come from upstream JSON; texture layout and VAT
playback have not been independently validated. The protected GLBs were only
hashed. Engine import, rendering and visual playback review were not performed.

## Attribution and redistribution status

Credit belongs to the Hell2025 creators and any third-party asset contributors.
The pinned repository's complete tree was audited for license, copying and
copyright filenames, and its [original README](upstream/README.md) reviewed.
Only unrelated third-party dependency/shader notices were found, with no general
project license or explicit redistribution grant for these blood assets.
Exact candidate paths are recorded in the manifest.

**Redistribution rights remain unconfirmed.** Public availability and this
conversion do not grant rights to redistribute the originals or derived GLBs.
These assets are not relicensed here; obtain a grant from the rights holder
before redistribution.
