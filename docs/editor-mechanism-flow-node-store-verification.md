# Flow NodeId ownership store qualification

Implementation: `98e24562c37744e9a21c5aaee0b796e0d6280000`.
This closes the Flow node identity/store migration only. MA06, MA08 and the overall migration remain incomplete.

## Responsibility and removed API

GraphTopology remains the only identity issuer. FlowGraph now owns nodes in a map keyed by NodeId;
the old recycled size_t NodeStorage index is gone. Node no longer owns an ID, and its address-derived
and explicit-ID constructors are deleted. Source capture, analysis, compiler lowering, script runtime
and all active callers use the graph key. There are no old-name forwarding APIs.

Extraction returns FlowNodeSnapshot, which owns the detached node and its explicit restoration key.
The same snapshot is used by GraphEdit removal/undo. FlowNodeInsertion distinguishes fresh issuance
from explicit restoration without a second preserve_insert_ids flag. Preparation reserves candidate
storage; commit moves owners and swaps prepared state without allocation or arbitrary callbacks.
The existing shared GraphEdit algorithm and identity high-water rules are retained.

The pointer-to-ID index is derived, non-owning and never issues IDs. Detached/foreign pointers resolve
to an invalid key. Moving a graph rebinds its nodes without moving the actual node objects. Public
enumeration borrows NodeEntry values rather than exposing owning pointers. Node lookup is O(log N),
with no sparse allocation proportional to large IDs. This does not claim O(1) PinId payload lookup.

## Behavior and SDK evidence

Expanded real-graph tests cover extraction/restoration with identical encoded source, same-ID
replacement and retained removed owner, UINT64_MAX-1 and UINT64_MAX, absorbing exhaustion after
removal, moving into a nonempty graph, dynamic pins, and derived-index membership. Existing assertions
and CTest names are retained; no count-based substitution was made.

The installed SDK accepts the real add/extract/restore path. Three separate compile negatives reject
Node::id(), the explicit SequenceNode ID constructor and FlowGraph::getNode(size_t). Restoring the
same fixture to its legal mode builds and runs. These use actual installed public headers and libraries.

Twelve complete semantic diagnostic records match the previously executed SDK baseline at
`bcbcf722c98334ecb1cbddd49b56c3f183aef90a`, both through the pure analysis module and the real compiler.
Three actual asynchronous AOT cases produce the same framed 4931 bytes as
`c799ddeb364e0b22dce8cb5b265d8fcaaefe21eb`:
`d4630ff923f0d6a37c9178c5a16b41708aa1c39b2c06d8e7a45c28ef743d0463`.
This tests code generation, not execution of those generated scripts.

The 123-export scalar fixture generates 28393 identical object bytes compared with the archived
`f3f1d4b4d0d4bb1fa5d9673317aee864443139e1` baseline:
`dbca5b42fd333d757779b33a84f365a8a0141fda677cdf35169cf56fc3b2516c`.
Old baseline results were inherited and compared, not rerun or attributed to the new SHA.

## Build and closure

ValidateTrackedSnapshot passed. Qualification uses independent clean tracked source and reused
Editor/PLAYER build trees; it is incremental qualification, not a cold-build claim. Full all builds
use -j 4 -- -k 0; second builds report no work. The SDK prefix is new.

| Actual run | Result |
|---|---|
| Editor CTest | 154/154; exact previous test-name set retained |
| PLAYER CTest | 87/87; exact previous test-name set retained |
| Pure installed Flow analysis | 1/1 and standalone header |
| Installed native/DLL/compiler/structure/analysis | 6/6 and standalone headers |
| Installed scalar AOT | 1/1 |
| Installed shared graph/domain/compiler | 4/4 |
| Installed payload/DLL | 2/2 and standalone header |
| Installed Ability metadata/DLL | 3/3 and standalone headers |

Actual CMake provider/link inputs establish the pure Flow dependency closure without upward
Toolchain/Engine/Editor/UI dependencies. Installed consumers use neither source-private headers
nor build-DLL fallbacks. Nine changed module public headers match the new SDK and Debug,
RelWithDebInfo and Android include prefixes. Android was not built.

The first development build failed on unmigrated constructors and an overbroad identifier edit;
the actual failed output is preserved. Corrections were followed by full successful builds and
affected tests, then the fixed-SHA matrix above. No production runtime failure is inferred from
those compilation mistakes. Earlier eight compile-negative results retain their original SHA;
they are not reported as newly executed here.

## Archive, protection and remaining work

Evidence directory:
`E:/SyncForder/CodeRepos/archives/lux-engine/EditorFramework-v2/mechanism-ma08/flow-node-store/evidence`.
1185 files / 61 commands; manifest SHA256:
`4966a9372fbc6fdae54333e9e74d522adcadbc53a408cf553bb83088dee0e17e`.
Relocation to a Chinese/space path passed. Removing or tampering with a required actual SDK log
was rejected; restoring the original bytes passed.

All six protected working-file hashes are unchanged. The separately archived ProjectBuilder patch
remains unapplied. Main and historical snapshots were not modified. Linux/LR08 PARTIAL, deferred
native input, IME and previous sanitizer scope remain unchanged. The prior installed-host minimize
failure, skinned WAR and VERTEX_COLOR compiler failure remain OPEN. No new explicit desktop/GPU
or native-input qualification is claimed by this graph-only change.

Pin identity fields, Node graph membership, polymorphic semantic nodes, the real FlowNodeCatalog,
registered compiler callbacks, canonical source codecs, O(1) PinId payload lookup and graph UI
qualification remain to be completed. The node store is a migration closure, not the final registered
Flow representation or an overall freeze.
