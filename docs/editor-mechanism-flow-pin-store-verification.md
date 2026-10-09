# Flow PinId index qualification

Implementation: `c72fe184c3c40746fac3f6c0989b0cdca83d18a1` (Pin migration `dd10dc9504893f55248d9d4dccb8ca03eda0761c` followed by the maximum-callee reference correction).
lux-cxx: `0a0e7419fc7229df6e372cd35a540249f92250ef`. This closes only Flow Pin identity/index migration; MA06, MA08 and the overall migration remain incomplete.

## Responsibility and deletion

GraphTopology remains the sole identity issuer and structural authority. Pin no longer contains an ID:
`Pin::id`, `Pin::setId`, the embedded ID field and `FlowGraph::assignDetachedPinId` are deleted,
with all active source, analysis, compiler and test callers migrated. No compatibility alias remains.
FlowGraph maintains derived, non-owning PinId-to-Pin and pointer-to-PinId indexes. `findPin` uses a hash
lookup instead of scanning every node and pin. Detached or foreign pointers have no key in that graph.

FlowNodeSnapshot owns the detached node and its input-then-output PinId sequence. FlowNodeInsertion
borrows the candidate owner and sequence during preparation. Restored IDs precede fresh signature
allocation, and issued high-water values never roll back. Preparation constructs indexes and removed
snapshots; commit transfers owners and swaps prepared state without allocation or arbitrary callbacks.
There is still only one shared GraphEdit algorithm. Dynamic Sequence restoration remains possible even
after fresh identity exhaustion. Actual Pin objects remain owned by the existing node members: these
indexes are not yet the final registered FlowPinPayload store.

## Real failure and correction

Against installed SDK `98e24562c37744e9a21c5aaee0b796e0d6280000`, real graphs with maximum NodeId and
PinId are successfully admitted by GraphTopology but rejected by source capture (INVALID_IDENTITY,
exit 42). Both original runs are archived. The first pin fixture left a fresh output after exhausting IDs
and failed during insertion instead; that mistaken fixture and its output are retained separately.

The codec now accepts the valid UINT64_MAX NodeId/PinId. Function call/return references follow the
same node identity contract; graph variables retain their distinct maximum-value sentinel. A real
maximum-callee graph survives capture, encode, decode and materialization with identical source.
The updated installed SDK executes both original maximum-ID scenarios successfully, using the new
explicit restoration input rather than a removed Pin mutation API.

Expanded existing tests retain their assertions and cover indexed membership, exhaustion rollback,
removal/restoration, same-ID replacement, rejected candidates preserving the original source and owner,
graph move, dynamic pins and exact source round trips. Three actual installed-header compile negatives
reject Pin::id, assignDetachedPinId and Pin::setId. The same fixture first builds/runs legally and is
restored to a successful legal build/run after those rejections.

## Fixed-SHA verification

ValidateTrackedSnapshot passed. Source comes from an independent clean tracked checkout. Editor and
PLAYER build trees were reused: this is incremental qualification, not a cold-build claim. All builds
use `--target all -j 4 -- -k 0`; final second builds report no work. The installation prefix is new.

| Actual run | Result |
|---|---|
| Editor | 154/154; exact previous test names retained |
| PLAYER | 87/87; exact previous test names retained |
| Pure installed Flow analysis | 1/1 and standalone header |
| Installed native/DLL/compiler/structure/analysis | 6/6 and standalone headers |
| Installed scalar AOT | 1/1 |
| Installed shared graph/domain/compiler | 4/4 |
| Installed payload/DLL | 2/2 and standalone header |
| Installed Ability metadata/DLL | 3/3 and standalone headers |

Twelve full semantic diagnostics match the archived real SDK `bcbcf722c98334ecb1cbddd49b56c3f183aef90a`
through both pure analysis and the actual compiler. The 123-export scalar fixture generates 28393 bytes,
identical to `f3f1d4b4d0d4bb1fa5d9673317aee864443139e1` (SHA256
`dbca5b42fd333d757779b33a84f365a8a0141fda677cdf35169cf56fc3b2516c`).
Three actual asynchronous AOT cases generate the same framed 4931 bytes as
`c799ddeb364e0b22dce8cb5b265d8fcaaefe21eb` (SHA256
`d4630ff923f0d6a37c9178c5a16b41708aa1c39b2c06d8e7a45c28ef743d0463`).
These are code-generation results, not execution of the generated scripts. Old baseline outputs were
inherited and compared, not reported as rerun at this SHA.

Actual CMake source/provider/link inputs verify the pure Flow closure without Toolchain, Engine,
Editor or UI dependencies. Installed consumers do not use source-private headers or build DLLs.
Two changed public headers match the new SDK and Debug/RelWithDebInfo/Android include prefixes.
Android was not built. Intermediate dd10dc950 qualification is preserved but is not the final matrix.
The failed development build and first codec test failure are retained without changing their outcome.

## Archive and remaining scope

Evidence: `E:/SyncForder/CodeRepos/archives/lux-engine/EditorFramework-v2/mechanism-ma08/flow-pin-store-final/evidence`.
1250 files / 109 command records; manifest SHA256:
`fa9d1ef833e78c8618e2b136cd7874da09af4a152015e497e7774839f4fbeb2d`.
Relocation to a Chinese/space path passed. Removing or tampering with a required real SDK log was
rejected; restoring its bytes passed. Production paths in commands are descriptive; archive verification
uses relative archived paths and the fixed implementation SHA.

All six protected working-file hashes remain unchanged. ProjectBuilder's archived user patch remains
unapplied. Main and historical snapshots were not modified. Linux/LR08 PARTIAL, deferred native input,
IME and prior sanitizer scope are unchanged. Original installed-host minimize failure, skinned WAR and
VERTEX_COLOR compiler failure remain OPEN. No new explicit GPU/desktop/input qualification is claimed.

Node graph membership, polymorphic semantic node/pin classes, real FlowNodeCatalog and compile
callbacks, canonical source type/version codecs and generic graph UI still require migration. The hash
index gives average O(1) PinId lookup, but does not by itself satisfy the complete MA08 registered graph
or final payload-authority gate.
