# Flow stable function references

Implementation: `1092095e16b9e4c0929a7411bc25bbada4f02049`. lux-cxx: `0a0e7419fc7229df6e372cd35a540249f92250ef`.
**This function-reference closure passes; MA06, MA08 and the full migration remain incomplete.**

## Responsibility and observed failure

Function call/return nodes previously retained `FuncDefNode*`. The ordinary graph extraction
API could remove a definition independently of those users. A real installed-SDK probe at
`d5fe58b83bb8c07f797c54fa163a47d6e3e3d3d3` extracted the definition, restored the same identity,
signature and pin IDs at a different address, then destroyed the old owner. Source capture
failed with code 6 / node 2 (exit 42), although the restored graph contained the intended
function. The probe does not claim that a sanitizer detected a use-after-free. Its first two
compilation attempts used the wrong diagnostic field/access and are preserved as fixture
failures; the corrected probe ran before production modification.

Call/return nodes now store the graph-local definition NodeId. Construction borrows the
definition only to copy its pin schema. Resolution checks the current graph, node kind and
signature. No function-definition address survives construction. Reflection type pointers
retain their existing immutable metadata-environment lifetime contract; this is not a new
metadata owner. GraphTopology remains the identity issuer; graph stores own actual nodes.

Source capture, source reconstruction, FlowAnalysis, Toolchain lowering and GraphEdit admission
use the same semantic reference. Source encoding is unchanged. Batch admission sees definitions
appearing later in the candidate list, validates signatures, retains candidate owners on
rejection and still rejects deleting a referenced definition without its users. Low-level
stores may contain unresolved drafts, which resolution and source capture reject. Explicit
IDs are relative to the receiving graph; matching numbers from two graphs are not a global
identity or an implicit cross-graph binding.

Deleted APIs: the one-argument function-definition constructors, `FuncReturnNode::def()` and
`GraphFuncCallNode::callee()`, plus both stored raw definition pointers. No compatibility shell,
new graph owner, counter, history algorithm or runtime was introduced.

## Actual qualification

ValidateTrackedSnapshot passed in an independent clean tracked checkout at the implementation
SHA above. Editor/PLAYER reused build trees, so this is incremental qualification, not a cold
build. All builds use `--target all -j 4 -- -k 0`; second builds report no work. A fresh SDK
prefix prevents old headers/libraries from supplying the removed APIs.

| Run | Result |
|---|---|
| Editor full CTest | 157/157; exact preceding names retained |
| PLAYER full CTest | 88/88; exact preceding names retained |
| Installed pure Flow analysis/control | 2/2, standalone headers, no compiler dependency |
| Installed native/DLL/compiler/structure/analysis | 6/6 and standalone headers |
| Installed scalar/control/nested native execution | 3/3 |
| Installed graph/domain/compiler | 4/4 |
| Installed payload/DLL | 2/2 |
| Installed Ability metadata/DLL | 3/3 |
| Removed API compilation | Four intended failures; positive and restored-positive execute |
| Same-ID/same-signature rebuild SDK probe | Source capture succeeds, exit 0 |

Expanded production regressions cover detached-definition resolution, restoration at a
different address, destruction of the previous owner, graph move, wrong kind/signature,
source byte equality, preserved maximum identity, late definition in a batch and rejected
batch ownership preservation. The real module/compiler analysis fixture also compiles a
function call before and after rebuilding its definition. These assertions were added to
existing tests; equal test counts are not the acceptance criterion.

Twelve complete semantic diagnostic records remain byte-identical through module and compiler.
The scalar AOT output retains 123 exports / 28393 bytes. Three actual asynchronous AOT cases
retain framed 4931 bytes. Six legal control objects retain 5772 raw bytes, and illegal
break-outside-loop keeps its exact error. These are code-generation comparisons. Separately,
the installed compile/link/NativeModule regression executes all 48 nested-control cases again.

Provider/include/link checks preserve domain layering; installed consumers do not include
source-private headers or use build DLLs. The changed FunctionalNode public header is synchronized
to Debug, RelWithDebInfo and Android include prefixes; Android was not built. All six protected
working-file hashes are unchanged. ProjectBuilder's archived user patch remains unapplied.

## Evidence and limits

Evidence: `E:/SyncForder/CodeRepos/archives/lux-engine/EditorFramework-v2/mechanism-ma08/flow-reference/evidence`.
1213 files / 70 command records. Manifest SHA256:
`27bf6e9e7d2314ed9431dabda0f0498f09293dfdb28214265024b30b028ed7bb`.
Relocation to a Chinese/space path passed. Removal and tampering of a required real SDK output
were rejected; restoration passed. Historical receipts and their original judgments are unchanged.

The registered Flow catalog, actual plain payload stores, extensible compile callbacks,
canonical codecs, Node graph-membership removal and generic graph UI remain outstanding.
LR08/Linux PARTIAL, deferred native input, IME and inherited sanitizer scope remain unchanged.
Host minimize, skinned WAR and VERTEX_COLOR findings remain OPEN. This closure does not claim
new explicit GPU/desktop/input qualification, a general graph lifetime proof, or completion
of MA06/MA08. Main and historical branches were not modified.
