# Flow connection authority qualification

Implementation: `bbd6fd69d0235a639676b801b8099e92657443fa`. lux-cxx:
`0a0e7419fc7229df6e372cd35a540249f92250ef`.
**Connection responsibility passes; MA06/MA08 and the overall migration remain incomplete.**

The receiving FlowGraph now performs membership checks, directional admission and structural
commit. Its indexed pin membership and GraphTopology are authoritative. Fifteen virtual
`canLink/linkTo/unlinkFrom` methods and two unused `hasPin` helpers are deleted, with all active
callers migrated. Inputs are const pin references: connecting changes the graph, not the payload.
The original topology commit and error classifications remain. Pin query adapters, Node.graph
and polymorphic storage still require the subsequent full payload migration.

## Behavioral evidence

Before changing production code, a real installed SDK at `ca4903dcb603b337c6852fe18735bdb1e5e9ccd6`
ran 588 directional combinations across execution/data pins, matching/mismatched types and
occupied/free links. The same fixture against the new SDK produces identical complete output.
Every rejected request preserves the entire encoded Flow source. Success adds one link;
reverse duplicate admission and reversed/repeated disconnect preserve the original classifications
and restore the original source. This is a compatibility baseline, not a reproduced defect.

The first development build exposed const pin consumers; its failure is retained. The graph
API was corrected to accept read-only inputs, without casting away constness or reducing assertions.

## Qualification

Independent clean tracked checkout, reused incremental build trees; **not a cold build**.
Full builds use `--target all -j 4 -- -k 0`, followed by a no-work check. Installed consumers
use a fresh SDK, without source private headers or build-tree DLL dependencies.

| Scope | Result |
|---|---|
| Editor / PLAYER | 163/163 and 91/91; all preceding test names retained |
| SDK analysis / native / scalar | 4/4, 6/6, 7/7 |
| SDK graph / payload / Ability | 4/4, 2/2, 3/3 |
| Registered native compilation, source and DLL lifetime | 3 x 77 actual calls |
| Nested control native execution | 48 actual calls |
| Removed APIs | Four C2039 negatives; same fixture passes before and after illegal calls are removed |
| Directional connection probe | 588 results byte-identical to the old SDK |

Twelve complete module/compiler diagnostics match the original evidence. Scalar AOT retains
123 exports / 28393 bytes; async AOT retains three objects / 4931 framed bytes; six legal control
objects retain 5772 raw bytes. Invalid outside-loop Break still fails accurately. Byte comparisons
prove generation parity; the native execution rows are separate evidence.

Actual CMake provider/include/link checks confirm Flow remains independent of Editor/UI/Toolchain.
Both changed public headers match the new SDK and the Debug, RelWithDebInfo and Android include
prefixes. Android was not built. All six protected user-file hashes match; the archived ProjectBuilder
patch remains unapplied. Main and past qualification snapshots are unchanged.

## Archive and limits

Archive: `E:/SyncForder/CodeRepos/archives/lux-engine/EditorFramework-v2/mechanism-ma08/flow-connection/evidence`.
1228 files / 71 command records. Manifest SHA256:
`b17ab6b989d645f3eb2e94b1d0fe126f4d7dc96f812d6b64e2710f18171c56a8`.
Relocation to a Chinese/space path, missing evidence rejection and tampering rejection passed.
The initial build failure, original fixture, output comparisons and actual build inputs are retained.

Full Flow registered stores, control/native/Ability contributions, Node.graph removal, generic graph
UI and later MA work remain outstanding. Linux is unmet / LR08 PARTIAL; native input stays
user-deferred. IME, sanitizer and old performance scopes are unchanged. Host minimize, skinned WAR
and Material VERTEX_COLOR findings remain open. No new GPU or native-input result is claimed.
