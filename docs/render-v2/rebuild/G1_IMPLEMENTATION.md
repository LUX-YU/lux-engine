# Draft4 G0/G1 Implementation

Scope: user-cleared Render V2 rebuild; only G0 and Core G1. No G2, GPU implementation,
remote push, PR, merge, or Legacy source changes.

## Source and protection baseline

- Original branch: `codex/render-v2`, HEAD `246fe21eb6d060801b9a16ddcadde2be89dbbecc`.
- Remote reference at preflight: `c221b93d1a8eccf690c5e4e0b848150f3634fd8b`.
- Working branch: `codex/render-v2-rebuild`, based on the user's current cleaned tree.
- Cleanup commit: `36e53c4bbf775152789be729454251ee09b8f402` (329 user-deleted files only).
- Cleanup counts: core 12, data 32, graph 76, runtime 33, transport 28, vulkan 148.
- Legacy source: `a669409a289a6fa4092f21176397795b1cdb7f3e`; all 719 manifest entries
  match frozen source blobs, current HEAD blobs and Git-filtered working files. The
  additional `render_legacy/CMakeLists.txt` guard is preserved (720 files total).
- The only unrelated worktree modification is `.vscode/settings.json`; neither cleanup
  nor implementation stages it. SHA-256:
  `f51fd038282aba9d31a4ad9ffb64f1e8bc3b525b4f6ca4c9ca3c2f306e64a57c`.
- Source-tree-external preflight: `E:/RenderV2Rebuild/G1/evidence/g0/`, including binary
  diff, exact settings bytes/patch, deleted-file list and input copies/hashes.

Inputs were fully read in this conversation. Draft4 is the only target architecture:

| Input | SHA-256 |
|---|---|
| Architecture Draft4 | `7405814c45b2388dd540e39ff920df3ad99d1b850cc9a2f8e089a8e641f4203d` |
| Implementation Plan | `da428144807d06feb1c89f27abc587dd2179f5570797e08e66f61b0fc1be2c53` |
| First Implementation Command | `e57a4c0a50a84f278a1fc3944030b260d0f67fae249d8ee6559a642a7b879c0e` |

## Actual implementation and boundary

Core has three public headers (`Ids`, `Error`, `Target`), one production TU, and one
static library. IDs directly use SlotKey. TargetDesc is a value, not an owner. Scope and
endpoint limitations are documented without introducing Ref wrappers or fake endpoints.
The existing neutral image-format vocabulary and structured error values are reused.
There is no Scene/View/Target owner, Operation, Compiler, Client/Server, GPU Storage,
Manager, Host or Port. No runtime identity hash index is introduced.

ALLOWED changes: `modules/function/render/core/**`, the new Render CMake entry, existing
bootstrap CMake entry, and these new rebuild reports/checks. READ-ONLY: canonical error
and image-format sources, lux-cxx/toolset, historical reports/test scripts and consumer
CMake. FORBIDDEN: `render_legacy/**`, user settings, production G2+ modules, main/remote.

The bootstrap now invokes only the actual Render entry/Core. It no longer invokes
historical tests whose implementation was removed by the user. Historical F4/F5/F6
scripts themselves are unchanged. No existing product profile or test source was disabled
to achieve Core qualification. Public include installation reuses the canonical three
lower-level value headers; it does not link the resource/Shader/script implementation.

## Dependency and consumer ledger

| Consumer/entry | Existing dependency problem | G1 disposition |
|---|---|---|
| `modules/function/CMakeLists.txt` TOOLCHAIN | Missing client, feature-client and standard-content paths | EXPECTED-BLOCKED; later real client/integration migration |
| Function component exports | Old render_client/feature_client/standard_content/graph/vulkan/runtime components | EXPECTED-BLOCKED; no aliases or fake components |
| Engine Context / Scene | render_runtime-gated rendering directories were frozen | Not restored in G1 |
| UI / Editor / project plugins | Old render client, scene/context rendering and UI rendering contracts | Later actual consumer integration |
| Installed spatial / render-kernels probes | Old render_client/render_vulkan SDK contracts | Historical consumers preserved; new Core consumer added |
| Existing Render bootstrap | References all removed V2 modules | Rewired to actual Core only |

This is an independent Core qualification, not a full-engine build PASS. No frozen
Legacy library is configured or linked. Future consumer work cannot treat this ledger
as permission to skip their tests.

## Verification procedure

Development toolchain: Windows x64, MSVC 19.44.35226, CMake 3.31.10, Ninja, C++20,
RelWithDebInfo, no RTTI, exceptions not globally disabled. Source code is platform-neutral.
lux-cxx is pinned to `bf779515a120350c7c5412c366eea73afdf59ccf`; toolset to
`c104eb6dd30a45f5d8e76f24e8d6f8de064bdf60`. Neither repository was modified.

The three required lux-cxx header components were staged in an isolated dependency SDK.
All 30 header bytes match the pinned sibling source and existing installation. Existing
package metadata was relocated to that prefix (the installed toolset metadata contains
absolute paths). No Engine headers or old Render libraries are present in that SDK.
This dependency packaging is not a new production target or a dependency source patch.

Commands are recorded by `E:/RenderV2Rebuild/G1/run.ps1`; raw logs remain outside source.
Required sequence: configure, `cmake --build <build> --target all -j 4 -- -k 0`, CTest,
second no-work build, installed consumer. After I, use an independent clean clone at exact
I, run `ValidateTrackedSnapshot.cmake`, rebuild/test/install again and run `check_g1.py`.
V must only add a verification report. Any implementation fix requires a new I.

The new seven CTests include six behavior groups plus the relocated installation test
(which runs its own 1/1 consumer test). Three separate header TUs are built in both source
and installed contexts. ASan coverage, root configure limitations and final qualification
results are reported by V, not inferred from development success.

GPU / Validation / SyncValidation: N/A. Linux: NOT-RUN (not required this round).
Android build: NOT-RUN; the mandated include synchronization does not imply qualification.
Endpoint admission, GPU completion, Runtime/Native resources and all G2+ work: NOT_IMPLEMENTED.
