# Client/server verification before merging PR #14

Verified application revision: `c6b73484b7559c3059fc45397c962147a04989d2`.
Platform: native macOS, debug configuration, MoltenVK.

## Build and protocol checks

- Recompiled both application translation units with `c build vulkan-server --no-object-cache` and `c build vulkan-client --no-object-cache`; both linked successfully.
- Ran `python3 tests/run_protocol.py` with AddressSanitizer and UBSan. The baseline fingerprint remained `e6b9287d7c634fb9`.
- The client compilation retains the existing third-party Ozz SIMD initializer warning. The server compiled without warnings.

## Live checks

Started an isolated server on loopback port 27956 with temporary DTLS identity/trust storage. Ran the actual headless client, the graphical client, and a sanitizer-enabled functional harness using the application's real client connection implementation concurrently.

Passed checks:

- DTLS connection and complete four-batch scene transfer: 17 meshes, 18 instances, 26 textures, 9782 KiB.
- Authoritative movement acknowledgement and ground collision.
- Initial weapon ammunition, firing one round, duplicate-fire idempotence, and reload refill.
- Light creation, property editing, and deletion replicated to the clients.
- Glass cube creation, geometry editing, and deletion replicated to the clients.
- Respawn acknowledgement, authoritative spawn restoration, and ammunition reset.
- Graphical client loaded Glock and casing assets and recorded GPU profiling samples.
- Rendering initialized PCloud lighting, photon transport, the full-glass compositor while glass was present, and planar mirror reflection after glass removal.
- Headless client and functional harness exited 0. Sent SIGTERM to the graphical client and server; both shut down gracefully with exit 0. All four test process IDs were confirmed absent afterward.

Local logs are in `build/merge-verification/` (generated, excluded from Git).

These are functional integration and rendering smoke checks; they do not establish exhaustive visual equivalence, every animation/editor interaction, or a universal 60 FPS guarantee. The earlier 6094-line reduction goal remains incomplete; the application currently contains 9500 physical source lines.
