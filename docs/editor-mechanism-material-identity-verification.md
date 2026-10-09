# MA08-C Material node identity verification

Implementation: `931611df33c297b8d54935d43275db211ff8bfdd`. This receipt does not change production code.
Branch: `codex/editor-framework-v2`; workspace: `E:/SyncForder/CodeRepos/lux-engine`.
lux-cxx source/dependency revision remains `0a0e7419fc7229df6e372cd35a540249f92250ef`.

## Responsibility and deletion

Deleted Material Node::id(), setId() and id_. GraphTopology remains the identity issuer;
MaterialGraph stores payloads by that key. MaterialGraphEdit receives explicit borrowed
MaterialNodeEntry values: zero requests fresh issuance; a nonzero key requests restoration.
Prepared results return assigned keys alongside borrowed payloads. There is no reverse-pointer
lookup, replacement allocator, extra identity map or compatibility accessor.

Actual topology registration, transactions, deterministic source encoding and compiler validation/
lowering now receive their key from the graph. Detached draft validation reports no identity unless
its caller supplies the diagnostic key. Runtime error classification, original no-fail commit,
issued-identity high-water rules, source format and actual shader algorithms remain unchanged.
Existing transaction assertions were migrated to explicit keys; none were removed. The old assertion
that an untouched candidate has no ID now checks the request key and unchanged candidate pin/value.

The old installed SDK at `dcb6774850ae973fb0c499cdd38484a048f79978` compiled and ran the
archived before fixture. Node::setId produced payload ID 101 while topology and graph lookup still
used ID 1, returning 42. This records duplicate mutable authority, not an invented crash.
The same old program is rejected by the new installed headers specifically for missing id/setId,
after successful positive SDK consumers. Static absence assertions cover both members separately.

## Actual verification

The implementation was checked out cleanly at `D:/LuxQualification/ma-source` and passed
ValidateTrackedSnapshot. Existing Editor and PLAYER build trees were reconfigured incrementally;
this is not a cold-build claim. Installation uses a fresh `ma08-material-identity-install` prefix.

| Check | Result |
| --- | --- |
| Editor target all, second no-work, full CTest | PASS, 146/146 |
| PLAYER target all, second no-work, full CTest | PASS, 80/80 |
| Installed pure Material consumers | PASS, 2/2 |
| Installed Material plus actual compiler consumers | PASS, 3/3 |
| Installed shared graph / both actual compilers | PASS, 4/4 |
| C++20 standalone public headers | PASS, 5 |
| Removed Node identity public API | Compile rejected for the exact removed members |
| Three required module include prefixes | Exact match, 3 changed headers |
| Six protected user files | Original hashes unchanged |

Tests exercise large store keys, extract/restore, a fresh clone of the same detached value in
another graph, duplicate rejection without publication, stable pins and links, layout restoration,
source roundtrip, cloning, abandoned/rejected preparation, and accurate codec/compiler diagnostics.
The installed shared transaction test invokes actual Material and Flow compilers and retains their
original equality assertions. Previous Editor/PLAYER test names are intact; each adds only
material.node_identity. Counts alone are not the acceptance criterion.

The same archived comparison sources were compiled and executed against both real installed SDKs:

- All ten builtin node kinds: deterministic TOML, 4784 bytes,
  SHA256 `9b58a7b0b65ac5204113e0b40f1e385091c7cc0d1bcd966b35144cc56758c58b`; old/new bytes identical.
- Four actual Material configurations, eight SPIR-V passes, 235228 bytes,
  SHA256 `362658d4bcb96aa1538d332886d8ee73fdc575720d6b225be5976f1a0725b6b0`; old/new bytes identical.

Actual CMake provider/dependency and compile/link commands show the pure Material module has no
Engine, Editor, UI or Toolchain provider. Installed consumers use installed headers/libraries,
without source-private includes, legacy or build-tree DLL linkage. Translation-unit counts by
verified build: 694, 635, 7, 8, 4.

## Scope and retained limitations

**This closes Material NodeId duplication only. MA06, MA08 and the overall task are incomplete.**
Material PinId/pin structure, registered payload integration, removal of polymorphic builtin nodes,
FlowNodeCatalog/public compilation and Flow graph authority remain required. This receipt does not
claim O(1) final pin-payload lookup or graph-editor rendering qualification.

Evidence: `E:/SyncForder/CodeRepos/archives/lux-engine/EditorFramework-v2/mechanism-ma08/material-identity/evidence`.
Manifest SHA256: `eac87f0682ef89f676fa73aa80848c122fea03bf569fdd1f7192a0a0eca8b85e` (1109 files, 39 commands).
Archive-relative verification passed after Chinese/space-path relocation. Removing or tampering
with actual SDK CTest evidence was rejected; restoring it passed.

The original SDK desktop suite was not rerun. Its 14/15 failure at
`617403987b91940b4851b2d8007755a96e492494` remains inherited unchanged; Q-LR03-HOST-MINIMIZE
is OPEN, and the common cause with old phase-8 failures remains unproven. Current source CTest
success does not close that finding. Linux is NOT_RUN/unmet; LR08 remains PARTIAL. Native input
is NOT_RUN_USER_DEFERRED. IME, historical skinned WAR, sanitizer and other deferred ranges retain
their original status. No new MA sanitizer evidence is claimed. Android includes were synchronized,
not built or tested. Main, history and the external unapplied ProjectBuilder patch are untouched.
Continue the authorized remaining MA work without requesting another stage approval.
