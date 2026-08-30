# Real Presentation / Render Lane Qualification Evidence

- Probe revision: `0dc1910834c1d534ac4fb86a2713a75d801befc6`.
- Product: `architecture_probe_render_lanes`, classified HOST / TEST / COMPOSITION.
- GPU path: `DeviceRenderFixture` -> real `GeneralRenderServer` thread -> Vulkan device -> offscreen target readback.
- Lane path: Simulation thread -> `LatestSpscExchange<PresentationState>` -> Presentation/main thread ->
  `RenderFrameSession` -> existing `RenderFrameChannel` / `BoundedSpscFrameRing` -> Render thread.
- Validation layer: enabled; error count checked after server/device destruction.

## Qualification result

Command: `architecture_probe_render_lanes.exe --qualification`

```text
gpu=1,simulation_steps=3,publishes=3,acquires=3,exchange_skips=0,presentation_frames=318,render_replies=318,presentation_fps=144.418,render_fps=144.418,frame_ring_backpressure=2,frame_skips=2,max_publish_ns=600,max_presentation_latency_ns=240700,validation_errors=0,readback=1
```

The Simulation step period was one second; Presentation remained at approximately 144 Hz and continued pumping window,
control, upload and frame replies. A deliberate no-pump burst filled the bounded frame ring; Presentation counted/skipped
the unavailable opportunities, then resumed after pumping. Simulation publication did not access or wait on the frame ring.
All 318 submitted frames produced Render replies, Vulkan readback succeeded, and validation errors remained zero.

## Normal qualification

- Full RelWithDebInfo build completed; second build reported `ninja: no work to do`.
- Full CTest: 110/110 passed.
- Ordinary GPU-unavailable CTest reports SKIP through return code 77; the recorded qualification run was non-SKIP on a
  real Vulkan device.
- The probe contains no live Registry/Scene traversal and introduces no timing, ingress, streaming or demand production type.
