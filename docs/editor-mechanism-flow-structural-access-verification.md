# Flow structural access qualification

Implementation: `5f59192e21d5e177f728d75ed2bde5dd56d7a01b`. This is a narrow MA08 prerequisite, not MA06/MA08 completion.
The old polymorphic Node/Pin representation, linear lookup, domain catalog/compiler migration
and graph-editor rendering qualification remain outstanding. Restricting access does not remove
the remaining structural fields or establish the final registered payload-store architecture.

## Actual before evidence

The independent installed SDK from `890c34121c4e24d57fabed87cd8378153900eb8a` admitted both:

- `Node::assignStableId` changed the payload identity while the topology node and pin owner kept
  their original identity. `captureFlowSource` accepted the resulting inconsistent model.
- `FlowGraph::topology().detachPin` removed a topology record while `findPin` still returned its
  owned Pin. `captureFlowSource` again accepted the inconsistent model.

Both actual executions returned 42. Each fixture restored and checked the original encoded source
and complete node/pin records before cleanup. Captured inconsistent source bytes and actual command
outputs are retained. No isolated declaration probe substitutes for these runtime results.

## Change and remaining boundaries

Removed the public Node rekey and graph-binding definitions and all their calls; the existing graph
and edit implementation assign their private state. Removed writable topology, input/output list and
NodeStorage access. Pin identity writes and dynamic pin registration/removal are private to their
existing implementation consumers. No new graph store, transaction algorithm, history, registry,
runtime check, compatibility alias or compiler path was added.

Semantic edits remain available through the existing domain operations. Explicit source identities,
dynamic pin reconstruction and GraphEdit still use the original issuance and commit algorithms.
The new real model regression checks topology/owner/pin correspondence, link cleanup, dynamic pin
identity restoration, exact source encoding, decode/materialization, graph moves and edit replay.
Existing mixed-batch, high-water, abandoned-candidate and compiler assertions were retained.

## Fixed-commit results

An independent clean tracked checkout was validated with `ValidateTrackedSnapshot`. Editor and
PLAYER build trees were reused: these are incremental qualification results, not cold-build claims.
All builds used `--target all -j 4 -- -k 0`; the second build reported no work.

| Check | Result |
|---|---|
| Editor CTest | 152/152; previous 151 names retained, `flow.structure_access` added |
| PLAYER CTest | 86/86; previous 85 names retained, same new behavior added |
| New SDK native/DLL/compiler/structure | 4/4 |
| New SDK observable scalar compilation | 1/1; 123 actual AOT exports |
| New SDK shared graph/domain/compiler | 4/4 |
| New SDK erased payload/DLL lifetime | 2/2 |
| New SDK Ability metadata/DLL lifetime | 3/3 |
| Independent public headers | Native set 5, payload set 2, Ability set 2, GraphEdit 1 |
| Illegal structural SDK calls | Eight real compiler rejections; same fixture passed before and after |

The illegal calls cover node ID, graph binding, topology membership, input/output lists, node owner,
pin membership and pin identity. Compiler diagnostics identify the removed/private/const interface;
an unrelated configure or dependency failure is not accepted as a negative result.

The actual installed scalar compiler emitted 28393 bytes, byte-identical to the
archived compiler result from `f3f1d4b4d0d4bb1fa5d9673317aee864443139e1`:
`dbca5b42fd333d757779b33a84f365a8a0141fda677cdf35169cf56fc3b2516c`. This supports the exercised scalar cases, not arbitrary Flow equivalence.
The original result is inherited evidence; only the new SDK execution was rerun here.

Actual File API providers and compile/link commands show Flow's production closure does not depend
on Engine, Editor, UI or Toolchain. Installed consumers use installed headers/libraries and no build
DLL or private/source include fallback. Both changed public headers match Debug, RelWithDebInfo and
Android include prefixes modulo checkout line endings. Android was not built.

## Evidence and preserved scope

External evidence: `E:/SyncForder/CodeRepos/archives/lux-engine/EditorFramework-v2/mechanism-ma08/flow-authority/evidence`.
Archive: 1158 files / 51 recorded commands.
Manifest SHA256: `85f40420a2b5a84ce2af5d62bf455bdcade767994896b3178320dad3e94e57e3`.
The archive was verified after copying to a Chinese/space path. Removing or tampering with the
actual SDK test log was rejected; restored bytes verified successfully.

The six protected user files retain their original recorded byte hashes; the ProjectBuilder patch
remains separately preserved and unapplied. No historical snapshot, main branch or user edit was changed.

LR08 remains PARTIAL because Linux is unmet. Native input remains NOT_RUN_USER_DEFERRED; IME and
unrerun sanitizer qualification retain their prior scope. The existing installed-host minimize
failure, skinned WAR observation and Material VERTEX_COLOR compiler failure remain OPEN under their
original evidence. Passing current CTest does not reclassify those separate results. A source-only
cross-graph Pin/receiver ownership concern is recorded for a real negative before later correction;
this receipt does not claim every Flow mutation boundary or final graph authority is qualified.
