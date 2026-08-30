# Real Jolt L1 Qualification Evidence

## ABI and dependency

- Overlay/bootstrap revision: `a57c75e3`.
- Probe revision: `49404081acb1e18c2efacd442a8d07e94d701e87`.
- Jolt: 5.5.0, vcpkg feature `double-precision`, host and Android packages rebuilt from the repo-owned overlay.
- Exported host and Android `Jolt::Jolt` targets both contain `JPH_DOUBLE_PRECISION`.
- The probe independently enforces `JPH::Real == double` and `JPH::RVec3 == JPH::DVec3` at compile time.

## Correctness

- A concrete `JoltProbeSystem` was installed through `SystemRegistration` and `SimulationBuilder`.
- The System contributed exactly one primary Task, which called the real `JPH::PhysicsSystem::Update()`.
- Create/get/move/restore preserved the required `0.125` relative position at origin and `1e12`.
- Fixed-step contact and narrow-phase raycast passed at both locations.
- Query reconstruction subtracts the double query base before narrowing the relative result to float, then restores the
  absolute double position.
- No persistent origin, rebase operation, runtime precision variant, or public JPH type was added.

## Qualification results

Command: `architecture_probe_jolt_l1.exe --qualification`

```text
scenario=near,step_ms=8.8716,bodies=110000,active=10000,broadphase_bodies=110000,contacts=10000,body_bytes=17600000,allocations=220415,allocated_bytes=327054052,workers=3,dynamic_relative_x=0.204999987,ray_relative_x=-0.25
scenario=far,step_ms=8.8927,bodies=110000,active=10000,broadphase_bodies=110000,contacts=10000,body_bytes=17600000,allocations=220415,allocated_bytes=327054052,workers=3,dynamic_relative_x=0.205078125,ray_relative_x=-0.25
```

The large qualification spaces independent body pairs by at least four float broadphase ULPs. The mandatory pair remains at
`1e12` and `1e12 + 0.125`. An exploratory meter-spaced far-origin run exhausted the Jolt body-pair cache because Jolt's
broadphase remains float even in double-position mode. This is retained as product evidence: a future dense 3D slice must
measure concrete spatial partitioning; it does not authorize an origin/rebase framework.

## Build and tests

- RelWithDebInfo full build completed; the second CMake/build pass reported `ninja: no work to do`.
- Full CTest: 109/109 passed.
- The Jolt CTest mode uses the same real update/contact/raycast path with a small body count.
