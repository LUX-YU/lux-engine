# R1 — Render Core qualification

## Identity and scope

- Result: **PASS** for the R1 Core scope; stop for review before R2.
- Branch: `codex/render-v2`.
- Approved R0 base: `db2c42c3d98277c71e63c1b102cfab57d2b05f29`.
- R1 implementation I: `1d2fa0c0733de1535da9df56831d1bb36e5c6e39`.
- V1 frozen source: `a669409a289a6fa4092f21176397795b1cdb7f3e`.
- This report is the only file added by the subsequent verification commit V.
  V contains no implementation or qualification-script fix.
- Date: 2026-10-10. Qualification used an independent clean clone, not the
  implementation worktree. Both passed `ValidateTrackedSnapshot.cmake`.
- The attached chapter 08 matches the authoritative Git blob exactly:
  SHA-256 `c958328997d391d6f78d5c3ca8850e76a047eb2a0418d2305694057e8a69cf76`.

The implementation adds the real static `render_core` component, five public
headers, descriptor validation, tests and a closure auditor. Changes are limited
to `modules/function/render/core/**` and `cmake/render-v2-bootstrap/**`.
Root/product CMake, Editor/Engine consumers, legacy and existing R0 reports are
unchanged. R0's historical checker retains its original R0-only contract.

## Contract and dependency decisions

Strong semantic IDs reuse `lux::cxx::StrongId` and `Fnv1a64`; handles reuse
`SlotKey`. Canonical names remain in borrowed static descriptors for collision
and definition checks. IDs reserve zero; handle null checks do not assert runtime
membership or liveness. There is no owning registry, queue, route or thread.

Core uses `lux::error::Error`, `ErrorDescriptor` and `lux::cxx::expected`.
The generic error **value headers** are an explicit build include dependency;
the synchronized error registry is neither compiled nor linked. Core exposes a
static error descriptor span for later host assembly. Failure paths construct
only predeclared error IDs and numeric arguments. Production code adds no
throw/catch, logging, terminal output or release-disabled invariant assertion.

Feature and capability metadata has no executable operations. Data metadata has
versions and native layout, but no domain payload or transport contract. R2 must
add lane/kind/reply validation; these checks are not claimed by R1. Target values
include offscreen/presentation kind, stable color/depth semantics and two neutral
device limits. Detailed contracts and borrow lifetimes are in the Core README.

## Independent qualification

Source: `D:/LuxQualification/render-v2-r1-1d2fa0c0733d/source`.
Build: `D:/LuxQualification/render-v2-r1-1d2fa0c0733d/build`.
Entry: `cmake/render-v2-bootstrap/CMakeLists.txt`.

Environment: Windows x64, MSVC 19.44.35228.0 (toolset 14.44.35207), CMake 4.1.2,
Ninja 1.11.1, C++20, RelWithDebInfo, `/EHsc`, `/DNDEBUG`, `BUILD_TESTING=ON`.
The sole package prefix was
`E:/SyncForder/CodeRepos/install/Framework-v2-dependencies`, which has no
`include/lux/engine` tree. User/system CMake package registries were disabled.
The regular Engine SDK prefixes and old build trees were not build inputs.

lux-cxx source SHA: `bf779515a120350c7c5412c366eea73afdf59ccf`.
Toolset source SHA: `99c3d0480d1db816779b1dfa7f3c06cb7d39a94f`.
Installed package files are hash-indexed. All six actually consumed lux-cxx
headers match their source counterparts; hashes of both sides are retained.

| Check | Result | Evidence |
| --- | --- | --- |
| Clean I before configure and after testing | PASS | `01`, `04`, `10`, `11` command logs |
| MSVC x64 / Ninja configure | PASS | `05-configure.log` |
| Full `all -j 4 -- -k 0`, first round | PASS | `06-build-1.log`, 11 build steps |
| Full `all -j 4 -- -k 0`, second round | PASS | `06-build-2.log`: `ninja: no work to do` |
| Five independent public-header TUs | PASS | Included in the default all build |
| CPU contract tests and positive consumer | PASS | `render.core.values`, `render.core.probe_control` |
| Six negative compile consumers | PASS | ID/handle type rejection; Vulkan/Scene/Runtime/legacy headers unavailable |
| CTest total | PASS | 8/8, `ctest.xml`; negative failures are intentional |
| Actual source/include/link closure | PASS | File API, `targets.dot`, Ninja dependency database; 100 header paths audited |
| Legacy mode/blob/size | PASS | 719/719 source files, 720 tree entries including guard |
| Three-prefix public-header synchronization | PASS | 15/15 exact copies from clean I; all destinations were previously absent |
| Original user checkout protection | PASS | Same HEAD, staged/unstaged patches, status and six file-byte hashes |
| Bootstrap installation boundary | PASS | No production install target/rules; no SDK qualification claimed |

The generated graph contains only Core, its test consumers and the four lux-cxx
interface components (plus CTest utility targets). Actual Engine header inputs
are the five new Core headers and two generic error value headers. MSVC STL
internal `xthreads.h` declarations are present through standard headers; no
`<thread>`, `<mutex>`, queue, Render thread API or error registry is introduced.
Compile-negative checks supplement the graph/header audit rather than replace it.

Evidence directory:
`E:/SyncForder/CodeRepos/archives/lux-engine/RenderV2/R1/1d2fa0c0733de1535da9df56831d1bb36e5c6e39/`.
`EVIDENCE_MANIFEST.json` indexes 121 files, including 12 command/exit-code records,
raw logs, dependency graphs, header hashes, install synchronization and user
protection records. Manifest SHA-256:
`cd19977c1caaf5f5bddb56c0b043a7b0edcf15bfd1882f9e7c2e49353d9d1a8d`.

## Limits and handoff

`NOT_RUN`: V1 full matrix, GPU/render behavior, ASan, Linux, Android and installed
SDK qualification. Allocation/performance gates are not claimed by these CPU
contract tests. Header synchronization does not constitute an Android build or
installed SDK qualification. Future installation must include the generic error
value header closure. No V1 known issue or historical result has been rewritten.

```text
R1_CORE = PASS
V1_REFERENCE = REFERENCE_WITH_KNOWN_LIMITATIONS
V2_PRODUCT = EXPECTED_UNAVAILABLE
NEXT_STAGE = R2 / render_transport only, pending approval
STOP
```
