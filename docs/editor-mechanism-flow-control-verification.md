# Flow control domain qualification

Implementation: `81861c236f11e30c8b58bce9dd358d4c6f8f2268`.
lux-cxx: `0a0e7419fc7229df6e372cd35a540249f92250ef`.
Status: **PARTIAL_OPEN_NESTED_BRANCH**. This record qualifies a responsibility migration;
it does not close MA06, MA08 or the overall migration.

## Responsibility and deletion

Flow now owns direct execution reachability and the existing ordered branch-merge rule.
The compiler consumes owned NodeId results through `reachableExecution` and `findBranchMerge`.
The original Toolchain `reachableFromPin` and `findPostDom` declarations and bodies are deleted.
Live graph queries and the immutable suspension-analysis projection share one private iterative
direct-edge walk. The projection remains a disposable analysis result, not another editable topology.

The merge rule remains the first common reachable node in true-leg-first breadth-first order,
excluding the branch itself. It is explicitly not a general post-dominance proof. Queries neither
traverse callees nor retain node/pin pointers. Editing the graph invalidates their analysis facts.
Actual control-region construction, SSA tokens and backend lowering remain in the compiler.
No new JIT catalog requirement, authoring state, executor or manager was introduced.

## Actual pre-change failure

The real installed SDK at `c72fe184c3c40746fac3f6c0989b0cdca83d18a1` compiled seven control cases.
Five legal cases succeeded: diamond merge, separate returns, for-loop branch with break/fallthrough,
while-loop break, and dynamic sequence with branch. A legal nested branch failed with
`AOT_CODEGEN_FAILED`, node/pin zero, `compile failed: exec token not materialised`.
An explicit break outside a loop was correctly rejected.

The first probe aborted before flushing stdout; its source and output are preserved. The corrected
probe writes every result and returns 42 for the unsupported legal nested case. Both the old and
current SDK return 42 with identical complete diagnostics. **Q-MA06-FLOW-NESTED-BRANCH remains OPEN**.
The CTest compatibility check retains those errors; its PASS is not qualification of nested branches.

Five successful objects contain 4810 bytes (4850 including size framing), identical across SDKs:
SHA256 `4d7c46ee692c26f35c5fccebad81f835a8abe5e8d78d4e82e1a4d8f628e21b6d`.

## Fixed-SHA verification

ValidateTrackedSnapshot passed in an independent clean tracked checkout. Editor and PLAYER build
trees were reused, so this is incremental qualification, not a cold build. All builds use
`--target all -j 4 -- -k 0`; second builds report no work. The SDK prefix is new.

| Actual run | Result |
|---|---|
| Editor | 156/156, original 154 names retained |
| PLAYER | 88/88, original 87 names retained |
| Installed pure Flow analysis/control | 2/2 and standalone public headers |
| Installed native/DLL/compiler/structure/analysis | 6/6 and standalone headers |
| Installed scalar/control compiler compatibility | 2/2; nested-branch probe separately FAIL/42 |
| Installed shared graph/domain/compiler | 4/4 |
| Installed payload/DLL | 2/2 and standalone header |
| Installed Ability metadata/DLL | 3/3 and standalone headers |

New graph assertions cover disjoint legs, ordered ties, invalid starts, source mutation, result
ownership after deletion, cycles and a 2048-node iterative chain. Existing assertions remain.
Twelve full semantic diagnostics match the previous real SDK through both domain analysis and
compiler. The scalar fixture still generates 123 exports / 28393 bytes with SHA256
`dbca5b42fd333d757779b33a84f365a8a0141fda677cdf35169cf56fc3b2516c`.
Three asynchronous AOT cases still generate framed 4931 bytes with SHA256
`d4630ff923f0d6a37c9178c5a16b41708aa1c39b2c06d8e7a45c28ef743d0463`.
These are actual code-generation results, not execution of generated scripts. Prior outputs are
inherited at their recorded SHAs and compared, not claimed as rerun.

CMake provider/include/link checks keep the pure domain free of Toolchain, Engine, Editor and UI.
Installed consumers use no source-private headers or build DLLs. The changed public header matches
the SDK and Debug/RelWithDebInfo/Android include prefixes; Android was not built.

## Evidence and remaining scope

Archive: `E:/SyncForder/CodeRepos/archives/lux-engine/EditorFramework-v2/mechanism-ma06/flow-control/evidence`.
1180 files / 53 command records; manifest SHA256:
`0a5b3ee4f6e2980c4f06107771cf21fae76dd4114e347632fe4c6cfa6152a35b`.
Relocation to a Chinese/space path passed. Missing or altered required SDK logs were rejected;
restoring the original bytes passed. Verification uses relative archive paths and fixed Git objects.

Six protected working files retain their exact hashes; ProjectBuilder's archived patch remains
unapplied. Main and historical snapshots were not modified. LR08 PARTIAL/Linux unmet, deferred
native input, IME and original sanitizer scope remain unchanged. Host minimize, skinned WAR and
VERTEX_COLOR findings remain OPEN. No new explicit desktop/GPU/input qualification is claimed.

Flow's registered plain payload stores, canonical node definitions/codecs, real extensible compile
callbacks, removal of Node graph membership and generic graph UI remain unfinished. This domain
algorithm extraction is not a substitute for those final authority and extension gates.
