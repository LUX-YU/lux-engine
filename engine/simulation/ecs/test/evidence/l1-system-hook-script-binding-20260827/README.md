# L1 SystemHook and ScriptBinding freeze-candidate qualification

This record qualifies production commit
`393180e9a67e01c9e6863b4d0bde4a8849b05b0c`, based on
`99f984cf0eb0ee75f45d21a5f1dd5d7c60c4b9be`. The evidence commit that adds
this file has that production commit as its sole parent and contains no
production or test changes.

The original `codex/object-ui-foundation` worktree remained at the base SHA
with all 69 user-owned status entries untouched. Qualification used clean,
detached exact-SHA worktrees. A short Windows worktree path was used for the
qualified Hardened, Android, and TOOLCHAIN builds so Ninja could stat generated
TU dependencies without the legacy Windows long-relative-path false-dirty
condition.

## Toolchain

- Date: 2026-08-27, Europe/London.
- CMake 4.1.2; Ninja 1.11.1.
- MSVC 19.44.35228 for x64, from Visual Studio Developer PowerShell.
- Android NDK 30.0.15729638, Clang 21.0.0, arm64-v8a, Android API 33.
- Python 3.12.13 for the benchmark-v6 evaluator.
- FlowForge qualification used the vcpkg x64-windows MLIR package.

## Exact-SHA build and contract matrix

- Windows RelWithDebInfo DEVELOPER: fresh 434-target `all -j 4 -k 0`, second
  build `ninja: no work to do`, CTest 88/88, fresh install, source architecture
  scan, and installed architecture scan.
- Windows Debug DEVELOPER: fresh 438-target `all -j 4 -k 0`, second build
  no-work, and CTest 93/93.
- Windows RelWithDebInfo DEVELOPER with Object and UI contract checks enabled:
  fresh 438-target `all -j 4 -k 0`, second build no-work, and CTest 93/93.
- Android arm64-v8a PLAYER with NDK 30 and API 33: fresh 399-target
  `all -j 4 -k 0`, second build no-work, install, and installed architecture
  scan. The installed closure contains no Lua, FlowForge, MLIR, or LLVM
  artifact.
- Windows RelWithDebInfo TOOLCHAIN with
  `LUX_ENABLE_FLOWFORGE_MLIR=ON`: fresh 278-target `all -j 4 -k 0`, second
  build no-work, and CTest 69/69, including the typed-entry-to-Script-v3
  FlowForge contract test.
- All ten independent installed consumers configured and built against the
  fresh RelWithDebInfo prefix: core-system, core-task, simulation-asset,
  simulation-description, simulation-ecs, simulation-system, snapshot,
  system-hook-script-binding, world, and world-asset. Every second consumer
  build reported no work.
- Public headers from the fresh install were synchronized to the Debug,
  RelWithDebInfo, and Android installation include prefixes before the final
  meta-generation matrix.

## Benchmark-v6 qualification

The RelWithDebInfo benchmark executable embeds the full qualified production
SHA. Each performance case used five warmups and thirty recorded samples. The
evaluator ran with `l0_l1_taskgraph_qualification.toml`, benchmark schema v6,
and `--expected-commit` set to the production SHA above.

Evaluator result:

```text
PASS: 2 scaling, 9 structural rules
```

Selected medians:

| Metric | Size | Median ns | Allocations | Notifications | Callbacks |
| --- | ---: | ---: | ---: | ---: | ---: |
| `reactive_dirty_patch` | 100,000 | 459,600 | 0 | 100,000 | 0 |
| `reactive_dirty_patch` | 1,000,000 | 4,578,400 | 0 | 1,000,000 | 0 |
| `command_buffer_record` | 1,000,000 | 13,146,250 | 0 | 0 | 0 |
| `bound_call_native` | 100,000 | 635,250 | 0 | 0 | 100,000 |
| `hook_multi` | 100,000 | 1,171,650 | 0 | 0 | 400,000 |
| `global_event` | 100,000 | 1,907,750 | 0 | 100,000 | 100,000 |
| `entity_targeted_event` | 100,000 | 2,305,650 | 0 | 100,000 | 100,000 |
| `entity_targeted_event` | 1,000,000 | 41,745,800 | 0 | 1,000,000 | 1,000,000 |

