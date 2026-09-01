# L4 Toolchain Convergence Qualification Evidence

Date: 2026-09-01

Starting baseline: `bb2b12b3f425db16eca6a96828a101fd3ae9bad5`

Qualified implementation revision: `75d82e7a2bae5ae804fc1697e551df4b83643e56`

## Commit sequence

```text
1be102f1 docs(toolchain): define l4 compiler and cooker layer
07a87f5a refactor(material): move source graph to function ownership
4c013db3 refactor(authoring): retire unused authoring surface
bec85799 refactor(material): separate source compiler and runtime contracts
2f2a8bf9 refactor(material): expose graph compiler and delegate cooker
72954165 refactor(material): unify imported and graph compilation
0c9d4a6e docs(architecture): freeze l4 material toolchain topology
3b2b4891 test(toolchain): qualify material compiler and cooker boundaries
75d82e7a fix(architecture): validate canonical process install includes
```

## Build and test matrix

All full builds used the VS Developer PowerShell environment and `all -j 4 -k 0`. Every CMake-changing
configuration was followed by an immediate second full build that reported `ninja: no work to do`.

```text
Default Developer / packed OFF: 154/154 CTest passed
PLAYER:                         154/154 CTest passed
EDITOR:                         156/156 CTest passed
TOOLCHAIN:                      146/146 CTest passed
Full Render / packed ON:        169/169 CTest passed, no qualification skipped
Restored Default / packed OFF:  154/154 CTest passed
```

Profile target checks:

```text
Default/PLAYER: material_graph, compiler and cooker absent
EDITOR:         material_graph and node_graph_editor present; compiler and cooker absent
TOOLCHAIN:      material_graph, compiler, cooker, Model and packer present
```

The source architecture gate and all four L4/L5 dependency probes passed:

```text
TOOLCHAIN -> FUNCTION positive
TOOLCHAIN -> EDITOR negative
EDITOR    -> TOOLCHAIN positive
FUNCTION  -> TOOLCHAIN negative
```

## Fresh install and consumers

The final TOOLCHAIN tree was configured with this exact install prefix:

```text
E:/SyncForder/CodeRepos/install/qualification/L4Toolchain-75d82e7a
```

The installed architecture validator passed against that prefix. The following five consumers independently
configured, built and ran using only installed package metadata and libraries:

```text
material-graph
material-compiler
material-cooker
flowforge-model
flowforge-compiler
```

The installed surface contains no active `engine/authoring`, legacy Material Toolchain path, old namespace or
retired package/component alias.

## Material artifact equivalence

The pre-migration and current packers were run against the same absolute generated Model source path. The resulting
Material assets and Pak are byte-identical:

```text
605ebb80-8566-4f2a-80b6-a153fead1eec.luxasset
  bytes: 62358
  SHA-256: DAA45C90CBF984ACCF0E6A7DE2A1D8E9060BD821CAEC8923E6423004B373037A

ccf44e8f-2220-459f-81a8-566d5c6d4b26.luxasset
  bytes: 66269
  SHA-256: D49B59DE705992068D5CDE6A26293744098082FFDF5E6174B021A790E19B2231

ce5f070e-f086-49da-b516-bf6febd75a0f.luxasset
  bytes: 64609
  SHA-256: DD12ACB0AE8DF4DD309F1E9037AE7808A8A3F36A80A0EF2D58A195E194F52197

static_model.luxpak
  SHA-256: 7728D9C0E6A5BBD6E7FCC962E5B7F628CF8F40DA84A1580AF766E4F0DD08703B
```

A cross-build-directory comparison initially produced different whole-image hashes because `AssetInfo.source_path`
records the caller-provided absolute source path. Binary comparison showed that only bytes 166-251 in that metadata
field differed; every byte from offset 384 through the end of all three Material assets was identical. Repeating the
comparison with the same source-path input produced the exact frozen hashes above. Thus the Material payload,
GBuffer/Forward SPIR-V, ShaderInfo and Texture AssetId relationships did not change.

## Full Render qualification

Host:

```text
OS: Microsoft Windows 11 Pro 10.0.26200
CPU: 13th Gen Intel(R) Core(TM) i7-13700KF
GPU: NVIDIA GeForce RTX 4070 Ti
Vulkan instance: 1.4.321
Vulkan device API: 1.4.325
Vulkan driver: 591.86.0.0 (Windows display driver 32.0.15.9186)
Validation layer: VK_LAYER_KHRONOS_validation 1.4.304
```

Explicit non-SKIP qualification results:

```text
Texture: BC3_SRGB, 14 mips, 44,740,096 cooked bytes,
         upload=1, lit_pixels=109, validation_errors=0
Model: model=1, mesh_handle=0:1, material_handle=0:1, validation_errors=0
SceneSystem Render: scene_system_path=1, lit_pixels=1904,
                    mesh_handle=0:1, material_handle=0:1, validation_errors=0
10k scene: 10,000 instances, 33 lights, 3 model primitives, 192 mesh vertices,
           2 Material textures, lit_pixels=162342,
           full_sync=2.318 ms, initial_state_apply=31.737 ms,
           state_update_mean=0.688 ms, frame_mean=0.411 ms,
           frame_p95=0.739 ms, gpu_frame_mean=0.487737 ms,
           gpu_frame_p95=0.496992 ms, gpu_samples=79,
           validation_errors=0
```

Screenshot evidence:

```text
E:/SyncForder/CodeRepos/build/RelWithDebInfo/lux-engine/test/architecture_probes/large_3d_scene_performance.png
1280x720, 491881 bytes
SHA-256: 08A8DAB79A8F9E04C508F6F47E56AC2E09817870E04E29CAC3B1E6214C2057ED
```

The screenshot was inspected at original resolution. It shows the dense 10k mesh grid with perspective depth,
per-instance material variation and lighting; there is no black/empty frame, missing geometry, gross clipping or
resource-binding regression.

## User-owned changes

The pre-existing `.gitignore` change and formatting-only patch in
`engine/domain/world/partition/include/lux/engine/world/WorldPartition.hpp` remain uncommitted and were not staged by
this implementation.

## Conclusion

```text
L4 Toolchain convergence is closed.
MaterialGraph is the only Material source SSOT.
compileMaterial() is the only graph-to-description pipeline.
Cooker and Model import share the same compiler path.
Runtime and PLAYER remain free of L4 and Editor implementation dependencies.
L1-L3 architecture remains closed.
Asset residency and Product streaming remain held.
```
