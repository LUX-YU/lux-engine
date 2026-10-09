# MA08 registered Material graph closure

Implementation `890c34121c4e24d57fabed87cd8378153900eb8a`; this receipt changes no production code.
Workspace `E:/SyncForder/CodeRepos/lux-engine`, branch `codex/editor-framework-v2`.
lux-cxx dependency: `0a0e7419fc7229df6e372cd35a540249f92250ef`.

## Responsibility and removal

GraphTopology remains the sole node/pin membership, identity, direction and link
authority. MaterialGraph now stores registered MaterialNode values under NodeId and
plain MaterialPinPayload values under PinId. Payloads own no structural IDs, graph
pointer or second link list. Pin metadata lookup is average O(1); semantic-to-PinId
lookup remains explicitly linear. The existing GraphEdit owns structural/layout
candidates and issued-identity high-water marks; no new allocator/history/gate was added.

The actual polymorphic Node, Nodes, DataPin headers and Node.cpp were deleted, together
with their active compiler, codec, importer and test call sites. No forwarding alias
remains. Read-only topology prevents the former public wrong-direction mutation;
tests now reject malformed restored records atomically and retain real source/compiler
invalid metadata diagnostics and identity attribution. Dead GraphTopology
ALLOCATION_FAILURE had no producer and was removed without changing remaining values.

MaterialGraphEdit stages cloned semantic values and existing GraphEdit candidates.
Explicit restored pins precede fresh issuance even in mixed request order. Commit
transfers prepared map nodes, with removed code/payload owners retained until edit
cleanup. extractNode transfers the actual payload and owned restoration records.
Snapshot records are restoration values, not a second live structural authority.
Clone rejection preserves the full published source. Existing domain undo/restore,
layout-locality and compiler transaction assertions remain.

Compilation builds temporary indexes from the frozen graph and invokes registered
callbacks for dynamic input/output schemas. Resource validation includes disconnected
nodes. No closed builtin node switch remains in lowering; builtin semantic algorithms
remain unique in their existing module. Actual DLL tests now exercise source decode,
graph storage, lowering, cloning, extraction and final unload after catalog destruction.

## Source migration and real compiler comparison

The writer now emits source v2 canonical node type/version and registered payload bytes.
The v1 reader uses the same builtin codecs. Editable, finite but uncompilable drafts,
disconnected nodes, cycles and missing surface output remain saveable; compilation
still rejects them accurately. Only the old Math operand-type output hint is normalized
to the actual result type when reading a matching v1 hint. Structural source links are
restored independently of compile eligibility. External decoding needs an explicit
catalog; missing codec/type/version and callback failures are explicit errors.

This is a format change: original TOML byte equality is not claimed. The old installed
SDK at `bf73db40bbccfacc8a6fcdf378eed268dbb2c851` actually generated 51 v1 graph files
while running the previously qualified compiler fixture. Its 50 successful graphs /
100 SPIR-V passes and full VERTEX_COLOR failure matched the historical bytes. The new
installed SDK read those exact files, performed v2 encode/decode/re-encode, and compiled
the results. Both SPIR-V and the complete known failure match byte for byte:

- `material.spirv.bin`: 2892432 bytes; SHA256 `32ebe6844c1a8c2f8fe964fe1f2df117d049a02f8aec8b04f82c6e312087ebe0`.
- `material.spirv.bin.failure`: 1092 bytes; SHA256 `a8268eb6ec667112db09dc2e62d6bd5a1e9e0a44b2da44ae18ac0b5d686a99ce`.

The normal test fixture `all_nodes_v1.luxmaterial` comes from the earlier actual
ten-builtin source output at `mechanism-ma06/material-builtins/evidence/actual-after/material.toml`.
It checks node/pin/layout identity and semantic roundtrip, not a fabricated before failure.

## Qualification

ValidateTrackedSnapshot passed for the independent clean tracked source. Editor and
PLAYER build trees were reused and reconfigured: incremental, not cold-build evidence.
The SDK prefix was new. Every final second build reports no work.

| Executed check | Result |
| --- | --- |
| Full Editor CTest | 151/151 PASS |
| Full PLAYER CTest | 85/85 PASS |
| Installed Material domain | 7/7 PASS |
| Installed actual compiler | 8/8 PASS |
| Installed shared graph / both domain compilers | 4/4 PASS |
| Installed catalog / actual DLL | 2/2 PASS |
| Standalone C++20 Material headers | 9 domain + 3 registration headers PASS |
| Old source / new source / actual compiler comparison | 50 successes, 100 equal outputs; original failure retained |
| Removed SDK headers | Positive build, Node/Nodes/DataPin each C1083, restored positive build |
| Six protected user differences | Original hashes unchanged |

No prior test name was removed. material.registered_graph adds dynamic multi-output
lowering, external codec, missing catalog/version rejection, clone failure with unchanged
source, mixed restored/fresh identity ordering and absorbing ID exhaustion. Graph and
DLL tests continue to use public SDK headers; test helpers do not inject private models.

The first syntax run failed on explicit toml::table construction; the first full build
failed on the removed invalid_pin constant in MaterialCompiler. Both outputs remain
archived, alongside the corrected builds. They are not erased or reported as passes.

Actual CMake providers/include/link commands show material_graph has no Engine, Editor,
UI, Toolchain or legacy dependency. The new SDK contains no removed public headers or
private codec headers. Modified module public headers were synchronized to Debug,
RelWithDebInfo and Android include prefixes; Android was not built.

## Evidence and remaining work

Archive: `E:/SyncForder/CodeRepos/archives/lux-engine/EditorFramework-v2/mechanism-ma08/registered-material-graph/evidence`.
1219 files / 49 commands; manifest SHA256 `e1469299b985ffac3c0f2428fe679de79617942deeb3e933b8a852af75a94843`.
Archive-relative hashes pass after Chinese/space-path relocation. Missing or modified
actual SDK test output is rejected; restoring the original bytes passes.

This is **PASS for the registered Material graph closure only**. FlowNodeCatalog,
Flow's remaining polymorphic node/pin storage, graph-editor rendering qualification,
the rest of MA06/MA08 and the overall goal remain unfinished. No concrete tool UI was
silently migrated and no CPU graph result is presented as graph UI qualification.

VERTEX_COLOR remains OPEN; identical failures do not establish backend support.
Q-LR03-HOST-MINIMIZE retains the original installed SDK14/15 failure at
`617403987b91940b4851b2d8007755a96e492494`; this closure does not rerun that SDK suite.
LR08 PARTIAL, Linux NOT_RUN/unmet, native input NOT_RUN_USER_DEFERRED, IME untested,
historical sanitizer scope and skinned WAR stay unchanged. Main, history and the
external unapplied ProjectBuilder patch remain protected.
