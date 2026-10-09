# MA06 registered Material Math / shared compilation verification

Implementation: `7b1429844a6222a68c06781f0ae512598696bd2c`. This receipt changes no production code.
Workspace `E:/SyncForder/CodeRepos/lux-engine`, branch `codex/editor-framework-v2`.
lux-cxx/dependency revision remains `0a0e7419fc7229df6e372cd35a540249f92250ef`.

## Responsibility

MaterialMath is a plain semantic payload with operation and operand type, without NodeId,
PinId, links or graph membership. materialMathRegistration contributes its canonical
`lux.material.math.v1` identity, factory, pin declaration, validation and compilation to the
existing MaterialNodeCatalog. Payload cloning and code lifetime use the existing owner.
The surviving definition and payload work after the original catalog is destroyed.

One private Material module operation table and appendMath implementation now serve both
registered compilation and the actual existing MaterialGraph lowering path. The old lowering
math emitter, operation mapper, unary classifier and result-type helper were removed, along
with the duplicated graph compilation payload validation. EMathOp moved to MaterialMath.hpp
without a namespace alias or duplicate definition. The original graph still owns its old node
and pin containers; this receipt does not pretend that graph storage has been converted.

MaterialPinDeclaration now states input demand: VALUE requires a resolved SSA value;
CONNECTED_VALUE permits kNoValue for an unconnected input; UNUSED requires kNoValue and must
not be evaluated by the graph compiler. Output declarations cannot carry an input demand.
The catalog validates these contracts before invoking extension compilation. This preserves
the actual unary unused-pin behavior and provides the contract needed for later graph traversal.
No new manager, registry, executor, graph or Toolchain dependency was introduced.

## Actual verification

ValidateTrackedSnapshot passed in the independent clean source `D:/LuxQualification/ma-source`.
Existing Editor/PLAYER build trees were reconfigured incrementally, not cold-built. The final
qualification's second builds explicitly report no work. The development command named
final-development-no-work did perform work after late source/CMake edits; it is archived as
development history and is not used as no-work evidence.

| Check | Result |
| --- | --- |
| Editor all / no-work / full CTest | PASS 148/148 |
| PLAYER all / no-work / full CTest | PASS 82/82 |
| Fresh installed pure Material consumer | PASS 4/4 |
| Fresh installed Material with actual compiler | PASS 5/5 |
| Fresh installed shared graph and both compilers | PASS 4/4 |
| Fresh installed Material catalog / actual DLL | PASS 2/2 |
| Pure consumer standalone C++20 headers | PASS 8; node consumer also compiles its 3 headers |
| Three changed public headers / three include prefixes | Exact match |
| Six protected user files | Hashes unchanged |

All prior Editor/PLAYER names remain; each adds material.math_registration. New tests compare
all 22 supported registered Math emissions against the actual graph lowering, including scalar
DOT/LENGTH results, unary unused inputs, clone independence, failure-before-mutation, unsupported
LERP and invalid values. Existing source, graph edit, identity, lazy-cycle, compiler and DLL
assertions remain. The catalog test adds optional/unused input, invalid mode and output misuse
cases without removing its original failure/lifetime assertions.

The same archived comparison sources were built against the real previous SDK
`3c35febcdb6edc8a41d1ee55b4d445eab40b6ede` and the new SDK:

- Ten builtin node source encodings are byte-identical: 4784 bytes,
  SHA256 `9b58a7b0b65ac5204113e0b40f1e385091c7cc0d1bcd966b35144cc56758c58b`.
- 26 actual Material graphs / 52 SPIR-V passes are byte-identical: 1498796 bytes,
  SHA256 `c19e6f143c40e45a3c5ed5917805ab3e47cfd9231852970743ebd55483d7d248`. The additional 22 graphs each consume
  WORLD_POSITION and exercise one supported Math operation; they are not unused functions.

The old module SDK rejects the new MaterialMath.hpp consumer with C1083 after its original
compiler comparison passes. This records prior API absence, not a historical runtime bug.
The new SDK's positive consumers and rebuilt node DLL pass. No binary compatibility with an
old DLL built against the prior pin-declaration layout is claimed.

CMake File API and actual compile/link inputs retain the pure Material module boundary, with
no Engine, Editor, UI, Toolchain or legacy provider. Installed consumers do not import source
private headers or build-tree DLLs. Verified translation-unit counts:
698, 639, 12, 13, 4, 6.

## Evidence / remaining work

Archive: `E:/SyncForder/CodeRepos/archives/lux-engine/EditorFramework-v2/mechanism-ma06/material-math/evidence`.
Manifest `c391d4fc03c8a5b1c4003110fcc73143d3a0330d19cec0feca5a7d6929d9bfda`; 1134 files and 40 commands.
Archive-relative verification passes after Chinese/space-path relocation. Missing and tampered
actual SDK test evidence are rejected; restoring the bytes passes.

**MA06, MA08 and the overall goal remain incomplete.** This qualifies the registered Math
compilation path and its shared algorithm, not a registered-payload MaterialGraph. Remaining
builtin registration, canonical source codec, sole Pin payload store, polymorphic node removal,
FlowNodeCatalog and other MA phases remain required. No graph-editor rendering qualification
or final O(1) Pin lookup claim is made here.

The original full SDK desktop suite is inherited, not rerun: 14/15 at
`617403987b91940b4851b2d8007755a96e492494`. Q-LR03-HOST-MINIMIZE stays OPEN; a common cause
with earlier phase-8 failures is unproven. Current source CTest success does not close it.
LR08 stays PARTIAL; Linux is NOT_RUN/unmet. Native input is NOT_RUN_USER_DEFERRED. IME,
historical skinned WAR and sanitizer ranges retain their original status and implementation
SHAs. Android includes were synchronized only. Main, history, the six user differences and
the external unapplied ProjectBuilder patch are preserved. Continue the authorized remaining work.
