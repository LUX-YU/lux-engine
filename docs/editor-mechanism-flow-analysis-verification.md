# Flow module semantic analysis

Implementation: `c799ddeb364e0b22dce8cb5b265d8fcaaefe21eb`. MA06 domain-policy migration; MA06/MA08 remain incomplete.

## Responsibility and deletion

The Flow module now owns Ability/Event requirement derivation, graph-function suspension
propagation, borrowed-step crossing checks and synchronous lifecycle rules. Toolchain invokes
`FlowAnalysis::create` before lowering and uses the same result to verify generated async markers.
The original `Compiler.cpp` policy bodies, private `SuspensionAnalysis.hpp/.cpp` and unused private
three-argument AOT entry are deleted. No compatibility forwarding implementation remains.

FlowAnalysis owns copied requirements and an immutable execution-reachability projection keyed
by the analyzed graph's stable IDs. It retains no Node/Pin pointer, graph/catalog borrow, reflection
operation or plugin callback. Queries remain valid after source destruction. This is a disposable
compilation result, not another authoring graph, topology authority, executor or live cache; lowering
must use the same unchanged input. An edited graph requires fresh analysis.

The original recursive fixed-point and path-sensitive traversal algorithms are retained, using IDs
instead of pointers. The lowest suspension identity wins deterministically. Foreign function definitions
are checked by membership before projecting their IDs, so equal local IDs from another graph do not
become a valid callee. There is no new library or dependency on Process, Editor, LLVM or a plugin manager.

## Actual behavior qualification

A real installed SDK at `bcbcf722c98334ecb1cbddd49b56c3f183aef90a` ran the same diagnostic fixture.
Twelve complete error records (code, node, pin and message) match both the new pure module and the actual
new compiler: unknown contract/method, schema/conflicting requirements, impostor Ability, Event source
and schema, direct/transitive/recursive lifecycle suspension, borrowed-step crossing, and foreign callee.
These are actual SDK executions, not source inspection or a declaration-only probe.

New regression also checks successful synchronous analysis, Event requirement ownership, recursive
witness ordering, unreachable/invalid starts and targets, removing the suspension from a borrowed-value
path, and querying after the graph/catalog have been destroyed. Existing tests and assertions remain.

Three additional real old/new SDK AOT cases generated identical objects: direct asynchronous Ability,
a graph-function call to an asynchronous Ability, and Script Event wait. Their framed comparison file
is 4931 bytes with SHA256 `d4630ff923f0d6a37c9178c5a16b41708aa1c39b2c06d8e7a45c28ef743d0463`.
This proves code-generation parity; generated native scripts were not executed in this fixture.

The existing 123 observable scalar exports generated 28393 object bytes, identical to
the archived scalar baseline at `f3f1d4b4d0d4bb1fa5d9673317aee864443139e1`:
`dbca5b42fd333d757779b33a84f365a8a0141fda677cdf35169cf56fc3b2516c`. That old scalar result was inherited and compared, not rerun here.

## Build and installation

ValidateTrackedSnapshot passed for the implementation SHA. Independent clean tracked source was used
with reused Editor/PLAYER build trees: this is incremental qualification, not a fresh cold-build claim.
Full `--target all -j 4 -- -k 0` and second no-work builds passed. Build and GPU runs were serial.

| Actual run | Result |
|---|---|
| Editor CTest | 154/154; previous names retained, two new analysis cases |
| PLAYER CTest | 87/87; previous names retained, pure analysis added |
| Pure installed Flow analysis, without compiler package | 1/1 and standalone public header |
| Installed native/DLL/compiler/structure/analysis | 6/6 |
| Installed scalar AOT | 1/1 |
| Installed shared graph/domain/compiler | 4/4 |
| Installed payload/DLL | 2/2 |
| Installed Ability metadata/DLL | 3/3 |

Actual File API provider closure and compile/link inputs confirm the pure Flow module/consumer have
no upward Toolchain/Engine/Editor/UI dependency. Fresh installation and standalone headers compile
without private/source includes or build-DLL fallback. The new public header is synchronized to Debug,
RelWithDebInfo and Android include prefixes; Android was not built. The earlier eight operation/API
compile negatives retain their original evidence; they were not represented as new runs.

The initial SDK fixture mistakenly used const Pin references for a mutable link call and failed to
compile; its source and failure log are retained, followed by the corrected execution. Archive preparation
also initially rejected a CMake LLVM compiler-identification filename because the probe scanned the
entire Ninja text. The original script and matched tokens are retained. The corrected probe checks
actual File API link libraries and loaded package entries, without accepting a forbidden dependency.
Production qualification passed; neither probe failure was erased or relabeled as a production failure.

## Evidence and limits

Evidence: `E:/SyncForder/CodeRepos/archives/lux-engine/EditorFramework-v2/mechanism-ma06/flow-analysis/evidence`.
1172 files / 53 commands; manifest SHA256
`2f4f2d6ac62ab223fe65c4bdd0ccf843d317134d0654ad8ea9f680b7d4b53185`. Relocation to a Chinese/space path passed. Removing and tampering with
an actual SDK log were rejected; restoring the original bytes passed.

Six protected user differences are unchanged; the separately archived ProjectBuilder patch remains
unapplied. Main and historical snapshots were not modified. LR08 PARTIAL/Linux unmet, deferred native
input, IME and prior sanitizer scope remain unchanged. The installed-host minimize failure, skinned WAR
and VERTEX_COLOR compilation failure remain OPEN under their previous evidence and responsibility.

This does not complete FlowNodeCatalog, registered node/pin payload stores, compiler callbacks, canonical
source codecs, old Node/Pin structural-field removal or graph UI qualification. Those remaining MA06/MA08
requirements and later MA phases still need implementation. No overall completion or freeze is claimed.