Reactive-dirty scaling exponent was 0.9983 against a 1.25 limit.
Entity-targeted-event scaling exponent was 1.2578 against a 1.30 limit. The
qualified hot paths recorded zero reflection lookups, string lookups, asset
lookups, and scene scans. Callback and notification counts were exact.

The performance gate rejected the earlier intermediate production candidate
`4217f0f42c215f72b79362ff716cb3274d0658a6` at an entity-targeted-event
scaling exponent of 1.3891. The qualified production commit replaces nested
per-entity handler vectors with flat event-major ranges and stores a compact
occurrence frame. The policy was not relaxed.

Raw CSVs remain outside the source repository at
`E:\SyncForder\CodeRepos\build\L1SHB393\evidence\performance`. Their SHA-256
digests are:

```text
08F5D80274DA53252B15B8ADC12447B77CE0D04CC7D6A8CB332C66D4E32DE100  bound-call-native-100k.csv
E465B794A2F9529B5FBA223A060E9DEE024D7D58A1EA2039DBED68D156EDB390  command-buffer-1m.csv
A8A3047762707A42A21AA65B7B70E2368D7599D17A83D1FC4CF6D91DFA7158C3  entity-targeted-event-100k.csv
6D457BEA4BF5E70B6CCB139CD2A053205325C20B9E0D30C95F0DB9A7CB18B623  entity-targeted-event-1m.csv
01ACC3DC994B466A173A5B691CCCF0B7C5C491F5FD79E221C32910682CB78B4F  global-event-100k.csv
E932B8D71385CDC20E39DB77509C425D2EA68B0CD8F7E9F3033CDC6AC040DF15  hook-multi-100k.csv
26EF9A5D31463801C2C0678C87668A99AC7E17307CC1532C0B83D61E217F73E2  reactive-100k.csv
1494299049A81E1B8A38D6CABEE24961141A77B8A1348B033B5DEAC56A1F154E  reactive-1m.csv
```

## Selected artifact digests

```text
875C4AF14DFE0E19634474DB38259743C7C96ABA636F1791F3BCD7EA88919A3D  ecs_l1_benchmark.exe
AFEBAC55870B9513A056E78D9D8189AF3590A456C81DFE6D8BE5D2A6FA085FE3  lux_engine_simulation_description.dll
762DC0DAD1D0C124A6E7FFA9611EE4E7BA660FAD6D808EE6F124DC406F877551  lux_engine_simulation_script_binding.dll
EF9BEF69A978B26430D91C9079C58EBA01B4C797F290B312EBFD71FDE11DFC93  lux_engine_simulation_script_binding_native.dll
F41842E125AF697AB9AE2C10CF7E34DB171D150FDB0C754D4CE4234C2558F498  lux_engine_simulation_script_binding_lua.dll
3DE96FDFFF49230561C59241F08BD9BC0E8E71040C3E5B220489A50B0296CFA0  liblux_engine_simulation_script_binding.so
667AF2E4DE11FC544A2F90053EA439801DF1CE2C256B314750852E92C7B14B02  liblux_engine_simulation_script_binding_native.so
1B812AA2BD37B56EB073541D5757B685AB08ABD5F928B2A2970641D266998163  lux_engine_authoring_flowforge.dll
19F0E86347097A161994E5F348E9A115BBCBDB4EF4A22CCABD9926C9386BD24D  lux_engine_toolchain_flowforge.dll
```

## Status boundary

```text
L1 SystemHook contract                 Freeze Candidate
L1 Script Description and assets      Freeze Candidate
L1 Script binding/runtime backends    Freeze Candidate
LXSD v3                               Freeze Candidate
FlowForge minimal bridge              Qualified for TOOLCHAIN
Performance policy v6                 Qualified
Public API freeze                     Independent Acceptance Required
L2 Process                            Blocked pending L1 acceptance
```

This evidence establishes an exact-SHA freeze candidate. It does not replace
the required independent API review and does not mark L1 formally FROZEN.
