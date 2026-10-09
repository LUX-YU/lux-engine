# Flow builtin scalar registration

Implementation: `e69853a3dc2e80d0c5cd311afc1d9358b4afb9d6`. lux-cxx:
`0a0e7419fc7229df6e372cd35a540249f92250ef`.
**This scalar migration passes; full MA06/MA08 and the overall task remain incomplete.**

Fifteen arithmetic, comparison and boolean definitions now own real payload creation, pin-schema,
validation, primitive compilation and codec callbacks. The existing palette and source reconstruction
consume those definitions. Removed `ArithmeticNode.hpp`, `ArithmeticNode.cpp`, `BinaryOpNode`,
`UnaryOpNode`, both specialized MLIR lowering methods and their concrete-node dispatch branches.
No forwarding classes or old installed header remain.

`ScalarNodePayload` contains the borrowed operand metadata only. Registration is explicit through
the existing empty `FlowNodeCatalog`; it does not create payloads. The definition remains the
callback/code owner, the payload retains its code and clone/destruction responsibility, and the
original graph/topology owns identity and connectivity. Input declarations preserve the old default
policy and use the existing DataInPin initializer. File identity, typed parameters, exact pin
semantics and zero defaults remain compatible with the established source format.

The intrinsic v1 names remain protected against reassignment and hash collisions. Their module
declarations enter the same catalog admission path; compilation calls the same registered-value
path used by external value nodes. This does not claim that the remaining control/native/Ability
registrations or final plain graph stores have been migrated.

Definitions contributed by a static module copy inside a DLL receive that DLL's original CodeLease.
The actual DLL regression releases the catalog and external library owner, then exercises create,
clone, encode/decode and primitive compilation. It verifies payload cleanup, actual library-owner
release and a late weak-definition release. The stable Object provider remains host-owned, as in
the previously qualified lifetime contract; no foundational-provider unload claim is added.

## Verification

Qualification uses an independent clean tracked checkout of the implementation above, reused
incremental build trees and a fresh SDK prefix. It is not a cold-build claim. All builds use
`--target all -j 4 -- -k 0`, followed by a no-work build. SDK consumers use installed public headers,
libraries and DLLs, without source private includes or build DLL fallback.

| Scope | Result |
|---|---|
| Editor / PLAYER | 163/163 and 91/91; preceding test names retained |
| SDK analysis / native / scalar | 4/4, 6/6, 7/7 |
| SDK graph / payload / Ability | 4/4, 2/2, 3/3 |
| Builtin registration assertions | All 15 definitions: clone, default, codec, compile and graph/source roundtrip |
| Actual old source fixtures | 33 installed-SDK v1 files retain their canonical v2 roundtrip |
| Actual registered native execution | 3 x 77 calls; two DLL modes also run the added builtin lifetime assertions |
| Actual nested control execution | 48 calls across depths 1–3 and both predecessor orders |
| Removed APIs | Arithmetic header C1083; both concrete types C2065; same fixture restored to passing |

Twelve module/compiler diagnostic records are byte-identical to the earlier qualified evidence.
The actual scalar AOT object retains 123 exports and 28,393 identical bytes, SHA256
`dbca5b42fd333d757779b33a84f365a8a0141fda677cdf35169cf56fc3b2516c`.
The three async objects retain 4,931 framed bytes; six legal control objects retain 5,772 raw bytes.
The invalid Break case still rejects. Generation parity is distinct from native execution evidence.

The new ScalarNodes header is also compiled independently as C++20 by the installed consumer.
Actual source/provider/link inspection retains the Flow module's exclusion of Editor/UI/Toolchain
dependencies. Both changed public headers match the SDK and all three required include prefixes;
ArithmeticNode is absent from those prefixes. Android was not built. All six protected user-file
hashes match, and the archived ProjectBuilder patch remains unapplied. Main was not modified.

## Evidence and remaining work

Archive: `E:/SyncForder/CodeRepos/archives/lux-engine/EditorFramework-v2/mechanism-ma08/scalar-registration/evidence`.
1,209 files / 63 commands. Manifest SHA256:
`217d9f5a2f9defa972e4afca6dee24abada12c32df26922f157fde036636c8ef`.
Chinese/space-path relocation, missing-evidence rejection and tamper rejection all passed.
Original outputs, source inventories, build inputs and inherited byte baselines are retained.

Full plain NodeId/PinId stores, Node.graph removal, control/native/Ability registrations, actual
graph UI and later MA work remain unfinished. Linux remains unmet / LR08 PARTIAL. Native input
is user-deferred; IME, sanitizer and historical performance qualifications are unchanged. Host
minimize, skinned WAR and Material VERTEX_COLOR findings remain open. The applicable CTest matrix
ran, but this change does not establish new interactive viewport or graph-UI GPU qualification.
