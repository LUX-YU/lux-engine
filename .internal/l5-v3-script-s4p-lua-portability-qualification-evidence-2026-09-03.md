# L5 v3 Script S4-P Portable Lua Qualification Evidence — 2026-09-03

## Qualified revision

- Production implementation: `b42e976dbcd3bc1536337ab04c217b67852aede9`.
- Qualification source: clean detached worktree
  `E:/SyncForder/CodeRepos/lux-engine-s4p-qualification` at that exact revision.
- `git status --short`, `git diff --exit-code`, `ValidateTrackedSnapshot.cmake`, and
  `ValidateSourceArchitecture.cmake` passed. The developer checkout's existing `.gitignore` and
  `WorldPartition.hpp` changes were not part of the qualified snapshot.
- Platform: Windows 11 Pro 10.0.26200, Intel Core i7-13700KF, 16 physical / 24 logical cores,
  MSVC 19.44.35228, x64 RelWithDebInfo.

## One backend architecture, selected VM implementation

`modules/function/script/lua` owns the narrow compatibility seam. `LuaScriptBackend` remains the only Simulation
Lua backend and still owns the per-instance object table, prepared Ability table, bounded Lua continuation records,
and adaptation to `ScriptBackendContinuation`. ScriptSystem never observes `lua_State` or the selected VM.

The compatibility implementation centralizes only the C API differences that affect semantics:

- VM policy/configuration and runtime information;
- `lua_resume` status/result-count normalization;
- the tail-yield operation used by an ASYNC_OPERATION binding.

Ordinary common C API calls are not wrapped. LuaJIT uses its 5.1-family `lua_resume(thread, nargs)` contract. Lua
5.4 uses `lua_resume(thread, caller, nargs, &nresults)`. Both produce the same private `LuaResumeResult`. Lua 5.4's
tail yield uses a backend-private continuation so resume arguments become the results of the original Lua Ability
call; this remains entirely inside the Lua package. No VM-native continuation enters ScriptSystem.

`ELuaExecutionPolicy::INTERPRETER_ONLY` disables LuaJIT's JIT through backend policy. It does not require gameplay
source to import or call `jit`. Lua 5.4 reports no JIT capability. Runtime metadata is diagnostic/benchmark data and
is not ScriptArtifact identity.

## Build and installed selection

The root cache setting is:

```text
LUX_LUA_VM=LUAJIT   # default
LUX_LUA_VM=LUA54
```

One build selects exactly one provider. There is no runtime VM registry, dynamic provider loading, second backend,
or mixed-VM backend. The installed `script_lua` package records the VM selected when that prefix was built and
materializes a generic private `lux::engine::function::script_lua_vm` link target. LuaJIT and Lua 5.4 were qualified
in separate clean build/install prefixes. Public Lux headers expose only Lux types and a forward-declared
`lua_State`; neither concrete VM is part of the Script semantic API.

Configuration checks compile the required Lua C API and require a `lua_Number` representation compatible with Lux
`f64`. Lua 5.4 qualification used Lua 5.4.8. LuaJIT qualification used LuaJIT 2.1.1771261233.

## Artifact identity and portable source profile

`Script::Kind::LUA_SOURCE` and its canonical UTF-8 source payload are unchanged. No ScriptArtifact schema or LXSA
wire revision was added for VM selection, and neither Lua bytecode nor a VM identifier is persisted.

The production packager generated `lua_portability_fixture.lxsa` once in the VM-independent TOOLCHAIN build. Its
SHA256 is:

```text
E5AECCF44A794C63AB4B2D20DF6C1B13855D1D8F8E44AFA62F25D77B2D054315
```

That exact file was consumed by LuaJIT JIT-on, LuaJIT interpreter-only, and Lua 5.4 tests. It includes custom-named
BeginPlay/EndPlay roles, synchronous state, QUERY, COMMAND, a copied BORROWED_STEP scalar, eager async completion,
Delay.nextStep, Simulation Delay, and two sequential awaits. Lifecycle counts, Ability results/call counts,
suspension ordering, failure behavior, multi-flight Event invocation, Hook single-flight, retirement, and late
completion semantics matched across all three configurations.

The installed external Inventory fixture was also packaged byte-identically by both installed prefixes:

```text
1B0D58089839C174A1D2C45525245098A4C6AAF62AD060C9B9193C992645F240
```

Lux Portable Lua Profile v1 is the source subset shared by LuaJIT 2.1's 5.1-family parser and Lua 5.4. Gameplay
fixtures do not import `ffi` or `jit`, use VM bytecode, or rely on VM-specific language/GC behavior. The packager is
not a second Lua parser; the selected VM still load/compiles source and fails closed on unsupported syntax.

## Portable scalar contract

The plain Lua production bridge supports exactly:

| Lux scalar | Accepted semantic range |
|---|---|
| `lux.bool` | `false`, `true` |
| `lux.i32` | `INT32_MIN` through `INT32_MAX` |
| `lux.u32` | `0` through `UINT32_MAX` |
| `lux.f32` | finite values representable by the canonical f32 conversion |
| `lux.f64` | Lux f64 semantics without VM-side narrowing |

