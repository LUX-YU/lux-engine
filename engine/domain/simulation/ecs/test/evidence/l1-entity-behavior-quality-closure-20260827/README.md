# L1 EntityBehavior and Method Binding freeze-candidate qualification

This record qualifies production and test commit
`4d9a9069c09c132c9b6274187ff79259c2696ad0`, based on reviewed branch head
`2bf05234c920b1a86a195e97ec68fc50be1cb5a6`. The evidence-only commit that
adds this file has the production commit as its sole parent and contains no
production or test changes.

The previously qualified production commit
`393180e9a67e01c9e6863b4d0bde4a8849b05b0c` remains rejected by
`independent-review-rejection.md`. This replacement qualification is only a
Freeze Candidate. L1 remains NO-GO and formal L2 remains BLOCKED until an
independent API and semantic acceptance succeeds.

## Contract source and workspace isolation

The replacement contract is
`doc/l1-entity-behavior-method-binding-quality-closure-refactor-spec.zh-CN.md`.
The downloaded LF source and the canonical Git blob both have SHA-256:

```text
843118EA52385E71BF3347F07D94D58A63D6B5B3DD95590A3EEE95950D05DB28
```

On a Windows checkout with automatic LF-to-CRLF expansion, the physical
working-tree copy has SHA-256
`C025B80AB42CBBBDAC2D181ED0555FC9ED974534CBA217EB05092CBEDB2E2149`.
The files have identical content and 3,194 logical lines; the canonical Git
blob is byte-identical to the supplied LF source.

Implementation and qualification used separate clean linked worktrees. The
original `codex/object-ui-foundation` worktree retained all 69 user-owned
status entries and was not reset, staged, or modified by this implementation.
The exact-SHA matrix ran from a clean detached worktree at the production SHA.

## Toolchain

- Date: 2026-08-27, Europe/London.
- CMake 4.1.2; Ninja 1.11.1.
- MSVC 19.44.35228.0 for x64, from Visual Studio Developer PowerShell.
- Android NDK 30.0.15729638, Clang 21.0.0, arm64-v8a, Android API 33.
- Python 3.13.0 for the benchmark evaluator and Tier-1 static importer tests.
- FlowForge qualification used the vcpkg x64-windows MLIR package.

## Exact-SHA build and contract matrix

- Windows RelWithDebInfo DEVELOPER: fresh 439-target `all -j 4 -k 0`, second
  build `ninja: no work to do`, CTest 89/89, fresh install, source architecture
  scan, and installed architecture scan.
- Windows Debug DEVELOPER: fresh 443-target `all -j 4 -k 0`, second build
  no-work, and CTest 94/94.
- Windows RelWithDebInfo DEVELOPER with `LUX_OBJECT_CONTRACT_CHECKS=ON` and
  `LUX_UI_CONTRACT_CHECKS=ON`: fresh 443-target `all -j 4 -k 0`, second build
  no-work, and CTest 94/94.
- Windows RelWithDebInfo TOOLCHAIN with
  `LUX_ENABLE_FLOWFORGE_MLIR=ON`: fresh 293-target `all -j 4 -k 0`, second
  build no-work, and CTest 73/73. This included FlowForge, the Lua static
  importer, the Python Tier-1 `ast.parse` importer, and canonical LXSA-v2
  re-encoding tests.
- Android arm64-v8a PLAYER with NDK 30 and API 33: fresh 404-target
  `all -j 4 -k 0`, second build no-work, fresh install, and installed
  architecture scan. Token-boundary inspection found no Lua, Python runtime,
  FlowForge, MLIR, or LLVM artifact in the Player closure.
- All ten independent installed consumers configured, built, and ran against
  the fresh RelWithDebInfo prefix: core-system, core-task, simulation-asset,
  simulation-description, simulation-ecs, simulation-system, snapshot,
  system-hook-script-binding, world, and world-asset. Every second consumer
  build reported no work.
- The deep SystemHook/ScriptBinding consumer exercised the fresh-installed
  public path `LUX_METHOD -> RefMethod -> C++ bridge -> Script v4 -> LXSA v2
  -> LXSD v4 -> ScriptComponent -> EntityBehavior instance -> lifecycle ->
  MULTI Hook/Event invoke`. It verified exact `self`, one export bound to
  multiple targets, and one prepared method reused by those targets.

## Corrected exact-SHA boundary

The first benchmark candidate, `ad38e445ead825967abba0f0b268e055c414b26e`,
was rejected before qualification because the real 2,000,000-entity sparse
case exposed EnTT's default 20-bit entity-index capacity and terminated with a
Windows access violation. The production correction uses a 64-bit `Entity`,
which gives the EnTT entity traits a 32-bit index and 32-bit generation, and
adds compile-time coverage for at least 2,000,000 live identities. The policy,
physical entity count, 10,000 scripted instances, and 100,000 occurrences were
not reduced or simulated.

All matrix and benchmark results below were rerun from the corrected exact SHA
`4d9a9069c09c132c9b6274187ff79259c2696ad0`.

## Benchmark-v7 qualification

The RelWithDebInfo benchmark executable embeds the full production SHA. Every
performance case used five warmups and thirty retained samples. Evaluation used
`l0_l1_taskgraph_qualification.toml`, schema v7, and
`evaluate_l1_qualification.py --expected-commit` set to the production SHA.

Evaluator result:

```text
PASS: 2 scaling, 1 ratio, 14 structural rules
```

Selected medians:

