# MA06 builtin Material compilation qualification

Implementation `cfcfa97b2a69b0328204e21f6508acfbdc662a05`. This receipt modifies no production code.
Workspace `E:/SyncForder/CodeRepos/lux-engine`, branch `codex/editor-framework-v2`.
Dependency/lux-cxx revision remains `0a0e7419fc7229df6e372cd35a540249f92250ef`.

## Actual responsibility migration

BuiltinMaterialNodes.hpp declares nine plain semantic payloads alongside the existing
MaterialMath. materialBuiltinRegistrations() explicitly contributes all ten definitions to
the existing MaterialNodeCatalog. Names are canonical, versioned and hashed; composition is
explicit, with no static catalog installation. Definitions retain the original payload/code
ownership. Payloads contain no NodeId, PinId, topology membership or second link list.

The actual graph compiler and registered callbacks share the same builtin emitters. Constant,
Input, Texture, Parameter, Swizzle, Construct, DecodeNormal and TbnTransform emission moved
out of MaterialLowering.cpp; the old bodies are deleted. Input-slot projection now checks
and reuses ShaderIR's real declaration instead of maintaining another lookup map. Original
vertex interpolant locations and first-use order remain. Surface binding emission moved to
one implementation; graph traversal still resolves the original overridden pin defaults
before calling it. Registered Surface accepts absent SSA values, preserving the contract
fallback instead of eagerly generating constants. Duplicate surfaces and invalid resource
slots/types fail without partial adoption.

Constant/Input/Param/Swizzle/Construct semantic validation is shared with the real graph
validator. Structural validation, graph resource declarations and source diagnostic identities
remain where they belong. The small private registration template only binds these concrete
module-owned payloads; it is not a public generic node framework. Future unsupported payloads
cannot silently acquire a no-op validator.

The old graph Node/Pin representation still exists. Its switch now projects semantic payloads;
this is an intermediate consumer migration, not a claim that registered graph storage,
canonical source codec or sole Pin authority has been delivered. No new library, Runtime,
manager, graph store or Toolchain dependency was introduced.

## Executed verification

ValidateTrackedSnapshot passed at the fixed implementation SHA in independent clean source
`D:/LuxQualification/ma-source`. Existing Editor/PLAYER build trees were reconfigured;
this is incremental qualification, not a cold-build claim. Both final second builds have
no work. All prior test names remain, with material.builtin_nodes added.

| Check | Actual result |
| --- | --- |
| Editor all / no-work / full CTest | PASS 149/149 |
| PLAYER all / no-work / full CTest | PASS 83/83 |
| Fresh installed pure Material | PASS 5/5 |
| Fresh installed actual Material compiler | PASS 6/6 |
| Fresh installed shared graph / both compilers | PASS 4/4 |
| Fresh installed Material catalog / real node DLL | PASS 2/2 |
| Standalone C++20 public headers | Pure consumer 9, node consumer 3 |
| Changed module public header / three include prefixes | Exact match |
| Six protected user file hashes | Unchanged |

The new real-module and installed tests compare registered emission to graph lowering across
all value types, five material inputs, variable Construct pin counts, texture/parameter slots,
normal transforms, surface defaults and explicit overrides. They verify code/payload lifetime
after catalog destruction, clone independence, deduplicated input slots, conflicting input
metadata, non-finite constants, invalid enum/swizzle components, resource errors, input shape
rejection and no candidate mutation on these failures. Original Math, graph transaction,
identity, lazy-input and actual DLL assertions remain.

The same external comparison source ran against the previous SDK at
`7b1429844a6222a68c06781f0ae512598696bd2c` and this fresh SDK:

- Ten builtin source encodings match exactly: 4784 bytes,
  SHA256 `9b58a7b0b65ac5204113e0b40f1e385091c7cc0d1bcd966b35144cc56758c58b`.
- 50 successful graphs / 100 SPIR-V passes match exactly: 2892432 bytes,
  SHA256 `32ebe6844c1a8c2f8fe964fe1f2df117d049a02f8aec8b04f82c6e312087ebe0`. This includes 22 live-input Math graphs,
  all scalar/vector widths, four successful inputs and surface overrides. The fifth input, VERTEX_COLOR, fails as described below.

The original and new SDK each fail the original compiler comparison (1/2 tests, CTest exit 8):
VERTEX_COLOR emits an undeclared `vertex_color` GLSL variable. This is an actual existing
backend defect, not an allowed-feature qualification or an archive-script issue. Both full
failures are retained. An expanded outcome fixture then tests all 51 graphs, requires the
exact SHADER_COMPILATION_FAILURE for that one input, and compares the complete failure
code/node/pin/message/GLSL alongside the 50 successful graphs. The failure has
1092 bytes and SHA256
`a8268eb6ec667112db09dc2e62d6bd5a1e9e0a44b2da44ae18ac0b5d686a99ce`.
**This closure is PARTIAL_OPEN_VERTEX_COLOR; the new finding remains OPEN.** Outcome parity
does not turn the original failures into passes or qualify VERTEX_COLOR support. It must be
addressed separately from the inherited minimize issue; no old failure identifier absorbs it.

The initial outcome aggregation script referenced the original comparison output directory
and stopped on a missing failure file. Only its path was repaired; completed tests were not
rerun or fabricated, and the archived scripts preserve that continuation.

The original SDK rejects the new builtin header
with C1083; this records new API absence, not a pre-existing runtime defect. No old binary
plugin compatibility is inferred from rebuilding and running the real DLL consumer.

CMake File API and compile/link inputs verify the module has no Engine, Editor, UI, Toolchain
or legacy provider. Installed consumers do not use source private headers or build-tree DLLs.
Translation-unit counts: 700, 641, 14, 15, 4, 6.

## Evidence and limits

Archive `E:/SyncForder/CodeRepos/archives/lux-engine/EditorFramework-v2/mechanism-ma06/material-builtins/evidence`:
1156 files, 49 commands, manifest `e1a4f43d0a40ce92038dd34358c7be56be13b70ccb557d60057be089e51fb3ed`.
Archive-relative hashes pass after relocation to a Chinese/space path. Deleting or tampering
with actual SDK evidence is rejected; restoring its bytes passes.

**MA06/MA08 and the overall goal remain incomplete.** Registered graph storage, canonical
payload codec, sole Pin payload store, old polymorphic Node removal, FlowNodeCatalog and other
remaining MA phases still require implementation and qualification. This is not graph-editor
rendering or final O(1) Pin lookup qualification.

The original full desktop SDK result remains 14/15 FAIL at
`617403987b91940b4851b2d8007755a96e492494`, inherited and not rerun. Q-LR03-HOST-MINIMIZE remains
OPEN; common cause with earlier failures is unproven. Source CTest success does not close it.
LR08 remains PARTIAL; Linux NOT_RUN/unmet, native input NOT_RUN_USER_DEFERRED, IME untested,
original sanitizer coverage and historical skinned WAR retain their original SHAs and scope.
Android is include synchronization only. Main, all user differences and the external unapplied
ProjectBuilder patch are preserved. Continue the authorized remaining work.
