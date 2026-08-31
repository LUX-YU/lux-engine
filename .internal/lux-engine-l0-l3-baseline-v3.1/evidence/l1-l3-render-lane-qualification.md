# L1–L3 Retained Render Lane Qualification

- Qualification revision: `f7cc57c5dc8862eb883b357b208f7fce846fc859`
- Qualification product: `l1_l3_render_sync_3d_qualification`
- Classification: HOST / TEST / COMPOSITION
- Build: RelWithDebInfo, `LUX_BUILD_PACKED_RENDER_CONTENT=ON`

## Verified path

```text
typed Model/Mesh/Material assets
  -> pure Render uploads and ready RMeshHandle/RMaterialHandle replies
  -> Simulation Registry (Mesh3D + Light3D + WorldTransform3D + ResolvedMeshResources)
  -> feature-owned Mesh/Light RenderSyncStages
  -> generic RenderSystem transactional StateUpdate ring
  -> Presentation forwarding through RenderProgramSession
  -> retained RenderScene MeshBinding/LightBinding
  -> RenderProgram(Frame)
  -> real Vulkan offscreen render and readback
```

The corrected offscreen qualification performs eight logical transform updates. Every update is extracted as a narrow
TransformBatch after the initial handle-based Upsert. StateUpdate programs mutate retained RenderScene state without
advancing frame serial; each subsequent Frame renders the latest retained state. The recorded local run completed non-SKIP
in 0.62 seconds on a physical Vulkan device, produced 1,904 lit pixels, returned mesh/material handles `0:1`, and reported
zero validation errors. GPU-unavailable ordinary CTest uses return code 77 and is not counted as qualification success.

The independent opt-in visible-window qualification uses the real registered Transform Simulation with
`TaskExecutorConfig{0, 64}`. It recorded 23 Simulation steps and 150 Presentation frames. Simulation advanced from revision
8 to 13 while Presentation forwarding was intentionally paused for 200 ms; after forwarding resumed, revision 23 was the
latest visible state. Validation errors remained zero.

Default runtime closure and the explicit full-render closure both build independently. The full-render closure uses the
in-tree asset packer and compiles the complete Mesh/Light handlers. Model upload, corrected offscreen extraction, large
scene performance and visible-window lane qualification are distinct source products rather than macro modes of one TU.
`LatestSpscExchange` remains installed and tested for the independent compact latest-state workloads.
