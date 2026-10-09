# MA08 Material pin direction authority

Implementation `1eadff3024d3691453bb51cd8def6dea183ce52a`. This receipt changes no production code.
Workspace `E:/SyncForder/CodeRepos/lux-engine`, branch `codex/editor-framework-v2`.
Dependency/lux-cxx: `0a0e7419fc7229df6e372cd35a540249f92250ef`.

## Actual correction

A real installed SDK at `cfcfa97b2a69b0328204e21f6508acfbdc662a05` accepted a graph,
then allowed its output DataPin.direction to become INPUT while GraphTopology still held
OUTPUT. Source encoding rejected that inconsistent graph. The exact reproducer returned 42:
`topology OUTPUT=1; payload INPUT=1; source rejected=1`. Its source and run output are retained.

Removed DataPin.direction, material::EPinDirection and the graphDirection conversion helper.
Node constructors no longer store a second direction. Graph insertion/restoration, source
encoding and compilation use graph::EPinDirection from structural records. Registration pin
declarations still express construction intent; they are not a second live topology.
The same illegal assignment is rejected by the new installed public headers (C2039).

Actual consumers migrated: Node.cpp (all builtin constructors), MaterialGraph.cpp (ordinary
and transactional registration), MaterialSource.cpp (equality, draft validation, source
validation and encoding), and MaterialValidation.cpp (real compiler boundary). No forwarding
alias or replacement field remains. The modified DataPin public header is synchronized to
Debug, RelWithDebInfo and Android include prefixes; Android was not built.

The existing identity regression now asserts the field is absent. It also attempts to restore
a reversed output while retaining its original link: GraphTopology rejects DIRECTION_MISMATCH
without adopting the candidate. With that link removed, a deliberately malformed topology is
still rejected by the source codec as INVALID_TOPOLOGY and the real lowering as INVALID_GRAPH
with the original NodeId. Original codec, clone, source bytes, identity high-water, atomic
rejection and undo/restore assertions remain. These checks replace only an impossible second
payload-direction comparison; they do not remove structural boundary validation.

## Executed qualification

ValidateTrackedSnapshot and independent clean source at the implementation SHA passed.
Editor/PLAYER build directories were reused and reconfigured: incremental qualification,
not a cold-build claim. Every final second build reports no work.

| Check | Result |
| --- | --- |
| Full Editor CTest | 149/149 PASS |
| Full PLAYER CTest | 83/83 PASS |
| Fresh installed Material domain | 5/5 PASS |
| Fresh installed actual Material compiler | 6/6 PASS |
| Shared graph / Material and Flow compilers | 4/4 PASS |
| Material registered payload / real DLL | 2/2 PASS |
| Standalone C++20 public headers | 9 domain, 3 node headers PASS |
| Illegal old direction assignment | New SDK C2039 rejection |
| Protected user differences | Six hashes unchanged |

The archived comparison fixture ran unchanged against this SDK. Ten builtin source encodings
and the 50 successful graphs / 100 actual SPIR-V passes match the previous SDK byte for byte.
VERTEX_COLOR still produces the identical full failure. Comparison hashes:

- `material.toml`: 4784 bytes, SHA256 `9b58a7b0b65ac5204113e0b40f1e385091c7cc0d1bcd966b35144cc56758c58b`.
- `material.spirv.bin`: 2892432 bytes, SHA256 `32ebe6844c1a8c2f8fe964fe1f2df117d049a02f8aec8b04f82c6e312087ebe0`.
- `material.spirv.bin.failure`: 1092 bytes, SHA256 `a8268eb6ec667112db09dc2e62d6bd5a1e9e0a44b2da44ae18ac0b5d686a99ce`.

This result preserves, and does not close, the newly discovered VERTEX_COLOR backend failure.
Its original old/new failing test outputs are archived. Outcome parity is not feature support.
No shader stage, mesh pipeline or renderer was changed by this correction.

The CMake File API / compile / link closure still has no Engine, Editor, UI, Toolchain or
legacy provider beneath material_graph. Installed tests use the fresh prefix, not source
private headers or build DLLs. No original test names were removed and no new executable was
added merely to increase the count. Translation units: 700, 641, 14, 15, 4, 6.

## Evidence and unfinished work

Archive: `E:/SyncForder/CodeRepos/archives/lux-engine/EditorFramework-v2/mechanism-ma08/material-pin-direction/evidence`.
1148 files / 41 commands, manifest SHA256 `b0c524de216b2f8be9b859732a3a89c2a47268292bfacd10cb41fd24270baf47`.
Archive-relative verification passes after relocation to a Chinese/space path. Removing or
altering the actual SDK log is rejected; restoring it passes.

This is **PASS for pin direction authority only**. PinId payload storage, registered graph
storage, canonical codec identity/version, old polymorphic Node removal and FlowNodeCatalog
remain unfinished. MA06/MA08 and the overall goal are not complete. No graph UI rendering or
final O(1) PinId lookup qualification is claimed.

Q-LR03-HOST-MINIMIZE remains OPEN: the original desktop SDK 14/15 failure at
`617403987b91940b4851b2d8007755a96e492494` is inherited, not rerun or reclassified. Its common
cause with earlier failures remains unproven. LR08 remains PARTIAL; Linux NOT_RUN/unmet,
native input NOT_RUN_USER_DEFERRED, IME untested, historical sanitizer and skinned WAR retain
their original SHA scopes. Main, six user differences and the external unapplied
ProjectBuilder patch are preserved.
