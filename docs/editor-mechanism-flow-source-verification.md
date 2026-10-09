# Flow canonical source and registered payload codecs

Implementation: `ca4903dcb603b337c6852fe18735bdb1e5e9ccd6`. lux-cxx:
`0a0e7419fc7229df6e372cd35a540249f92250ef`.
**This source-format slice passes; MA06, MA08 and the overall migration remain incomplete.**

## Responsibility and compatibility

Flow source v2 stores canonical node type names, schema versions and stable pin semantics.
The public source `operation` field and graph type identity derived from enum ordinals are removed.
GraphTopology remains the only structural authority. A private, frozen v1 ordinal table reads old
files; new writes do not depend on the current operation enum order. Existing builtin parameter
schemas and materialization adapters remain until their actual registration migration.

Registered nodes encode and decode their owning binary payload through their immutable definition.
The source codec itself needs no live catalog or plugin. Materialization resolves the canonical
name/version in the supplied catalog and verifies pin kind, type, count and semantic identity.
Missing definitions, mismatched versions and failed codecs reject accurately; codec failures retain
their full owning domain cause. Editable non-compilable drafts can still be saved. Definitions and
payloads retain the original code lease after the temporary catalog is destroyed.

The original SDK at `a6d4242c101f8ecbe3d83123037c36b376646d80` generated and roundtripped
33 builtin v1 files, then failed registered-node source capture with UNKNOWN_NODE_KIND (exit 42).
The same probe against the new SDK passes. All 33 old files are retained as normal test fixtures;
v1 decode, graph reconstruction, v2 encode/decode and second graph reconstruction preserve IDs,
parameters, pin names/types/literals, links, exports, variables and layout. An independent parsed
before/after comparison permits only the documented version/type/semantic representation changes.

## Actual qualification

Independent clean tracked checkout; reused Editor/PLAYER build trees, **not a cold build**.
Every build uses `--target all -j 4 -- -k 0`, followed by a no-work check. Consumers use a new
SDK prefix, installed headers and libraries; no source private include or build DLL completes them.

| Scope | Actual result |
|---|---|
| Editor full CTest | 162/162; preceding 161 names retained |
| PLAYER full CTest | 90/90; preceding 89 names retained |
| Installed pure Flow analysis/source | 3/3, standalone public headers |
| Installed native/compiler | 6/6 |
| Installed scalar/control/registered DLL | 7/7 |
| Installed graph/domain/compiler | 4/4 |
| Installed payload/DLL | 2/2 |
| Installed Ability/DLL | 3/3 |
| Registered source roundtrip then actual native execution | 3 x 77 calls: builtin, DLL, DLL-created catalog |
| Nested control actual native execution | 48 calls |
| Removed source `operation` API | Same installed fixture passes, fails C2039 when used, passes after removal |
| Original SDK capture probe | Before exit 42; after exit 0 |

Binary payloads including NUL/0xff, dynamic pin counts, draft preservation, missing codec,
version/schema rejection, malformed hex, mixed-format fields, limits and identity collisions
have executable assertions. Existing assertions remain; only API/format representations changed.

Twelve full diagnostics remain byte-identical through module and compiler. Scalar AOT retains
123 exports / 28393 bytes, async AOT retains three objects / 4931 framed bytes, and six legal
control cases retain 5772 raw bytes. Invalid outside-loop Break retains its failure. These byte
comparisons prove code-generation parity; the native-call rows separately prove actual execution.

CMake provider/include/link checks retain Flow's independence from Editor/UI/Toolchain. The changed
FlowSource public header matches the fresh SDK and Debug, RelWithDebInfo and Android include
prefixes. Android was not built. All six protected user-file hashes match; the archived
ProjectBuilder patch remains unapplied. Main and historical qualification records are untouched.

## Evidence and remaining work

Archive: `E:/SyncForder/CodeRepos/archives/lux-engine/EditorFramework-v2/mechanism-ma08/flow-source/evidence`.
1289 files / 64 command records. Manifest SHA256:
`2836afb823696adb6b27a8d40cc6b12565905a7a93b71f67c4c552437a9252fa`.
Relocation to a Chinese/space path and rejection of missing or tampered evidence passed.
The original failure, v1/v2 bytes, fixtures, real commands and build/provider inputs are retained.

Remaining MA06/MA08 work includes builtin/control/native/Ability registrations, the actual plain
Flow payload stores, removal of Node.graph and polymorphic structural adapters, and generic graph
UI. This change is not a completion claim for those responsibilities or later MA stages.
Linux remains unmet / LR08 PARTIAL; native input remains user-deferred. IME, sanitizer and old
performance scopes are unchanged. Host minimize, skinned WAR and VERTEX_COLOR findings remain open.
No new GPU or native-input qualification is claimed.