`i32`/`u32` conversion is range-checked through `lua_Number` rather than assuming a 64-bit `lua_Integer`; boundary
tests cover both signed endpoints and `UINT32_MAX` in both directions. `lux.i64` and `lux.u64` are intentionally not
plain-Lua values in Portable Profile v1. The packager, Lua Ability contribution, component binding, and runtime
preparation reject them explicitly. This restriction is Lua-projection-only and does not reduce C++, Native ABI, or
FlowForge scalar support. Future 64-bit STABLE_ID values require an approved typed opaque representation rather than
an imprecise Lua number.

## Ability source naming

Canonical Ability metadata now separates:

- `ContractId`: stable runtime requirement/provider identity;
- `name`: stable language-facing identifier;
- `display_name`: human/editor text;
- `MethodId`: stable method identity;
- method `name`: language-facing member identifier.

Lua registers `lux.<Ability.name>.<Method.name>`. Display text is neither an identifier nor a lookup key and may
change without changing Lua source. Code-facing names are validated for uniqueness, while provider preparation and
dispatch remain ContractId/MethodId/schema based. C closures still retain only a catalog ordinal; there is no
per-call provider, contract-name, or method-name lookup.

## Cross-VM semantic qualification

All production semantics matched for LuaJIT JIT-on, LuaJIT interpreter-only, and Lua 5.4:

- custom lifecycle authoring and exactly-once BeginPlay/EndPlay/destruction;
- explicit coroutine-capable exports and explicit Ability requirements;
- exact prepared QUERY/COMMAND calls and provider isolation;
- rejection of undeclared Abilities;
- BORROWED_STEP scalar copy at the bridge boundary;
- NextStep and Simulation Delay stable-point ordering;
- eager completion still yielding and resuming only at a stable point;
- sequential re-suspension on one VM coroutine;
- raw `coroutine.yield()` without an engine awaitable failing closed;
- provider admission rejection, failed completion, and Lua errors before/after yield mapping to FAILED;
- multiple Event invocations with separate coroutine stacks and one shared object table;
- Hook single-flight owned by ScriptSystem;
- continuation destruction before EndPlay/table destruction and safe late completion.

READY resume values are copied from the owned Script resume packet into VM values. FAILED and CANCELLED do not
continue user Lua. External/worker completion only reaches the existing AwaitableIngress; Lua resume remains on the
caller-triggered stable-point path. No ScriptStepContext is stored across suspension.

## Build and test matrix

All configurations used full `all` builds with `-j 4 -k 0`, complete CTest, and a second no-op build:

| Selected VM | Profile | CTest | Second build |
|---|---|---:|---|
| LuaJIT | DEVELOPER | 184/184 | `ninja: no work to do` |
| LuaJIT | PLAYER | 184/184 | `ninja: no work to do` |
| LuaJIT | EDITOR | 197/197 | `ninja: no work to do` |
| Lua 5.4 | DEVELOPER | 184/184 | `ninja: no work to do` |
| Lua 5.4 | PLAYER | 184/184 | `ninja: no work to do` |
| Lua 5.4 | EDITOR | 197/197 | `ninja: no work to do` |
| VM-independent | TOOLCHAIN | 168/168 | `ninja: no work to do` |

TOOLCHAIN does not close over a Lua runtime backend; its package-once artifact output is VM-independent. The
LuaJIT build additionally ran the focused backend/coroutine/Scene/lifecycle/continuation suite with JIT explicitly
disabled.

One hundred repeat-until-fail runs passed independently for LuaJIT JIT-on, LuaJIT interpreter-only, and Lua 5.4 for
the VM coroutine contract, production Lua coroutine integration, and Scene Lua runtime. Logs are retained under:

```text
E:/SyncForder/CodeRepos/build/RelWithDebInfo/lux-engine-s4p-stress/
```

## Installed closure

Separate prefixes were installed and validated:

```text
E:/SyncForder/CodeRepos/install/RelWithDebInfo/lux-engine-s4p-luajit
E:/SyncForder/CodeRepos/install/RelWithDebInfo/lux-engine-s4p-lua54
```

Both passed `ValidateInstalledArchitecture.cmake`. Fresh relocated Lua packager/runtime, Script Ability codegen, and
Scene Script runtime consumers configured and built against each prefix. All six executables ran successfully with
their selected provider dependency available, and all consumer second builds reported no work. The two consumer
sets use the same C++, Ability declaration, and Lua source. Generated paths do not refer to the engine source tree.

Changed public module headers were synchronized byte-identically to the Debug, RelWithDebInfo, and Android install
include prefixes. Android was not configured or built, per the current qualification policy.

## Scope

- S2.4 AssetLoad remains blocked by the script-visible residency-backed Asset handle contract.
- S5 Event.await is not implemented.
- S5 Physics/Navigation production Abilities are not implemented.
- S6 C++ coroutine ergonomics/static specialization is not implemented.
- Python runtime is not implemented.
- Async BeginPlay/EndPlay is not implemented.
- Temporary Script activation/deactivation is not implemented.

Portable Lua S4-P is qualified. No S5 implementation was started.
