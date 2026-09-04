# L5 v3 Script S6 dependency audit — 2026-09-04

Qualified production SHA: `647b80d6fdc4472840cdf23be3c35ab5b42d5ab0`.

## Physics2D

- Canonical owner: `engine/domain/simulation/builtin/physics2d`.
- No repository-level `extensions/` directory exists; the architecture validator hard-fails if it returns.
- Public identities remain `lux::physics2d`, `<lux/engine/physics2d/...>`, `physics2d_simulation`,
  `lux-engine-physics2d`, `lux.physics2d.query` and `lux.physics2d.query.overlaps_box`.
- Box2D is PRIVATE. Eigen is PUBLIC because public component headers expose Eigen types.
- `simulation_composition` and serialization remain PRIVATE implementation dependencies.
- A clean DEVELOPER build with `LUX_BUILD_PHYSICS2D=OFF` passed 187/187 tests. Its `compile_commands.json` and
  Ninja target inventory contain no Box2D/Physics2D compile or target entry.

## Runtime closure

Generated runtime-dependency inventories are retained under
`E:/SyncForder/CodeRepos/build/RelWithDebInfo/s6-performance/`.

`lux_engine_simulation_script.dll` resolves only the description, asset, domain-system identity, ScriptArtifact and
Simulation-description DLLs. `lux_engine_scene_script_runtime.dll` adds Scene/Task/Process/Simulation runtime
composition dependencies. Neither closure includes Lua, LLVM, MLIR, FlowForge compiler, Box2D, Jolt, Toolchain or
Editor binaries.

Lua runtime implementation is linked only by the Lua targets. Toolchain-produced LXSA remains source-based and
VM-independent. Lua54's selected runtime DLL is staged explicitly by the private VM implementation target and is
installed as an implementation dependency, not encoded into ScriptArtifact.

## Ownership result

Module, Simulation, Process, Scene, Toolchain and Editor dependency directions remain unchanged. S6 introduces no
new Manager, registry, scheduler, service locator, plugin VM system or language-specific state in ScriptSystem.
