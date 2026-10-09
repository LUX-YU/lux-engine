# MA06 Material module compilation verification

Implementation: `3c35febcdb6edc8a41d1ee55b4d445eab40b6ede`. This receipt does not modify production code.
Workspace: `E:/SyncForder/CodeRepos/lux-engine`; branch: `codex/editor-framework-v2`.
lux-cxx/dependency revision: `0a0e7419fc7229df6e372cd35a540249f92250ef`.

## Actual responsibility migration

The existing Material module now owns the sole graph validation and iterative graph-to-ShaderIR
lowering implementations. The original Toolchain validation bodies, MaterialLowering.cpp,
private Lowering.hpp and private MaterialIR.hpp are removed. MaterialIR is the fully owning,
backend-independent domain result; lowerMaterial validates and produces it. Toolchain invokes
that result and retains GLSL/SPIR-V backend generation, reflection and final artifact assembly.
There is no forwarding header, duplicate traversal, extra graph or replacement compiler.

Source format, graph ownership, original lazy input demand, shader fingerprints and failure
classifications remain unchanged. This is a prerequisite for registered node compilation,
not a claim that Material's builtin type switch or polymorphic nodes are already gone.

## Verification bound to the implementation

The clean tracked source at `D:/LuxQualification/ma-source` passed ValidateTrackedSnapshot.
Editor/PLAYER reuse reconfigured incremental build trees; this is not a cold-build result.
The SDK was installed into the new `D:/LuxQualification/ma06-material-lowering-install` prefix.

| Check | Actual result |
| --- | --- |
| Editor target all, second no-work, full CTest | PASS, 147/147 |
| PLAYER target all, second no-work, full CTest | PASS, 81/81 |
| Pure installed Material consumer | PASS, 3/3 |
| Installed Material plus real compiler | PASS, 4/4 |
| Installed graph edit plus both real compilers | PASS, 4/4 |
| Standalone C++20 public headers | PASS, 6 |
| Two changed module headers, three required include prefixes | Exact byte match |
| Six protected user files | Hashes unchanged |

All previous Editor/PLAYER test names remain; each suite adds material.lowering. Its actual
public module test covers invalid/empty/missing-output graphs, diagnostic node identity,
default surface output, all supported math operations, fingerprint consistency, and an
owning result copied after the original graph is destroyed. A two-node cycle through an unused
unary input is accepted; moving that dependency onto the demanded input reports CYCLE. This
locks the old lazy evaluation semantics before registered payload integration.

The first development run was 146/147: the new fixture assigned VEC4 to a Constant output whose
default was already VEC4, then incorrectly expected a mismatch. The fixture now assigns VEC2.
The initial failure is retained; no production classification or prior assertion was weakened.

The previous real module SDK at `931611df33c297b8d54935d43275db211ff8bfdd` cannot compile a
consumer of the new module MaterialIR.hpp (C1083). This is evidence of the previous component
boundary, not a fabricated runtime failure. Positive consumers of the new installed SDK pass.

Identical archived comparison programs were built and run against both actual SDKs:

- Ten builtin node source encodings: 4784 bytes,
  SHA256 `9b58a7b0b65ac5204113e0b40f1e385091c7cc0d1bcd966b35144cc56758c58b`; byte-identical.
- Four Material configurations, eight actual SPIR-V passes: 235228 bytes,
  SHA256 `362658d4bcb96aa1538d332886d8ee73fdc575720d6b225be5976f1a0725b6b0`; byte-identical.

CMake File API, real source providers and compile/link commands confirm the Material module has
no Engine, Editor, UI or Toolchain dependency. The original Toolchain lowering source is absent;
the new validation/lowering sources belong to material_graph. Installed consumers use installed
headers and libraries without source-private include paths or build-tree DLL linkage. The new
private validation header and old private lowering/IR headers are not installed. Verified build
translation-unit counts: 696, 637, 9, 10, 4.

## Evidence and scope

Archive: `E:/SyncForder/CodeRepos/archives/lux-engine/EditorFramework-v2/mechanism-ma06/material-lowering/evidence`.
Manifest SHA256: `6da6756ec52fce1b49d15008270b1e0d00ae1d777fe1129b33759100ecc8ac32`; 1112 files, 38 command records.
Relative archive verification passes after relocation to a Chinese/space path. Missing and
tampered actual SDK CTest logs are rejected; restoring the original passes.

**Only this module compilation migration is qualified. MA06, MA08 and the overall task remain
incomplete.** Registered builtin payload integration, sole Pin authority, FlowNodeCatalog and
remaining MA stages still require implementation. No final graph-editor rendering or O(1)
pin-payload lookup qualification is claimed here.

The original full SDK desktop suite was not rerun. Its 14/15 result at
`617403987b91940b4851b2d8007755a96e492494` remains unchanged. Q-LR03-HOST-MINIMIZE is OPEN;
the common cause with earlier phase-8 failures is unproven. Current source tests do not close it.
LR08 remains PARTIAL; Linux is NOT_RUN/unmet. Native input is NOT_RUN_USER_DEFERRED. IME,
historical skinned WAR and sanitizer qualifications retain their original ranges and SHAs;
there is no new MA sanitizer claim. Android headers were synchronized only. Main, history,
the six user differences and external unapplied ProjectBuilder patch are preserved.
Continue the authorized remaining work without another stage approval.
