# L1-L3 Post-Cleanup Qualification Evidence

Date: 2026-09-01

Starting baseline: `a39246eeff3542af6345a96f586980e96ba1c367`

Qualified implementation revision: `48216e1b`

## Commit sequence

```text
d87172e2 docs(architecture): define post-closure topology cleanup
246a35f1 fix(scene-meta): enforce metadata referential integrity
c16ec9da refactor(world): converge description partition and identity topology
5a984b1d refactor(process): name async workflows by responsibility
4602b680 refactor(simulation): align package names with runtime roles
b65531f8 refactor(scene): align composition and integration topology
48216e1b docs(architecture): freeze semantic source topology
```

## Default qualification

Configuration: `RelWithDebInfo`, `LUX_BUILD_PROFILE=DEVELOPER`,
`LUX_BUILD_PACKED_RENDER_CONTENT=OFF`.

- Full `all -j 4 -k 0`: passed.
- Immediate second full build: `ninja: no work to do`.
- Complete CTest: **150/150 passed**.
- Final restored-OFF build: passed; immediate second build was no-work.
- Final restored-OFF CTest: **150/150 passed**.
- `world_partition_benchmark`: passed (exit 0). At 1M partitions, root build retained 381 bytes and completed in
  23,800 ns; 1M page lookups completed in 3,448,400 ns. The 100k-object layout build completed in 23,156,500 ns.
- Source architecture gate: passed.
- Post-cleanup install-path scan: all retired package/component/include paths from Waves 2-4 are absent.

The following installed consumers independently configured and built against the RelWithDebInfo install prefix:

```text
world-identity
partition-identity
world-partition
world-description
simulation-composition
process-asset-loading
process-world-loading
scene-composition
scene-presentation
scene-world-materialization
scene-render
dedicated-scene
```

The dedicated Scene consumer links no Render, Vulkan or window package. Architecture negative tests continue to
reject Simulation-to-World and Process-to-Scene dependencies.

## Full Render qualification

Configuration: `RelWithDebInfo`, `LUX_BUILD_PROFILE=DEVELOPER`,
`LUX_BUILD_PACKED_RENDER_CONTENT=ON`.

- Full build: passed.
- Immediate second full build: `ninja: no work to do`.
- Complete CTest: **163/163 passed**, no qualification skipped.

Host:

```text
OS: Microsoft Windows 11 Pro 10.0.26200
CPU: 13th Gen Intel(R) Core(TM) i7-13700KF
GPU: NVIDIA GeForce RTX 4070 Ti
Vulkan device API: 1.4.325
Vulkan driver: 591.86.0.0 (Windows display driver 32.0.15.9186)
Validation layer: VK_LAYER_KHRONOS_validation 1.4.304
```

Explicit qualification results:

```text
Texture: BC3_SRGB, 14 mips, 44,740,096 cooked bytes, upload=1,
         lit_pixels=109, validation_errors=0
Model: model=1, mesh_handle=0:1, material_handle=0:1, validation_errors=0
SceneSystem Render: scene_system_path=1, lit_pixels=1904,
                    mesh_handle=0:1, material_handle=0:1, validation_errors=0
10k scene: 10,000 instances, 33 lights, 3 model primitives, 192 mesh vertices,
           full_sync=2.469 ms, initial_state_apply=32.259 ms,
           state_update_mean=0.772 ms, frame_mean=0.527 ms,
           frame_p95=1.256 ms, gpu_frame_mean=0.520297 ms,
           gpu_frame_p95=0.537472 ms, validation_errors=0
```

Screenshot evidence:

```text
E:/SyncForder/CodeRepos/build/RelWithDebInfo/lux-engine/test/architecture_probes/large_3d_scene_performance.ppm
1280x720, SHA-256 B129410B461FAE16A7F60DED3A20499E529005819BA8F2D72DB9E7F3DFD53E17
```

The screenshot was visually inspected: the 10k mesh grid, perspective depth, material variation and lighting are
visible; no black/empty frame, systematic resource mismatch or gross clipping regression was observed.

## User-owned changes

The existing `.gitignore` change and the formatting-only patch in
`engine/domain/world/partition/include/lux/engine/world/WorldPartition.hpp` were preserved and remain uncommitted.

## Conclusion

```text
L1-L3 architecture remains closed.
Metadata referential integrity is fail-closed.
Source topology names semantic roles rather than historical aggregation.
L4 Authoring / L5 Editor / L6 concrete Host runtime may proceed.
Product streaming remains held.
CI remains deferred.
```
