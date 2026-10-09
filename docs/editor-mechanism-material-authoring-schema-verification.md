# MA08 Material authoring schema prerequisite

Implementation `bf73db40bbccfacc8a6fcdf378eed268dbb2c851`; this receipt changes no production code.
Workspace `E:/SyncForder/CodeRepos/lux-engine`, branch `codex/editor-framework-v2`.
lux-cxx dependency: `0a0e7419fc7229df6e372cd35a540249f92250ef`.

## Correction and actual before evidence

The actual installed SDK at `1eadff3024d3691453bb51cd8def6dea183ce52a` could encode,
decode and re-encode five editable drafts, but its registered describePins rejected all
five: LERP, scalar DOT, scalar CROSS, Vec2 CROSS, and a scalar-input Swizzle selecting
unavailable components. The reproducer returned 42. The real graph compiler and registered
compiler both retained the expected semantic diagnostic. No private-header shim was used.

The first reproducer incorrectly connected a Vec2 output to the Surface base-color input
and stopped at that unrelated admission check. Its original source and failed output are
retained in `before/`. The corrected fixture uses disconnected draft nodes, which are
explicitly saveable and still checked by the real compiler; `before-corrected/` is the
fixture actually used for the five-case before/after comparison.

MaterialNodeType::describePins now checks payload/type/code ownership and calls the
registration's authoring schema callback. It no longer requires compilation eligibility.
MaterialNodeType::compile still performs intrinsic compilation validation first, before
schema/input processing or IR mutation. The registration contract documents that
DescribePins itself must reject malformed fields before interpreting them.

Math and Swizzle reuse one field-validation implementation between the actual source
codec and registered schemas. Compile-only restrictions remain in their original domain
validators. Invalid enum/type/component values are still rejected. Swizzle preserves the
first offending component's diagnostic order. The source's error domain, fields and
identity attribution remain intact; the persistent source format did not change.

No new callback family, catalog, owner, graph store or exception path was added. The
removed check was compilation eligibility at an authoring query boundary; ownership
validation remains at both public entry points. The removed codec component loop is
replaced by the same synchronous field validator, with no intervening callback or owner
replacement. Typed source decode still validates untrusted serialized values.

## Qualification

ValidateTrackedSnapshot and an independent clean tracked source passed. Editor/PLAYER
build trees were reused and reconfigured: this is incremental, not cold-build evidence.
Every final second build reports no work. All commands bind the implementation above.

| Executed check | Result |
| --- | --- |
| Full Editor CTest | 150/150 PASS |
| Full PLAYER CTest | 84/84 PASS |
| Fresh installed Material domain | 6/6 PASS |
| Fresh installed actual Material compiler | 7/7 PASS |
| Shared graph / Material and Flow compilers | 4/4 PASS |
| Material catalog / real DLL | 2/2 PASS |
| Standalone C++20 headers | 9 domain + 3 node headers PASS |
| Identical corrected old-SDK fixture against new SDK | All five schemas available; exit 0 |
| Six protected user differences | Hashes unchanged |

The existing Math LERP test still checks rejection and the exact diagnostic through
compile(), while its schema assertion now checks editable pins. No failure assertion was
removed without replacement. The catalog regression also proves an external node can
expose editable pins while compilation rejects it before modifying IR. The new test checks
real source roundtrip, clone, accurate compiler node/pin diagnostics, malformed payloads,
empty/mismatched payloads and unchanged IR on rejection. No prior test name was removed.

The unchanged installed comparison fixture preserves all ten builtin source encodings,
50 successful graphs / 100 actual SPIR-V passes, and the complete VERTEX_COLOR failure.
- `material.toml`: 4784 bytes; SHA256 `9b58a7b0b65ac5204113e0b40f1e385091c7cc0d1bcd966b35144cc56758c58b`.
- `material.spirv.bin`: 2892432 bytes; SHA256 `32ebe6844c1a8c2f8fe964fe1f2df117d049a02f8aec8b04f82c6e312087ebe0`.
- `material.spirv.bin.failure`: 1092 bytes; SHA256 `a8268eb6ec667112db09dc2e62d6bd5a1e9e0a44b2da44ae18ac0b5d686a99ce`.

Actual CMake providers, source/include/link commands and fresh installation show no Engine,
Editor, UI, Toolchain or legacy dependency beneath material_graph. The modified public
contract is synchronized to Debug, RelWithDebInfo and Android includes. Android was not built.

## Evidence and limits

Archive: `E:/SyncForder/CodeRepos/archives/lux-engine/EditorFramework-v2/mechanism-ma08/material-authoring-schema/evidence`.
1157 files / 45 commands; manifest SHA256 `36d91298a07db0cd404983ac8d715375933589d981fd4bbbdb80470e1919c90f`.
Archive-relative verification passed after Chinese/space-path relocation. Deleting or
altering the actual SDK test log was rejected; restoring it passed.

This is **PASS for the authoring-schema prerequisite only**. MaterialGraph still uses
polymorphic Node storage and PinId-bearing DataPin. Registered graph storage, canonical
codec identity/version, semantic PinId payload lookup, old Node deletion, FlowNodeCatalog,
MA06/MA08 and the overall goal remain unfinished. This does not qualify graph UI rendering.

VERTEX_COLOR backend compilation remains OPEN; matching failures do not establish support.
Q-LR03-HOST-MINIMIZE remains OPEN with the original desktop SDK14/15 failure at
`617403987b91940b4851b2d8007755a96e492494`; no new desktop SDK qualification is claimed.
LR08 remains PARTIAL; Linux NOT_RUN/unmet, native input NOT_RUN_USER_DEFERRED, IME untested,
historical sanitizer scope and skinned WAR remain unchanged. Main, historical evidence,
the six user differences and the external unapplied ProjectBuilder patch are preserved.
