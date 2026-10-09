# Flow nested control correction

Implementation: `d5fe58b83bb8c07f797c54fa163a47d6e3e3d3d3`. lux-cxx: `0a0e7419fc7229df6e372cd35a540249f92250ef`.
**Q-MA06-FLOW-NESTED-BRANCH CLOSED for the demonstrated merge-boundary defect.**
MA06, MA08 and the overall migration remain incomplete. This does not redefine arbitrary graph
post-dominance or qualify every possible control topology.

## Failure and responsibility

The preceding control-domain migration preserved the real SDK's nested-branch failure and reported
PARTIAL. Those snapshots and failed results remain unchanged. The original seven-case AOT probe
rejected a legal nested branch with `exec token not materialised`.

A second actual installed-SDK probe at `81861c236f11e30c8b58bce9dd358d4c6f8f2268` builds branch trees
of depth one, two and three, with both forward and reverse insertion of merge predecessors.
Depth one links and executes eight input combinations in each ordering. Depth two and three fail
MLIR verification with `operand #0 does not dominate this use`; the probe returns 42.
The first fixture mistakenly queried export symbol 1 instead of authored symbol 41 and aborted;
its original source and output are retained. The corrected fixture ran before the production edit.

The inner region had reached a merge owned by an enclosing region but tried to select one of its
predecessors before returning. That predecessor could be absent or belong to a sibling SSA region.
The original compiler now checks its existing external frontier first and returns the branch's
outer ordering token. The owning chain gathers merge predecessors after its regions have returned.
The existing Branch-to-CF lowering already preserves the actual condition and region control flow;
tokens are ordering artifacts, not condition/data values.

Production behavior changes in one eight-line block of `IR.cpp`. No graph, executor, Runtime,
publication, catalog or storage algorithm was added or replaced. Original failed legal compilation
is now required to succeed; the invalid break-outside-loop assertion and full error remain.

## Actual execution and regression

The new public-API regression compiles a real FlowGraph, links the emitted object, loads the
artifact through NativeModule and invokes its native ABI. Every leaf writes a distinct graph
variable value. It checks all eight boolean input combinations for depths one/two/three and both
predecessor orders: **48 native invocations with exact result checks**. This is execution evidence,
not only object generation or a replacement interpreter.

ValidateTrackedSnapshot passed in an independent clean tracked checkout. Reused Editor/PLAYER
build trees make this incremental qualification, not a cold build. Full builds use
`--target all -j 4 -- -k 0`; second builds report no work. SDK installation uses a new prefix.

| Actual run | Result |
|---|---|
| Editor | 157/157, preceding 156 names retained |
| PLAYER | 88/88, preceding names retained |
| Installed pure Flow analysis/control | 2/2 and standalone headers |
| Installed native/DLL/compiler/structure/analysis | 6/6 and standalone headers |
| Installed scalar/control/nested execution | 3/3, plus explicit 48-call execution probe |
| Installed shared graph/domain/compiler | 4/4 |
| Installed payload/DLL | 2/2 and standalone header |
| Installed Ability metadata/DLL | 3/3 and standalone headers |

The original control AOT probe now compiles all six legal cases. The five previously successful
object payloads are byte-identical; the invalid seventh case still reports its exact error.
Twelve existing full semantic diagnostics remain byte-identical through module and compiler.
Scalar AOT retains 123 exports / 28393 bytes; three asynchronous AOT cases retain framed 4931 bytes.
Those scalar/async figures are code-generation parity, not new script-execution claims.

Actual provider/include/link checks retain domain layering. Installed consumers use public SDK
headers/libraries, not source-private headers or build DLLs. No modules public header changed.
Six protected working-file hashes remain exact, and ProjectBuilder's archived patch is unapplied.

## Archive and limits

Evidence: `E:/SyncForder/CodeRepos/archives/lux-engine/EditorFramework-v2/mechanism-ma06/flow-nested/evidence`.
1184 files / 51 command records; manifest SHA256:
`5b7b15263cf8cbecfbcc602fb33fafc116154789b1da980a62109bee31245ff7`.
Relocation to a Chinese/space path passed. Removing or tampering with a required real SDK log
was rejected; restoring it passed. Old failure and object baselines remain attributed to their
original SHAs. Historical receipt files were not edited.

Linux/LR08 PARTIAL, deferred native input, IME and original sanitizer scope remain unchanged.
Host minimize, skinned WAR and VERTEX_COLOR findings remain OPEN. This does not claim new explicit
GPU/desktop/input qualification. Main was not modified.

Flow's actual registered payload stores, canonical definitions/codecs, extensible compile callbacks,
removal of Node graph membership and generic graph UI remain outstanding under MA06/MA08.
