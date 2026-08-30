# Preloaded Spatial3D Vertical Slice Evidence

- Full-render build seam: `1ac6821c`.
- Probe revision: `7c30c7be433e9258a4b29dac5321fe1f13b04b5c`.
- Product: `architecture_probe_spatial3d_preloaded`, HOST / TEST / COMPOSITION.

## Independent build configuration

The default Developer/Player closure keeps packed render content and this probe OFF. Qualification used a separate build
tree with:

```text
LUX_BUILD_PACKED_RENDER_CONTENT=ON
LUX_BUILD_PRELOADED_SPATIAL3D_PROBE=ON
LUX_ASSET_PACKER_EXECUTABLE=<compatible host lux_asset_packer>
```

The explicit full-render configuration compiled the real render feature implementation, generated server operation bodies,
compiled/packed/embedded builtin shaders, and linked the probe to `render_features` and `render_vulkan`. Its second probe
build reported `ninja: no work to do`; focused CTest passed 1/1 on a real GPU. The ordinary default-OFF build separately
reported `ninja: no work to do` and 110/110 CTest passed.

## Vertical slice

The probe executes this concrete sequence:

```text
World metadata + encoded sidecar
  -> WorldStorageSource range IO with byte accounting
  -> loadWorldPartition
  -> WorldMaterializer into final-address Scene Registry
  -> concrete PreloadedSpatial3DSystem through SystemRegistration/Simulation
  -> concrete Spatial3D index query
  -> Jolt double-position fixed step/contact at 1e12
  -> LatestSpscExchange compact presentation state
  -> subtract double render origin, then narrow to float
  -> real Vulkan Texture / GraphMaterial / Mesh upload
  -> real mesh instance / RenderGraph / GPU readback
```

Result at the exact probe revision:

```text
io_submits=6,accounting=1,entities=1,partition=0,jolt_contact=1,physics_x=1000000000000.205078125,render_relative_x=0.205078125,texture=1,material=1,mesh=1,instance=1,lit_pixels=338,validation_errors=0
```

All asset references are concrete probe-local values. No streaming/demand/residency/timing/ingress production type was
created. This preloaded slice proves readiness and rendering, but by itself does not establish duplicate-interest, retry,
release or generation ownership semantics and therefore does not close Barrier A.
