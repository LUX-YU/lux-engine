# L1–L3 Retained Render Lane Qualification

- Baseline: `909338d31ac77d9515df95ba00d8f7f676d28a14`
- Qualification product: `l1_l3_render_sync_3d_qualification`
- Classification: HOST / TEST / COMPOSITION
- Build: RelWithDebInfo, `LUX_BUILD_PACKED_RENDER_CONTENT=ON`

## Verified path

```text
typed Model/Mesh/Material assets
  -> explicit AssetId uploads and ready replies
  -> Simulation Registry (Mesh3D + Light3D + WorldTransform3D)
  -> RenderSystem dirty/coalesced StateUpdate ring
  -> Presentation forwarding through RenderProgramSession
  -> retained RenderScene MeshBinding/LightBinding
  -> RenderProgram(Frame)
  -> real Vulkan offscreen render and readback
```

The qualification performs 40 logical transform updates at 20 Hz. StateUpdate programs mutate retained RenderScene state
without advancing frame serial; each subsequent Frame renders the latest retained state. The recorded local run completed
non-SKIP in 2.66 seconds on a physical Vulkan device. Validation was enabled, readback contained lit pixels and the test
returned success. GPU-unavailable ordinary CTest uses return code 77 and is not counted as qualification success.

Default runtime closure and the explicit full-render closure both build independently. The full-render closure uses the
in-tree asset packer and compiles the complete Mesh/Light handlers. `LatestSpscExchange` remains installed and tested for
the independent compact latest-state workloads.
