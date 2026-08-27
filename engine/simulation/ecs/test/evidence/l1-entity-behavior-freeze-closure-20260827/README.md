# L1 EntityBehavior final quality-closure qualification

This evidence qualifies production and test commit
`dcb85fcb5fcf0124137e523b702885a2d2bb5fe0`. The evidence-only commit that
adds this file has that production commit as its sole parent and contains no
production or test changes.

The contract source remains
`doc/l1-entity-behavior-method-binding-quality-closure-refactor-spec.zh-CN.md`
with canonical SHA-256
`843118EA52385E71BF3347F07D94D58A63D6B5B3DD95590A3EEE95950D05DB28`.
The independent rejection input is preserved as
`independent-freeze-review-rejection-2.md`, SHA-256
`F6C9C4E542763A6CFA890C0C20E70AEE8EE55CE24165AB6B2BEFB02E381930C0`.
It rejects the former production/test SHA
`4d9a9069c09c132c9b6274187ff79259c2696ad0` and is not a replacement
implementation specification.

## Workspace isolation and corrected exact-SHA boundary

Implementation used a clean linked worktree on
`codex/l1-entity-behavior-freeze-closure`. Qualification used a separate clean
detached worktree. The original `codex/object-ui-foundation` worktree was not
reset, staged, or modified. It began this implementation with 89 user-owned
status entries and contained 92 when qualification completed; the additional
entries appeared independently while this work ran.

The first qualification candidate,
`6341e5e8c23a214de8d2778a8948d8a84818f430`, was rejected during the
TOOLCHAIN matrix. A clean Windows checkout expanded the source semantic JSON
to CRLF, so the installed catalog was not byte-canonical. Production commit
`dcb85fcb5fcf0124137e523b702885a2d2bb5fe0` configures, tests, and installs a
generated LF-only catalog. All final matrices and benchmark data below were
rerun from that corrected exact SHA in a new build root.

## Toolchain

- Date: 2026-08-27, Europe/London.
- CMake 4.1.2; Ninja 1.11.1.
- MSVC 19.44.35228 for x64 from Visual Studio Developer PowerShell.
- Python 3.13.0 for the Tier-1 importer and benchmark evaluator.
- Android NDK 30.0.15729638-beta2, Clang 21.0.0, arm64-v8a, API 33.
- FlowForge qualification enabled the installed x64-windows MLIR closure.

## Exact-SHA matrix

- RelWithDebInfo DEVELOPER: fresh 441-target `all -j 4 -k 0`, second build
  `ninja: no work to do`, CTest 89/89, fresh install, source architecture scan,
  and installed architecture scan.
- Debug DEVELOPER: fresh 445-target `all -j 4 -k 0`, second build no-work, and
  CTest 94/94.
- RelWithDebInfo hardened DEVELOPER with
  `LUX_OBJECT_CONTRACT_CHECKS=ON` and `LUX_UI_CONTRACT_CHECKS=ON`: fresh
  445-target build, second build no-work, and CTest 94/94. Simulation contract
  boundaries were additionally covered by the source/install scans and L1
  negative tests.
- RelWithDebInfo TOOLCHAIN with `LUX_ENABLE_FLOWFORGE_MLIR=ON`: fresh
  294-target build, second build no-work, CTest 73/73, and install into the
  exact-SHA prefix. This includes FlowForge, Python Tier-1, Lua static importer,
  canonical LXSA re-encoding, and canonical semantic-catalog tests.
- Android arm64-v8a PLAYER: fresh 404-target build using NDK 30 and API 33,
  second build no-work, fresh install, and installed architecture scan. The
  installed closure contains no Lua/Python runtime, FlowForge, MLIR, or LLVM
  artifacts.
- All 11 installed consumers configured and built against the fresh exact-SHA
  prefix, reported no work on their second builds, and ran successfully:
  core-system, core-task, script-binding-authoring, simulation-asset,
  simulation-description, simulation-ecs, simulation-system, snapshot,
  system-hook-script-binding, world, and world-asset.
- The runtime consumer exercises
  `LUX_METHOD -> RefMethod -> C++ bridge -> Script v4 -> LXSA v2 -> LXSD v4 ->
  ScriptComponent -> instance -> lifecycle -> MULTI Hook/Event`. The authoring
  consumer enumerates exports/targets, composes one export to multiple hooks,
  diagnoses invalid composition, and round-trips Builder/LXSD.

The installed canonical catalog is LF-only and has SHA-256
`FF8E7AE6AC44AA58D26794B88454055E6F6059BEFEE9FDFC096C839CB0A3750E`.

## Benchmark v8

The RelWithDebInfo executable embeds the full production SHA. Every case used
five warmups and thirty retained samples. Evaluation used
`l0_l1_taskgraph_qualification.toml` and
`evaluate_l1_qualification.py --expected-commit` with the production SHA.

```text
PASS: 2 scaling, 1 ratio, 16 structural rules
```

