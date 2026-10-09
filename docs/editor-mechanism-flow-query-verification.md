# Flow explicit graph queries

Implementation: `42f49e9f372545f139639767d0aaedbe15a2fd86`. lux-cxx:
`0a0e7419fc7229df6e372cd35a540249f92250ef`.
**This query migration passes; full MA06/MA08 and the overall task remain incomplete.**

Removed five Pin query definitions: ExecInPin::linkedPins, both ExecOutPin::nextPin overloads,
DataInPin::linkedPin and DataOutPin::linkPins. No forwarding aliases remain. Actual control and
suspension analysis, MLIR predecessor/operand lowering and the Ability regression now query the
explicit FlowGraph/GraphTopology. No new graph, index, compiler or registration mechanism was added.

The removed Pin queries discovered their graph through Node.graph. The new callers already borrow
the exact graph being analyzed. Connect and FlowGraphEdit establish pin membership, direction and
execution/data compatibility; these facts remain valid during the unchanged synchronous compile
borrow. No validation result is retained across edits or callbacks. Existing graph lookup resolves
the returned pins; topology incoming/linkCount queries avoid temporary linked-pin lists where only
a source or count is needed. This is not a measured global performance claim.

The old graph pointer remains for dynamic pin construction and removal. Its eventual deletion,
plain registered stores and control/native/Ability registration are outstanding, not hidden by
this API removal.

## Verification

Independent clean tracked checkout, incremental reused build trees; not a cold build. Full builds
use `--target all -j 4 -- -k 0` and a second no-work check. A fresh installed SDK provides headers,
libraries and DLLs for the consumers; no source private includes or build DLLs complete them.

| Scope | Result |
|---|---|
| Editor / PLAYER | 163/163 and 91/91; exact preceding names retained |
| SDK analysis / native / scalar | 4/4, 6/6, 7/7 |
| SDK graph / payload / Ability | 4/4, 2/2, 3/3 |
| Registered native/DLL execution | 3 x 77 calls |
| Nested control native execution | 48 calls |
| Removed query methods | Four C2039 negatives; the same fixture passes before/after illegal calls |

Expanded actual-graph assertions cover equal local IDs in separate graphs, both link directions,
and queries from old/new owners after moving a graph. Existing source, identity, reconstruction,
undo/redo and failure assertions remain. Twelve complete module/compiler diagnostics are
byte-identical to prior evidence. Scalar AOT retains 123 exports / 28393 bytes, async AOT three
objects / 4931 framed bytes, and control AOT six legal objects / 5772 raw bytes. Invalid Break
still rejects. Generation parity and actual native execution are recorded separately.

CMake source/provider/link inspection confirms Flow has no Editor/UI/Toolchain dependency.
The changed NodeBase header matches the new SDK and all three required include prefixes;
Android was not built. All six protected user-file hashes match; ProjectBuilder's archived patch
remains unapplied. Main and historical evidence were not modified.

## Evidence and remaining scope

Archive: `E:/SyncForder/CodeRepos/archives/lux-engine/EditorFramework-v2/mechanism-ma08/flow-query/evidence`.
1207 files / 62 commands. Manifest SHA256:
`e2287e6c5e8e5f4a15124a703ef1c17121b300b4688cc55696f0c8efc912687a`.
Chinese/space path relocation and rejection of missing/tampered evidence passed. Commands and
build inputs are bound to the implementation SHA above, with original output retained.

Full registered Flow stores, Node.graph deletion, builtin/control/native/Ability contributions,
graph UI and later MA work remain unfinished. Linux remains unmet / LR08 PARTIAL; native input
is user-deferred. IME, sanitizer and historical performance scopes are unchanged. Host minimize,
skinned WAR and Material VERTEX_COLOR findings remain open. No new GPU qualification is claimed.
