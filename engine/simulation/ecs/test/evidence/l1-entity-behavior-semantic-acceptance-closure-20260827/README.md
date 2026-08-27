# L1 EntityBehavior Rev3 semantic-acceptance closure qualification

This evidence qualifies production and test commit
`b086dc7ef6f11b2a6c5a8ae053b837312c77675c`. The evidence-only commit that
adds this file has that production commit as its sole parent and contains no
production or test changes.

The sole normative source is
`doc/l1-entity-behavior-method-binding-quality-closure-refactor-spec-rev3.zh-CN.md`.
Its canonical Git-blob SHA-256 is
`F9D17D286CA97E6F1E4BD89FC2D9E99F53BBB1C2E86ABA97257585E735D4B18D`.
Revision 2 and all earlier qualification evidence remain historical only.

The independent rejection input is preserved as
`independent-freeze-review-rejection-3.md`, canonical SHA-256
`C7C1CFE989622CAFB907B0CA48BFD0EE08EE6F3E06EA47CC5811E9EC1EB44237`.
It rejects production/test commit
`dcb85fcb5fcf0124137e523b702885a2d2bb5fe0` and evidence HEAD
`d72cc8366d789533d00f8b6ed6a8eb81aa8017ca`; it is audit input, not a
normative contract.

## Workspace and exact-SHA boundary

Implementation used the clean linked worktree on
`codex/l1-entity-behavior-semantic-acceptance-closure`. Final qualification
used a separate clean detached worktree at the exact production SHA. The
original `codex/object-ui-foundation` worktree retained all 92 user-owned
status entries and was not reset, staged, or modified by this implementation.

A preliminary detached build under a long Windows path was discarded after
Ninja diagnosed an overlong pre-normalization relative depfile path as a
missing phony input. The same exact SHA was checked out at the short path
`E:/SyncForder/CodeRepos/w/b086` and all accepted matrices were rebuilt in
fresh short-path build trees. The accepted second builds all reported
`ninja: no work to do`.

## Toolchain

- Date: 2026-08-27, Europe/London.
- CMake 4.1.2; Ninja 1.11.1.
- MSVC 19.44.35228.0 from Visual Studio 2022 Developer PowerShell 17.14.35.
- Python 3.13.0 for the Tier-1 importer and benchmark evaluator.
- Android NDK 30.0.15729638, Clang 21.0.0, arm64-v8a, API 33.
- TOOLCHAIN qualification enabled `LUX_ENABLE_FLOWFORGE_MLIR=ON` against the
  installed x64-windows MLIR closure.

## Exact-SHA matrix

- RelWithDebInfo DEVELOPER: fresh 441-target `all -j 4 -k 0`, second build
  no-work, CTest 89/89, fresh install, source architecture scan, and installed
  architecture scan.
- Debug DEVELOPER: fresh 445-target build, second build no-work, CTest 94/94.
- RelWithDebInfo hardened DEVELOPER with
  `LUX_OBJECT_CONTRACT_CHECKS=ON` and `LUX_UI_CONTRACT_CHECKS=ON`: fresh
  445-target build, second build no-work, CTest 94/94. Simulation boundaries
  were covered by the L1 contract tests and source/install negative scans.
- RelWithDebInfo TOOLCHAIN with FlowForge/MLIR enabled: fresh 294-target
  build, second build no-work, CTest 73/73, and fresh install. The matrix
  includes FlowForge overload identity, Python dotted aliases, Lua/Python
  project-record import, canonical LXSA re-encoding, and owning authoring
  catalog tests.
- Android arm64-v8a PLAYER: fresh 404-target build, second build no-work,
  fresh install, and installed architecture scan. The install has 578 files
  and 35 shared libraries. Artifact-name and ELF `NEEDED` scans found no
  Lua/Python runtime, FlowForge, MLIR, or LLVM closure.
- All 11 installed consumers configured and built from fresh exact-SHA
  installs, ran successfully, and reported no work on their second builds:
  core-system, core-task, script-binding-authoring, simulation-asset,
  simulation-description, simulation-ecs, simulation-system, snapshot,
  system-hook-script-binding, world, and world-asset.
- The runtime consumer covers
  `LUX_METHOD -> RefMethod -> C++ bridge -> Script v4 -> LXSA v2 -> LXSD v4 ->
  ScriptComponent -> instance -> lifecycle -> MULTI Hook/Event`. The
  TOOLCHAIN-installed authoring consumer enumerates exports and targets,
  composes one export to multiple hooks, diagnoses invalid composition, and
  round-trips Builder/LXSD.

The installed canonical semantic catalog is LF-only and has SHA-256
`FF8E7AE6AC44AA58D26794B88454055E6F6059BEFEE9FDFC096C839CB0A3750E`.

## Benchmark v8

The RelWithDebInfo executable embeds the full production SHA. Every case used
five warmups and thirty retained samples. Evaluation used
`l0_l1_taskgraph_qualification.toml` and
`evaluate_l1_qualification.py --expected-commit` with the production SHA.

```text
PASS: 2 scaling, 1 ratio, 21 structural rules
```

