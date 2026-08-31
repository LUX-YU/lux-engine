# Large 3D Scene Performance Qualification

- Code revision: `be666c3138eba25f4b371f2374db613d0e5a8674`
- Product: `large_3d_scene_performance_qualification`
- Classification: HOST / TEST / COMPOSITION
- Configuration: RelWithDebInfo, full Render content, real physical Vulkan GPU
- OS: Windows 11 Pro 10.0.26200
- CPU: Intel Core i7-13700KF, 16 cores / 24 logical processors
- GPU: NVIDIA GeForce RTX 4070 Ti, driver 591.86
- Vulkan device API: 1.4.325

## Workload

- 1280 x 720 offscreen Render target with GPU readback.
- 100 x 100 retained Render entities (10,000 total).
- One shared typed Mesh/Material/Texture asset set loaded through Pak/VFS and explicit AssetId upload.
- The source triangle is expanded to a 64-triangle local cluster: 192 vertices per instance, approximately 640,000
  submitted scene triangles before culling.
- One directional light and 32 colored point lights.
- Initial full `RenderSystem` synchronization followed by 20 updates; each update patches 1,000 entities.
- 80 measured frames, including 60 stable-state frames.
- Vulkan validation enabled; validation error count must remain zero.

## Result

```text
server_instances=10000
server_lights=33
full_sync_ms=0.831
initial_state_apply_ms=34.424
state_update_mean_ms=0.939
frame_mean_ms=0.290
frame_p95_ms=0.691
gpu_frame_mean_ms=0.326990
gpu_frame_p95_ms=0.332288
gpu_samples=79
lit_pixels=72499
luminance_variance=0.01233527
validation_errors=0
```

`full_sync_ms` measures Simulation-lane collection and Program construction. `initial_state_apply_ms` includes forwarding,
Render-thread creation of all 10,000 retained bindings and the Program reply. `state_update_mean_ms` includes patching
1,000 transforms, dirty coalescing, Program publication, Render-thread application and reply. CPU frame time is the
blocking host round trip used by the fixture; GPU frame time comes from Vulkan timestamp queries after warmup.

The first cold full-render run separately observed approximately 704.9 ms of graph/pipeline compilation, of which about
701.8 ms was graphics pipeline creation. The steady-state numbers above intentionally exclude that one-time cost.

## Image qualification

The test writes `test/architecture_probes/large_3d_scene_performance.ppm` in the full-render build tree. The image was
converted to PNG for inspection. The high-angle view shows the full 100 x 100 field, stable perspective/depth ordering,
textured PBR shading and the colored light distribution. The automatic image gate also requires more than 1/50 of the
frame to be lit and luminance variance above `1e-4`.

## Scope limit

This is an instance/culling/material/lighting workload, not a complete Product streaming benchmark. It deliberately uses
shared resident assets and excludes World IO, Asset residency, Jolt, skeletal animation, terrain and swapchain present.
Those need separate workloads so their ownership and latency are not hidden inside this baseline.
