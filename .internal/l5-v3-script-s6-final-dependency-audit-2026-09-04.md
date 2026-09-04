# L5 v3 Script S6 final dependency audit — 2026-09-04

Qualified candidate revision: `f3178dfc2dd0f38959e16ab2df18c78898d69377`.

## Runtime boundary

`lux_engine_simulation_script.dll` directly depends on ScriptArtifact and Simulation description plus standard C++
runtime libraries. It does not depend on Lua, Native/FlowForge, LLVM, MLIR, Box2D, Process, Scene, Toolchain or
Editor binaries. `lux_engine_simulation_script_native.dll` adds only generic Script Native and ScriptArtifact
dependencies. NativeScriptBackend does not depend on FlowForge source or compiler packages.

Lua VM dependencies remain confined to the selected `script_lua` and `simulation_script_lua` implementation
targets. The same source ScriptArtifact is used for LuaJIT and Lua 5.4. Domain Ability owners do not link Lua;
generated Lua projectors are instantiated only in explicit Lua composition targets.

## Physics and optional closure

Physics2D remains at `engine/domain/simulation/builtin/physics2d`, with Box2D PRIVATE and Eigen public only because
the public component data uses Eigen types. `LUX_BUILD_PHYSICS2D=OFF` produces zero Physics2D/Box2D entries in
`compile_commands.json` and passes 187/187 Developer tests. Source names appear only as architecture-validator input
dependencies in the root Ninja graph, not as configured compile or runtime targets.

## Hot-plane dependency result

- C++ coroutine, FlowForge and Lua all terminate in the same ScriptBackendContinuation/Awaitable/ResumeRing owner.
- Native ABI v5 imports generated typed entries without introducing a FlowForge runtime dependency.
- Lua prepared entries contain instance-local provider receivers but no global authority or service lookup.
- External completion ingress owns no ScriptSystem/provider and publishes only bounded compact records.
- Static Ability specialization is source-composed and absent from persistent artifacts.

No new Manager, registry, service locator, scheduler, VM plugin system, second backend or cross-layer dependency was
introduced. `ValidateSourceArchitecture`, `ValidateSourceStyle`, both installed architecture validations and the
relocated consumers verify this closure.