| Metric | Size | Median ns | Allocations | Notifications | Callbacks | Entities examined | Range lookups | Handlers visited | Frame builds |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `reactive_dirty_patch` | 100,000 | 476,000 | 0 | 100,000 | 0 | 0 | 0 | 0 | 0 |
| `reactive_dirty_patch` | 1,000,000 | 4,841,850 | 0 | 1,000,000 | 0 | 0 | 0 | 0 | 0 |
| `command_buffer_record` | 1,000,000 | 13,881,500 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| `cpp_method_prepared` | 100,000 | 440,150 | 0 | 0 | 100,000 | 0 | 0 | 100,000 | 100,000 |
| `hook_global_multi` | 100,000 | 1,128,900 | 0 | 0 | 400,000 | 0 | 0 | 400,000 | 100,000 |
| `hook_entity_multi` | 10,000 subscribers | 34,200 | 0 | 0 | 10,000 | 0 | 0 | 10,000 | 1 |
| `global_event` | 100,000 | 508,550 | 0 | 100,000 | 100,000 | 0 | 0 | 100,000 | 100,000 |
| `entity_targeted_event_sparse` | 100,000 total | 1,253,800 | 0 | 100,000 | 100,000 | 100,000 | 100,000 | 100,000 | 100,000 |
| `entity_targeted_event_sparse` | 1,000,000 total | 1,402,400 | 0 | 100,000 | 100,000 | 100,000 | 100,000 | 100,000 | 100,000 |
| `entity_targeted_event_sparse` | 2,000,000 total | 1,497,050 | 0 | 100,000 | 100,000 | 100,000 | 100,000 | 100,000 | 100,000 |
| `owned_worker_event_buffer` | 1,000,000 | 8,397,050 | 0 | 1,000,000 | 1,000,000 | 0 | 0 | 0 | 1,000,000 |

Reactive scaling exponent was `1.007404` against the `1.25` limit. Sparse
targeted-Event exponent was `0.048644` against the `1.30` limit. The 2M/1M
median ratio with identical 10,000 scripted entities and 100,000 occurrences
was `1.067491` against the `1.20` limit. The corrected EntityBehavior Hook
case performs one invocation over 10,000 contiguous subscribers with
`entities_examined == 0`, `target_range_lookups == 0`, one frame build, and
exact callbacks. All qualified hot paths recorded zero Lux-side allocation.

Raw CSV SHA-256 digests:

```text
812AB0F87377FD93AA31BC651B96F73A162BD2BEF83ADD1753B67D14E01CAD9D  command-buffer-1000000.csv
D1BC52D57860B7A10E193A9DB0895455676E4835D8998408BA5C3030D984634E  cpp-method-prepared-100000.csv
C21BF7280BB0E3F6908D0191FF2FB5898585FEABF9B74FC14183D1C4A6A57D08  entity-targeted-event-sparse-100000.csv
57630A9BA7DFFC06EC92145A7D889D91FD99B19F59F6713F17EE02B50D807B69  entity-targeted-event-sparse-1000000.csv
843404FC9AA4357A645FAB72B93FC00B18CC255F48B0389EF8A67114BB0C088C  entity-targeted-event-sparse-2000000.csv
86AA9E7DB47DA9652190A3B64EBA2F473213BDC079571435075900713814A41A  global-event-100000.csv
13F06F1F59E5B30967B8C9A65AABEF61020B615F3F17325FCC4F4ECC3D2A19B1  hook-entity-multi-10000.csv
69854C0026D37F88E8045BEFC81BD511811E8EE4F4F74E18563E6724C49F8BA2  hook-global-multi-100000.csv
98827936521E4195C07D2EEDFC27A2A6B8DF56EF47108E9220E2B896C694CA5D  owned-worker-event-buffer-1000000.csv
5633276E514F09FBE114ADE29D2A09CD785D4A62F5E4087A433DF244EAB6DF5E  reactive-dirty-100000.csv
A4C23E69EA3A9EDE70EF2B2AE43A8BF0DC75D2B304D7B8E0A9F34E12223B5039  reactive-dirty-1000000.csv
```

Selected artifact SHA-256 digests:

```text
447B57D00DBC4074D7B23F752F7AEB46C2EB79AE9061199D12AA2966FD5AC026  ecs_l1_benchmark.exe
10452D4A9CFBE00A0D266BEF9C46A0AA04228683A0158B8BA375CFCB85359952  lux_engine_simulation_description.dll
5331CE569A3CCE13048F18F21B12A2260D06F734CE2A906C9541705DA5AD2BFA  lux_engine_simulation_script_binding.dll
8BE446F758BB96969101C805A50DCF4C137E8BFB79326060CD41E4E13E2C7983  lux_engine_simulation_script_binding_cpp_static.dll
0F294C7CF0D610DF9AC4639183878F6D38BF4F63DED488ED9FFE9AB6BB1F0139  lux_engine_simulation_script_binding_native.dll
3AE5715AD261329DF1871A60BE97332BAFEFA33F27A02F313BB9AB9B546245BB  lux_engine_simulation_script_binding_lua.dll
B23C30A1F10FA6C4C955AA071B088E8D41AC8D85FC86E65C938A12FD65F3FF11  liblux_engine_simulation_script_binding.so
27B542391665F71BC23E9D2FDAF9A3A2963667CF71A161B5FCB48870ACDD3990  liblux_engine_simulation_script_binding_native.so
```

## Status boundary

```text
L1 EntityBehavior Rev3 closure           Freeze Candidate
L1 Hook/lifecycle/Method contract        Freeze Candidate
L1 exact-SHA build/performance matrix    Qualified
Independent API/semantic acceptance      Required
L1 formal freeze                         NO-GO pending acceptance
Formal L2                                BLOCKED
```

This evidence does not declare L1 FROZEN and does not authorize formal L2
implementation.