| Metric | Size | Median ns | Allocations | Notifications | Callbacks | Entities examined | Range lookups | Handlers visited |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `reactive_dirty_patch` | 100,000 | 473,050 | 0 | 100,000 | 0 | 0 | 0 | 0 |
| `reactive_dirty_patch` | 1,000,000 | 4,718,500 | 0 | 1,000,000 | 0 | 0 | 0 | 0 |
| `command_buffer_record` | 1,000,000 | 13,832,200 | 0 | 0 | 0 | 0 | 0 | 0 |
| `cpp_method_prepared` | 100,000 | 727,300 | 0 | 0 | 100,000 | 0 | 0 | 100,000 |
| `hook_global_multi` | 100,000 | 1,540,850 | 0 | 0 | 400,000 | 0 | 0 | 400,000 |
| `hook_entity_multi` | 100,000 | 916,900 | 0 | 0 | 100,000 | 100,000 | 100,000 | 100,000 |
| `global_event` | 100,000 | 538,650 | 0 | 100,000 | 100,000 | 0 | 0 | 100,000 |
| `entity_targeted_event_sparse` | 100,000 total | 1,452,450 | 0 | 100,000 | 100,000 | 100,000 | 100,000 | 100,000 |
| `entity_targeted_event_sparse` | 1,000,000 total | 1,764,150 | 0 | 100,000 | 100,000 | 100,000 | 100,000 | 100,000 |
| `entity_targeted_event_sparse` | 2,000,000 total | 1,749,300 | 0 | 100,000 | 100,000 | 100,000 | 100,000 | 100,000 |
| `owned_worker_event_buffer` | 1,000,000 | 8,496,800 | 0 | 1,000,000 | 1,000,000 | 0 | 0 | 0 |

Reactive scaling exponent was `0.998897` against the `1.25` limit. Sparse
targeted-dispatch exponent was `0.067691` against the `1.30` limit. The 2M/1M
median ratio with identical 10,000 scripted entities and 100,000 occurrences
was `0.991582` against the `1.20` limit. `entities_examined == occurrences`,
callbacks/notifications are exact, and all qualified update paths recorded
zero Lux-side allocation.

Raw CSV SHA-256 digests:

```text
DD5BD97F22E5C28418D190103D1485D7A32374CA064EE16677E26020C3790C3E  command-buffer-1000000.csv
081EB70004B7BD51216E1B8B389F743FC392AA587C2914287D0184DBC1EFACDE  cpp-method-prepared-100000.csv
12598A44C5256AB616D85F162F12CB813E6D83EE75C7D2405632812B6B688A61  entity-targeted-event-sparse-100000.csv
9D45A472FF36DF1C9762660CFD7D1C0EE1007DD4F952AD063DA5E1E7E43AE843  entity-targeted-event-sparse-1000000.csv
E6A8413120EBB560B6BF4781210E1584CF6EE72906A5446FEF9B8FFA1BDF7FC1  entity-targeted-event-sparse-2000000.csv
64B3A5B7EF55697B0621725109CE8BCB9EEAFDB9B4C27747F68C0EC1F5BC2B3C  global-event-100000.csv
619F6DD6BDEFAF5A16D46500DA84C2278EAE2A25F1C4FF7A314B6492911CD8B0  hook-entity-multi-100000.csv
5E09490FEE41A7DDDE371DF9043F1113F79FFE17F39185C341B93B50075A902D  hook-global-multi-100000.csv
AE831403D7E6076C62F2AC75A206E1B0CDED355F2429BC8FB935DD5ABD92D845  owned-worker-event-buffer-1000000.csv
2FFCA2A0837544E8307475532098C3DD135DCEAA1436B984D1FB9AACBDE867CD  reactive-dirty-100000.csv
C1DF607087457859230C8A3D53841F9494743A4FE887CDAE23DA4EA226A620FB  reactive-dirty-1000000.csv
```

Selected artifact SHA-256 digests:

```text
AC62EF50A6365C50A261D23DCE43E1A1335D6599DD39C0B9930A047F1F44057B  ecs_l1_benchmark.exe
AFAEEF62FA23C0CAA1706C7A09B608CFD02F8C9E9F2ED547F214480625D22725  lux_engine_simulation_description.dll
31C4B2BBB854AADFFD305AC317545FDFACF76F51878C58CE5323ED5A5E9A8151  lux_engine_simulation_script_binding.dll
8F92EB00C3536A023815FD9E5CA2127FAFFDFCFBE18B03259E91E38F7D4347AA  lux_engine_simulation_script_binding_cpp_static.dll
AA42F3FDC6D12D5B9D088B19AD0AE8759AFF9059BD469A1EE9B18D5D8A0E9D6E  lux_engine_simulation_script_binding_native.dll
6EE3EF48FEC15ED3FDE9BA4E79A08B5D9CFF487E68D179AB27CC400D3E6AD676  lux_engine_simulation_script_binding_lua.dll
BBFACD83AC7350B021B6A841C278E384D3204B85147747EA9A82E131A4CB026C  liblux_engine_simulation_script_binding.so
7E346875FBA8146B14E54B505AE65B27E8701641178C78FDB03BBACDB3F7C3B1  liblux_engine_simulation_script_binding_native.so
```

## Status boundary

```text
L1 EntityBehavior quality closure        Freeze Candidate
L1 explicit Method Binding contract      Freeze Candidate
L1 owned/sparse Event dispatch contract  Freeze Candidate
L1 exact-SHA build/performance matrix    Qualified
Independent API/semantic acceptance      Required
L1 formal freeze                         NO-GO pending acceptance
Formal L2                                BLOCKED
```

This evidence does not declare L1 FROZEN and does not authorize formal L2
implementation.
