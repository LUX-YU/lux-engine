# L1 Script / Event / FlowForge Freeze Closure Evidence

## Status

- Verified production SHA: `636dbb77a45ef749096514f7046b99e16c1534c9`.
- Baseline evidence SHA: `6c26bb03dfb478a2f4703a83a619fb4a5577aa42`.
- Rejected baseline production SHA: `777576327bfc9a6323522cf777b8f3fac26871aa`.
- Normative closure document SHA-256:
  `1C9DC27472084E453D890B2B237ADCAC529B985267086E39981E3222361BEF22`.
- Result: new Freeze Candidate. L1 remains NO-GO and Formal L2 remains BLOCKED until independent API and
  semantic acceptance succeeds.

This evidence commit contains no production or test changes and has the verified production SHA as its only
parent.

## Toolchain

- MSVC `19.44.35228` x64, initialized through `VsDevCmd.bat`.
- CMake `4.1.2`.
- Ninja `1.11.1`.
- Python `3.10.13`.
- vcpkg x64-windows MLIR/LLVM `18.1` in the TOOLCHAIN profile.

All builds used Ninja with `-j 4 -k 0`. Android configure/build/test was intentionally not executed under the
repository validation policy.

## Build and test matrix

| Configuration | Result |
| --- | --- |
| RelWithDebInfo DEVELOPER | full `all` PASS; second build no-work; CTest 94/94 PASS |
| Debug DEVELOPER | full `all` PASS; second build no-work; CTest 81/81 PASS |
| RelWithDebInfo PLAYER + Object/UI contract checks | full `all` PASS; second build no-work; CTest 99/99 PASS |
| Desktop PLAYER closure | full `all` PASS; second build no-work; zero FlowForge compiler, NodeGraph, MLIR, or LLVM targets/binaries |
| RelWithDebInfo EDITOR | full `all` PASS; second build no-work; CTest 81/81 PASS |
| RelWithDebInfo TOOLCHAIN | full MLIR/LLVM compiler PASS; second build no-work; CTest 79/79 PASS |

The TOOLCHAIN suite includes `flowforge_script_artifact_test`, which compiles a `FlowGraph` through MLIR/LLVM,
links a native module into `ScriptArtifact::payload`, loads it from memory, resolves the authored
`ScriptSymbolId`, and invokes it.

## Installation and consumers

- A clean DEVELOPER build/install prefix passed `cmake/ValidateInstalledArchitecture.cmake`.
- The exact-SHA EDITOR and TOOLCHAIN products were overlaid into the consumer prefix after the product-surface
  architecture scan.
- All 14 installed consumers configured and built from the installed packages; every second build reported
  `ninja: no work to do`:
  `core-system`, `core-task`, `flowforge-compiler`, `flowforge-model`, `node-graph-editor`,
  `script-binding-authoring`, `simulation-asset`, `simulation-description`, `simulation-ecs`,
  `simulation-system`, `snapshot`, `system-hook-script-binding`, `world`, and `world-asset`.
- Source architecture validation ran as part of every full build and passed.

## Benchmark v12

Command shape:

```text
ecs_l1_benchmark.exe --group <group> --mode performance --size <size> --output <csv>
python evaluate_l1_benchmark_v12.py <all csv> --policy l1_benchmark_v12_policy.json
```

Each size used five warmups and thirty samples. The final evaluator result was:

```text
PASS: 7 scaling, 1 ratio, 3390 structural checks
```

| Group | Sizes | Median ns | Exponent |
| --- | ---: | ---: | ---: |
| reactive-dirty | 100K / 1M | 400900 / 3887450 | 0.987 |
| entity-targeted-event-sparse | 1M / 2M total entities, 10K scripted | 331750 / 373000 | 0.169 |
| entity-targeted-event-dense | 500K / 1M handlers | 590800 / 1173350 | 0.990 |
| script-prepare-scaling | 1K / 10K | 204300 / 1898550 | 0.968 |
| lua-prepared-call-pool | 10K / 20K instances, eight calls each | 1413450 / 2771350 | 0.971 |
| cpp-static-object-slab | 100K / 1M instances | 4766150 / 47132450 | 0.995 |
| script-artifact-export-scaling | 10K / 100K exports | 538100 / 6211900 | 1.062 |

The sparse 2M/1M median ratio was `1.124`, below the `1.20` gate. Retained v11 groups also ran with their
canonical sizes, including 1M prepared calls, 1M Hook handlers, 1M detach bindings, 100K worker-produced events,
and 100K snapshot entities. Structural checks confirmed exact callback/work counts and zero measured Lux
allocation on the gated hot paths.

## Artifact hashes

```text
A9AAD990C77F2683D2C8FB59E19C0176A3BC3B85AB700B4885F32F2F94284509  ecs_l1_benchmark.exe
C5F99B0638A5588320747CCDBED499E1DEB99644A1731C56380EDB8306016B65  lux_engine_flowforge_compiler.dll
C969041236998A1FC98E2ADB9C071525A2DFBA76D5194E6B8B4DCD0C822D8628  flowforge_script_artifact_test.exe
A98A6A25AE57044134B3429AD0433BDC1B44EABA406DB3E52403B68324D195B2  installed Compiler.hpp
```
