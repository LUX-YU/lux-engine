# Flow dynamic schema transactions

Implementation: `2d78edfa193d8501d85c5b0a34f907c3bb30c203`. lux-cxx: `0a0e7419fc7229df6e372cd35a540249f92250ef`.
**This correction passes. Full MA06/MA08 and the overall task remain incomplete.**

The real installed SDK at `e69853a3dc2e80d0c5cd311afc1d9358b4afb9d6` reproduced
native reconstruction at exhausted PinId capacity. The original source captured successfully;
after `reconstruct()`, both parameter and result IDs were zero, only two exec pins remained in
the topology, and source capture failed (exit 42). The executable, source and original output
are retained. This is a new reproduced defect, not an inherited Material/render finding.

## Implementation and owners

Removed Sequence add/remove overloads, exec-output mixin mutation, `Node::reconstruct`,
`NativeFuncCall::reconstruct/rebind`, and the now-unused graph `assignPinId` implementation.
There are no compatibility wrappers. Sequence creates its complete detached schema from
`SequenceSchema`; NativeCall builds its pins once from its immutable, owned definition.
Source reconstruction and actual compiler/analysis/test consumers use the complete candidates.

Dynamic changes use the existing `FlowGraphEdit`: erase and insert the same NodeId, with explicit
retained PinIds, new-ID requests, surviving links and layout. No second edit system was introduced.
Preparation leaves both the live graph and candidate owners intact on refusal. Commit exchanges
prepared storage; removed snapshots retain old pins, RuntimeObject defaults and native definition/code.
Their destruction happens after commit, with pin defaults destroyed before the final definition owner.
The actual DLL regression replaces the node, releases the old snapshot, then invokes the new node
after external library ownership has gone; the library unloads after its final graph owner is released.

The semantic node classes, their Pin objects and Node.graph still exist. In particular the old
Pin construction protocol has not yet been removed. This does not claim the final plain stores,
control/native/Ability registrations or complete structural-authority migration are finished.

## Verification

All final qualification uses the fixed clean tracked implementation above in an independent source
checkout, reused incremental build trees and a fresh SDK prefix. This is not a cold-build claim.
Every build uses `--target all -j 4 -- -k 0`, followed by a no-work build. Installed consumers use
public installed headers/libraries/DLLs, without private source includes or build-DLL fallback.

| Scope | Result |
|---|---|
| Editor / PLAYER | 163/163 and 91/91; previous test names retained |
| SDK analysis / native / scalar | 4/4, 6/6, 7/7 |
| SDK graph / payload / Ability | 4/4, 2/2, 3/3 |
| Exhausted native schema | Candidate refused; full source, defaults, live owner and all original IDs unchanged |
| Dynamic Sequence | Remove/restore, maximum IDs, exhaustion, links, source roundtrip, layout, graph move and replay |
| Native schema / real DLL | Exec identity, receiver changes, copied metadata, code lifetime and callback after old owner removal |
| Removed APIs | Five actual SDK C2039 negatives; same fixture positive and restored positive |
| Actual native execution | 48 nested-control calls and 3 x 77 registered-value calls |

The 33 original v1 source fixtures still roundtrip. Twelve diagnostic records are byte-identical
to the earlier qualified evidence. Scalar AOT retains 123 exports and 28,393 identical bytes
(`dbca5b42fd333d757779b33a84f365a8a0141fda677cdf35169cf56fc3b2516c`).
Three async objects retain 4,931 framed bytes; six legal control objects retain 5,772 raw bytes.
The invalid Break case still rejects. AOT generation parity is separate from actual native execution.

The original dangerous assertions remain, with schema changes expressed through the transaction.
Added layout coverage initially exposed an incomplete test replay (the fixture omitted layout in its
undo change); it was corrected to restore the layout rather than weaken source equality. Initial
test compilation failures (local name collision and missing array include) are also retained.

Actual CMake source/provider/link inspection preserves the Flow module's exclusion of Editor/UI/
Toolchain dependencies. The four changed public headers match the fresh SDK and Debug,
RelWithDebInfo and Android include prefixes; Android was not built. Six protected user-file hashes
are unchanged. The archived ProjectBuilder patch remains unapplied; main and historical records
were not modified.

## Evidence and remaining work

Archive: `E:/SyncForder/CodeRepos/archives/lux-engine/EditorFramework-v2/mechanism-ma08/dynamic-schema/evidence`.
1,223 files / 70 commands. Manifest SHA256:
`274d87e080510f05384557a6b5c4aca969e372d14a59c0b077c75ff2c857fa8a`.
Chinese/space-path relocation, missing-evidence rejection and tamper rejection passed.

Full Flow registered stores, Node.graph/Pin constructor-protocol removal, control/native/Ability
callbacks, graph UI and later MA work remain unfinished. Linux remains unmet / LR08 PARTIAL.
Native input is user-deferred; IME, sanitizer and historical performance qualifications are unchanged.
Host minimize, skinned WAR and Material VERTEX_COLOR findings remain open. The applicable CTest
matrix ran; this change does not establish new interactive viewport or graph-UI GPU qualification.
