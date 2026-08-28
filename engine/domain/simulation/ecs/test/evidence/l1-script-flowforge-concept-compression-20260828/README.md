# L1 Script / FlowForge Concept Compression Qualification

## Verified revision

- Production commit: `777576327bfc9a6323522cf777b8f3fac26871aa`
- Production tree: `84c94525ea11e9d6d9a9d40fca7a71344734bc33`
- Normative specification SHA-256:
  `FCE5792E30B4B645CF08A18382DB5879AA0AE34CB90CE9C6CCC98DD6020B1D69`
- Branch: `codex/script-flowforge-concept-compression`

This evidence commit is the direct child of the verified production commit and contains no production changes.

## Toolchain

- Visual Studio 2022 Developer PowerShell 17.14.35
- MSVC 19.44.35228.0
- CMake 4.1.2
- Ninja 1.11.1
- Python 3.12.13
- vcpkg triplet `x64-windows`

Android configure, build, CTest, and closure validation were intentionally omitted according to the repository's
current `AGENTS.md` verification policy. The Android install include mirror was kept synchronized for meta generation.

## Build and test matrix

| Configuration | Result |
| --- | --- |
| Developer RelWithDebInfo, full `all` | PASS; 94/94 CTest |
| Developer Debug, full `all` | PASS; 81/81 CTest |
| Hardened Object/UI contracts | PASS; 99/99 CTest |
| Desktop PLAYER closure | PASS; no Node Graph Editor, FlowForge compiler/dialect, MLIR, or LLVM target leakage |
| EDITOR profile | PASS; 97/97 CTest; Node Graph Editor present; Toolchain compiler absent |
| TOOLCHAIN + FlowForge MLIR | PASS; 78/78 CTest |

Every CMake profile build was followed by a second full build that reported `ninja: no work to do`.

## Installation and architecture

- Source architecture validation: PASS.
- Fresh Developer, EDITOR, and TOOLCHAIN installation trees: PASS.
- Installed architecture validation on all three fresh prefixes: PASS.
- Installed consumers: 14/14 configured, built, and executed successfully.
- Every installed consumer's second build reported `ninja: no work to do`.
- Retired ScriptAsset package roots, Toolchain Script root, and empty `world-section` consumer root are absent.
- Simulation hot/safe runtime packages contain no `RuntimeObject` dependency.

## Benchmark v11

The RelWithDebInfo producer embedded the exact production commit above. The formal run used five warmups and 30
samples for every `(group, size)` input and covered all retained benchmark groups, including:

- `reactive-dirty`: 100K and 1M;
- `entity-targeted-event-sparse`: 100K, 1M, and 2M;
- `script-prepare-scaling`: 4,096 and 8,192 mounts;
- one-million-call Hook, Event, prepared C++ method, worker Event buffer, and Script detach cases.

Evaluator result:

```text
PASS: 3 scaling, 1 ratio, 2580 structural checks
```

## Installed artifact hashes

| Artifact | SHA-256 |
| --- | --- |
| `lux_engine_simulation_script.dll` | `6AFF2D9197294CA3B2336F9481C2B8C7F7E20FCDCBB30D54D34B6D9244E01D0D` |
| `lux_engine_function_script_artifact.dll` | `1C85F1D17EECE01B5EEBC55459651FCB1F08DD9A689184D32AE25C424470868E` |
| `lux_engine_flowforge_compiler.dll` | `5A13336F59848A1C7E6C0262CB576833D86135C98F9AB2D003BFD46A1BC4731C` |
| `lux_engine_editor_node_graph.lib` | `BF79E6590761BF39E6724C795BF38D94EC613267157E4A14F67B90A0145B100E` |

## Qualification status

The verified production commit satisfies this implementation plan's build, architecture, installation, consumer, and
benchmark gates. It is ready for review; this record does not merge, push, or independently grant a formal freeze.
