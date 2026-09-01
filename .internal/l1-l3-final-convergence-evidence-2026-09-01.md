# L1-L3 Final Convergence Evidence — 2026-09-01

Implementation revision: `64ffca58` (`feat(render): converge runtime scene system`).

## Default runtime closure

- Configuration: RelWithDebInfo, `LUX_BUILD_PACKED_RENDER_CONTENT=OFF`.
- Full build: `target all -j 4 -k 0` passed.
- Immediate second build: `ninja: no work to do`.
- CTest: 149/149 passed.
- `render_scene_system_test` verifies static metadata without activation, missing-provider failure, successful
  SceneDescription -> Scene::create -> RenderSystem installation, stable/presentation hooks and RenderScene release.
- Installed consumers: `scene-render` and `dedicated-scene` both configured, built and ran from the installed prefix.
  The dedicated consumer links Scene/World/Simulation only and does not link scene runtime Render, Vulkan or window.

## Full Render closure

- Configuration: RelWithDebInfo, `LUX_BUILD_PACKED_RENDER_CONTENT=ON`.
- Full build: `target all -j 4 -k 0` passed after correcting Render client metadata DLL visibility.
- Immediate second build: `ninja: no work to do`.
- CTest: 162/162 passed, including all four explicit Vulkan qualifications.
- The main L1-L3 qualification now uses the final composition path and no longer creates stages or a
  RenderSyncPipeline directly:

  ```text
  SceneMetaManager -> SceneDescription(RenderSystem) -> Scene::create
  -> QualificationRenderRuntime -> RenderScene/Features/Stages
  -> Simulation execute -> Scene stable point -> Scene Presentation
  -> Host Frame -> Vulkan retained scene -> readback
  ```

- Direct result: `scene_system_path=1`, `lit_pixels=1904`, validation errors `0`.
- Texture and Model Vulkan qualifications passed.

## Real GPU environment

- OS: Microsoft Windows 11 Pro `10.0.26200`.
- CPU: Intel Core i7-13700KF.
- GPU: NVIDIA GeForce RTX 4070 Ti.
- Vulkan instance: `1.4.321`; device API: `1.4.325`.
- Driver: NVIDIA `591.86`.
- Validation layer enabled by qualification; validation error count: `0`.

## 10k retained-scene evidence

- Entities: 10,000; lights: 33; approximately 640k triangles.
- Lit pixels: 162,342; luminance variance: `0.03477081`.
- Full sync: `2.354 ms`; initial StateUpdate apply: `31.706 ms`.
- Incremental StateUpdate mean: `0.711 ms`.
- CPU frame mean/p95: `0.417 / 0.707 ms`.
- GPU frame mean/p95: `0.515538 / 0.525056 ms` over 79 samples.
- Validation errors: `0`.
- The 1280x720 screenshot was visually inspected: geometry, material appearance, depth distribution and lighting are
  coherent; no missing mesh wall, invalid transform fan-out or obvious protocol corruption was observed.

## Remaining holds

Asset residency/demand ownership, ScriptSystem capability injection, product streaming, plugin hot reload and generic
timing/ingress abstractions remain held. No Manager/Context/Services/Registry glue was introduced by this convergence.
