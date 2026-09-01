# Pre-L5 Foundation Hardening Qualification Evidence

Date: 2026-09-01

Starting baseline: `e26faaac12409e4cdd9b5dd33d6caf912413df89`

Qualified implementation revision: `fe46e7ecf47c38d4d57131101fdcfc37e3327115`

## Commit sequence

```text
4307c5e8 docs(architecture): reopen l4 for pre-l5 hardening
a1cd9c57 fix(material): reject malformed public source graphs
27cb70e1 refactor(toolchain): centralize shader support and compiler resources
8233740c fix(editor): make graph command transactions atomic
c1b74069 fix(architecture): classify graphkit as editor foundation
fe46e7ec docs(architecture): supersede stale freeze and index held work
```

## Build and test matrix

Every full build used the VS Developer PowerShell environment and `all -j 4 -k 0`. Every CMake-changing configure was
followed by an immediate second full build reporting `ninja: no work to do`.

```text
Default Developer / packed OFF: 154/154 CTest passed
PLAYER:                         154/154 CTest passed
EDITOR:                         157/157 CTest passed
TOOLCHAIN:                      147/147 CTest passed
Full Render / packed ON:        170/170 CTest passed, no qualification skipped
Restored Default / packed OFF:  154/154 CTest passed
```

Profile target scans confirmed:

```text
PLAYER: no material_graph, Material compiler/cooker, Shader reflection, GraphKit or FlowForge compiler
EDITOR: material_graph + node_graph_editor; no Material compiler/cooker or Shader reflection
TOOLCHAIN/packed host: Shader support -> Material -> Asset ordering
```

## Material fail-closed evidence

The compiler adversarial suite rejects invalid value/input/math/pin enums, partial/dangling links, wrong pin shape,
payload/pin mismatches, invalid slots, invalid Swizzle/Construct payloads, non-finite constants/defaults and malformed
OutputSurface shape before lowering. Every case returns `INVALID_GRAPH`; cycle, type mismatch and missing-output
classifications remain distinct.

`material_external_node_negative` proves external code cannot construct a custom Node kind. The internal lowering
maps no longer contain FLOAT/MUL fallbacks for invalid source enums.

## Installed relocation evidence

Fresh TOOLCHAIN install:

```text
E:/SyncForder/CodeRepos/install/qualification/PreL5-fe46e7ec
```

Copied relocation prefix:

```text
E:/SyncForder/CodeRepos/install/qualification/PreL5-fe46e7ec-relocated
```

The installed architecture validator passed. The compiler DLL was scanned and contained none of the repository,
build, `material_lglsl_emitted`, or Shader source directory strings.

For the decisive runtime test, the original tracked Shader source directory and original generated LGLSL include
directory were temporarily moved out of their compiled locations. With both paths unavailable, a fresh consumer was
configured and linked only against the copied prefix, then executed `compileMaterial()` successfully. Both directories
were restored in `finally`, and the working tree remained unchanged.

Installed consumers that independently configured, built and ran:

```text
material-graph
material-compiler
material-cooker
flowforge-model
flowforge-compiler
node-graph-editor
```

## Material artifact equivalence

The current packer was run with the same source identity/path metadata as the frozen baseline:

```text
605ebb80-8566-4f2a-80b6-a153fead1eec.luxasset
  SHA-256: DAA45C90CBF984ACCF0E6A7DE2A1D8E9060BD821CAEC8923E6423004B373037A
ccf44e8f-2220-459f-81a8-566d5c6d4b26.luxasset
  SHA-256: D49B59DE705992068D5CDE6A26293744098082FFDF5E6174B021A790E19B2231
ce5f070e-f086-49da-b516-bf6febd75a0f.luxasset
  SHA-256: DD12ACB0AE8DF4DD309F1E9037AE7808A8A3F36A80A0EF2D58A195E194F52197
static_model.luxpak
  SHA-256: 7728D9C0E6A5BBD6E7FCC962E5B7F628CF8F40DA84A1580AF766E4F0DD08703B
```

MaterialAsset v4, GBuffer/Forward SPIR-V, ShaderInfo and Texture AssetId relationships are byte-identical.

## GraphKit transaction evidence

The fault-injection suite covers failed cap-1 replacement, failed node detach, failed manual compound transaction,
mid-transaction undo failure and mid-transaction redo failure. Every failure restores the exact entry document,
leaves history/redo/revision unchanged, and permits a successful retry. Existing successful add/remove/connect/move,
ID-stability, layout and bimap tests remain green.

## Full Render qualification

```text
Texture: BC3_SRGB, 14 mips, 44,740,096 cooked bytes,
         upload=1, lit_pixels=109, validation_errors=0
Model: model=1, mesh_handle=0:1, material_handle=0:1, validation_errors=0
SceneSystem Render: scene_system_path=1, lit_pixels=1904,
                    mesh_handle=0:1, material_handle=0:1, validation_errors=0
10k scene: 10,000 instances, 33 lights, 3 model primitives, 192 vertices,
           full_sync=2.582 ms, initial_state_apply=33.872 ms,
           state_update_mean=0.772 ms, frame_mean=0.460 ms,
           frame_p95=0.852 ms, gpu_frame_mean=0.519202 ms,
           gpu_frame_p95=0.530080 ms, validation_errors=0
```

Screenshot:

```text
E:/SyncForder/CodeRepos/build/RelWithDebInfo/lux-engine/test/architecture_probes/large_3d_scene_performance.png
1280x720, 491881 bytes
SHA-256: 08A8DAB79A8F9E04C508F6F47E56AC2E09817870E04E29CAC3B1E6214C2057ED
```

The screenshot was inspected at original resolution; geometry, perspective depth, materials and lighting match the
previous qualified appearance.

## Conclusion

```text
L1-L3 remain closed.
L4 is closed on fe46e7ec after pre-L5 hardening.
Material compilation is fail-closed and installed-prefix relocatable.
GraphKit compound transactions are atomic.
L5 Editor implementation may proceed.
Held work remains indexed in .internal/UNFINISHED-WORK.md.
```
