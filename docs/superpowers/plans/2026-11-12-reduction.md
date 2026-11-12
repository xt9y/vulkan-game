# Application reduction implementation plan

**Goal:** Reduce application-owned C++ while preserving both entry points and existing gameplay, rendering, editor, and authenticated network behavior.

**Architecture:** Replace duplicated serialization with field visitors shared by readers and writers. Consolidate repeated G-buffer passes and connection operations around their existing resource ownership and synchronization. Work on the user's existing reduction branch.

**Constraints:** Keep client/client.cpp and server/server.cpp. Do not count moving code into dependencies or formatting as architectural reduction. Preserve packet bytes, validation, retry ordering, and Vulkan dependencies.

## Tasks

- [x] Capture deterministic protocol behavior before changing implementation; exercise all packet families, scene payloads, truncated/corrupt inputs, and sequence wraparound.
- [x] Share serialization field definitions and bounds checking; compare output and rejection behavior with the baseline corpus.
- [x] Consolidate renderer pass setup and world drawing; build the client and preserve barriers, viewport depth ranges, optical filtering, and clip planes.
- [x] Consolidate connection state operations and server dispatch where contracts match; rerun protocol checks and native builds.
- [x] Review complete diff, run a local DTLS scene-transfer smoke test and available rendering checks, and report source counts with remaining limitations.

## Verification record

Baseline: `c build vulkan-server` and `c build vulkan-client` both pass. Existing warnings: unused game_weapons motion alias and third-party Ozz missing SIMD initializer.

User has explicitly authorized immediate implementation and broad restructuring in the existing branch; no additional design approval is needed.

Results:

- Native server and client builds pass. Removed the unused application motion alias; the existing Ozz SIMD initializer warning remains.
- Protocol corpus matches the pre-refactor fingerprint `e6b9287d7c634fb9` under AddressSanitizer and UBSan. Additional checks cover endpoint/token binding, inactive sessions, encoded PNG dimensions, unchanged output on decode failure, missing/duplicate mesh IDs and nonuniform/negative-scale transforms.
- Local DTLS headless client exits 0 after receiving all four scene batches (9782 KiB, approximately two seconds).
- A 15-second graphical client exits 0, loading weapon assets and rendering PCloud plus planar mirror reflections. Logs contain 480 completed profiling samples. This is a smoke test, not proof of visual equivalence or 60 FPS.
- Independent review found no actionable regressions and independently reran the protocol checks.
- The initial checkout was stale: pushing discovered 56 remote commits already implementing overlapping refactors. Rebased onto `6ab075d`, preserved its CLI/window/loader cleanup, data-driven render graph, faster optical scissor union, shared CPU skinning, mesh traversal and transforms. Replaced its archive with the bounded variant and retained the consolidated receive dispatcher and direct header dependencies. Integration review found no actionable regressions.
- Application C++ physical line count: initial local checkout 10,248; fetched remote baseline 9,692; integrated result 9,500 (192 additional application lines removed from the fetched branch, 748 fewer than the initial checkout). Tests and documentation are excluded from this application count. Do not attribute the remote's existing 556-line reduction to this pass.
- The 6,094-line target remains unmet by 3,406 lines. This pass must not be represented as achieving the requested 60% reduction.