| Metric | Size | Median ns | Allocations | Notifications | Callbacks | Entities examined |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| `reactive_dirty_patch` | 100,000 | 462,750 | 0 | 100,000 | 0 | 0 |
| `reactive_dirty_patch` | 1,000,000 | 4,805,950 | 0 | 1,000,000 | 0 | 0 |
| `command_buffer_record` | 1,000,000 | 13,800,200 | 0 | 0 | 0 | 0 |
| `cpp_method_prepared` | 100,000 | 528,900 | 0 | 0 | 100,000 | 0 |
| `hook_global_multi` | 100,000 | 1,314,500 | 0 | 0 | 400,000 | 0 |
| `hook_entity_multi` | 100,000 | 704,450 | 0 | 0 | 100,000 | 100,000 |
| `global_event` | 100,000 | 452,800 | 0 | 100,000 | 100,000 | 0 |
| `entity_targeted_event_sparse` | 100,000 total | 1,087,550 | 0 | 100,000 | 100,000 | 100,000 |
| `entity_targeted_event_sparse` | 1,000,000 total | 1,075,900 | 0 | 100,000 | 100,000 | 100,000 |
| `entity_targeted_event_sparse` | 2,000,000 total | 1,050,400 | 0 | 100,000 | 100,000 | 100,000 |
| `owned_worker_event_buffer` | 1,000,000 | 5,352,200 | 0 | 1,000,000 | 1,000,000 | 0 |

Reactive-dirty scaling exponent was 1.016433 against a 1.25 limit. Sparse
targeted-dispatch scaling exponent was -0.004677 against a 1.30 limit. With
the same 10,000 scripted entities and 100,000 occurrences, the 2M/1M total
entity median ratio was 0.976299 against a 1.20 limit.

All qualified hot paths recorded zero Lux-side update allocations, reflection
lookups, string lookups, asset lookups, scene scans, and signature adaptations.
Sparse targeted dispatch recorded `entities_examined == occurrences == 100000`
at every total-entity size. Callback, notification, instance-create,
method-prepare, and frame-build counters matched their declared cases.

Raw CSVs remain outside the source repository under the exact-SHA build tree.
Their SHA-256 digests are:

```text
0BCF1530B6D9903E1BC3EE111F599ECB67241F629B963719402EFF325978BA8C  command-buffer-1000000.csv
DD4B082A01EC2E47095EC5402E4F9DF1C2B7BCF92354F533C2488C7E5264FBD7  cpp-method-prepared-100000.csv
6D000D4B38EACAC8E713069DCBAFB280402C9E1F9407EE1F9D67830F7AB62B62  entity-targeted-event-sparse-100000.csv
E215335575E85331132BAD56A155CB82E8B757EBBA4F41275E663A376BDDD62F  entity-targeted-event-sparse-1000000.csv
6371510C1229D3B0FFFA5227C033D686904719C3E04A1FD19B3D9FCB31CABFB9  entity-targeted-event-sparse-2000000.csv
769E4D151125129BF630F028A7A828BDA571079428A5050FF7A9580F28579E05  global-event-100000.csv
A500001841FFA67E21661DEC84C5C3D1A1F84292115D20F01B2E3377446972F4  hook-entity-multi-100000.csv
E62226B73256C980E17CAA5E249DFDDF560FC87312F3D28D656667F2346DF0F8  hook-global-multi-100000.csv
30FC6570414D9D9A51CEC9CA2AA61630E91C11A1C746658F9CE08092C4A46BA4  owned-worker-event-buffer-1000000.csv
A8373D1B357CB4C99DAB4C7B2B3C2F9FCBF714D23DF912656F83A5CD80DD3591  reactive-dirty-100000.csv
971821E3A794FF5601E358C3F94E9E2A8E55569BCBA31FCFFA7BA2A225177D52  reactive-dirty-1000000.csv
```

## Selected artifact digests

```text
36941F37F6D677303C7ED3AE1840A2249124AB632F5C4118AC17CC2D9A145CFD  ecs_l1_benchmark.exe
A7EAAB80B77A9D8D8A2E05449CB71D15EDC4846E04848C3A17D0BA35ACAA76E7  lux_engine_simulation_description.dll
5A04A9783ECACD0EB7C850F26D6EE36EB980FAD167B3C26FF1A49AC579C51285  lux_engine_simulation_script_binding.dll
1D0F7AA8744D36A8AFDBBBECC85AB7004D8C3636A178C1CDC0007BD30291C424  lux_engine_simulation_script_binding_cpp_static.dll
93DEA4B7AF442A8F2BA5BC2EFF177C41025345F37831F44BE5EE692A63680623  lux_engine_simulation_script_binding_native.dll
D946B5DA8E2B4626138F4D95F8B482209E90CAD2579E6F95D6F46C1EF98C2F1B  lux_engine_simulation_script_binding_lua.dll
0615581BF21E12921C1F4D1F181CC023BC00015DB93BB3FD9B394227387E7E89  liblux_engine_simulation_script_binding.so
7A223AE7444E1422A046F72D7E0BECA8159572604F91CA0F3159CDDF1828587E  liblux_engine_simulation_script_binding_native.so
```

## Status boundary

```text
L1 EntityBehavior contract              Freeze Candidate
L1 explicit Method Binding contract      Freeze Candidate
L1 owned Event dispatch contract         Freeze Candidate
L1 exact-SHA build/performance matrix    Qualified
Independent API/semantic acceptance      Required
L1 formal freeze                         NO-GO pending acceptance
Formal L2                                BLOCKED
```

This exact-SHA evidence does not itself declare L1 FROZEN and does not
authorize formal L2 implementation.
