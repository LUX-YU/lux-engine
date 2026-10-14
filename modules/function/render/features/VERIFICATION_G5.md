# G5 independent verification — 2026-10-14

**G5 module qualification: PASS. Root TOOLCHAIN/product closure: BLOCKED.**
G6 NOT_STARTED. GPU, Vulkan Validation and SyncValidation: N/A.
IPC-R2 historical performance PARTIAL and G4 historical PASS are unchanged.

## Commit and independent source

- Construction baseline / fetched remote HEAD / I parent:
  `f80902e100879b114e848cabe1469540aba4d25d`.
- Implementation I: `44875e80a5d86cf84730e4b7a6f504d7710a9504`.
- V parent: exactly I. This V adds only this report. Its content-dependent SHA is
  supplied after committing in the delivery message and external receipt.
- Remote HEAD rechecked before V: still `f80902e100879b114e848cabe1469540aba4d25d`.
- Implementation worktree:
  `C:/Users/chenh/.codex/worktrees/render-g5/lux-engine`,
  branch `codex/g5-feature-implementation`.
- Independent source: `E:/RenderV2Rebuild/G5/verify/source`, detached at exact I.
  Created with `git -c core.autocrlf=false clone --no-local --single-branch
  --branch codex/g5-feature-implementation E:/SyncForder/CodeRepos/lux-engine
  E:/RenderV2Rebuild/G5/verify/source`, then exact-SHA checkout.
  No alternates or shared object store. Source remained clean throughout V;
  no production/test repair was made in the verification clone.
- The frozen Legacy reference is outside the selected branch history. It was
  explicitly fetched from the source repository by SHA before the protection
  audit. The shallow-root warning did not prevent obtaining the tree; the audit
  then read that actual tree and passed. No reference tree was fabricated.
- `ValidateTrackedSnapshot.cmake`: PASS before configuring.

Evidence root:
`E:/RenderV2Rebuild/G5/qualification/44875e80a5d86cf84730e4b7a6f504d7710a9504/`.
It contains raw CTest logs, Ninja build records, caches/compile commands,
installed-file hashes, input/dependency/protection audits, 1800 raw cost samples,
summary statistics, runners and public-header backups. `SHA256SUMS.json`
inventories 109 archived files at report creation.

## Inputs, dependencies and environment

Four supplied documents match the handoff SHA256 JSON. All three pasted commands
match the named implementation command after line-ending normalization.

| Input | SHA256 |
|---|---|
| Draft 7.3 | 8281afa0898e2d3de61eb218580653bc81ed24dea11c59d2badd2d5ec15b3485 |
| Plan v6 | 1f631366cb37efde03faa3876841e5082505aba7ea39879e5d3e6d2befab1820 |
| G5 command | 0768acd6770f4fa1a9e4f583c730e8491515463eab5cfc779fdde171176bfb18 |
| Document audit | 559d469a6fc98fea095e532c745dcb79142af873c25f0d1bcf17f939836c02f5 |

- lux-cxx: `bf779515a120350c7c5412c366eea73afdf59ccf`, clean sibling.
  All 64 headers of the isolated G3 dependency SDK byte-match sibling source.
- lux-cmake-toolset: `c104eb6dd30a45f5d8e76f24e8d6f8de064bdf60`, clean sibling.
- Windows x64, Intel Core i9-13900H, 14 cores / 20 logical processors.
  MSVC 19.44.35226, CMake 3.31.10, Ninja, C++20, `/GR-`, `/EHsc`.
- ASan: RelWithDebInfo plus `/fsanitize=address /Zi`;
  `ASAN_OPTIONS=alloc_dealloc_mismatch=1`.
- Linux, Android builds and TSan: NOT_RUN. ASan is not race detection.

## Changes, ownership and boundaries

I changes 14 files: 12 new Feature files and two existing files.

| Files | Actual change |
|---|---|
| Render CMakeLists | Adds the real render_features component/export |
| graph/include/.../Builder.hpp | Three read-only resource/pass/output counts; no analyzer change |
| features/CMakeLists, include/.../{Feature,Resources}.hpp, src/Assemble.cpp | Static component, public value contracts and one assembly algorithm |
| features/README.md | Frozen ownership, counterexample, semantics and migration limits |
| features/test/{CMakeLists,Feature.cpp,Scenarios.hpp,Cost.cpp,check_g5.py} | Ten behavior groups, fixtures, complete cold cost and protection checker |
| features/test/installed/{CMakeLists,Main.cpp} | Relocatable SDK consumer executing A/B/C |

No production types were deleted. No old tests/reports/checkers were rewritten.
Allowed changes are exactly the Feature module, Render component registration and
the three Builder accessors. Core/Data/Protocol/IPC production, Legacy, Engine,
Editor, MaterialGraph, old reports and user changes remain read-only.

FeatureDefinition/SelectedFeature borrow immutable metadata, code, config and
instance keys for one synchronous call. ResourceClaim owns its data.
StorageRequirement uses owning cxx::SchemaId; FeatureTypeIdView uses the existing
stable-name view. No runtime FeatureId, allocator, hash registry or manager exists.
ResolvedResource borrows canonical names only during build. FeatureAssembly owns
one G4 Definition, its one Analysis and copied external bindings, with no retained
callback/config pointers or Native owner.

Assembly validates exact Schema/Wire/Operation metadata, sorts complete type and
instance names, collects all claims, requires a unique provider and resolves
scope/type/ABI/origin/range compatibility before declaring shared resources once.
OPTIONAL absence yields no binding. Transient cannot carry persistent Storage.
Repeated instances reuse one Definition; distinct definitions cannot silently
claim the same type or Operation. No Handler or OperationId is allocated.

Shared initial content binds v0; graph-produced ports bind v1. Callbacks use
resolved versions, never a latest-version lookup. Shared multi-version/in-place
chains are rejected. Private scratch may use G4's explicit internal versions.
Length-prefixed type/instance/local names isolate private contributions.
Callback append intervals enforce resource, pass, output and fallback ownership.
This catches a REQUIRE callback writing a PROVIDE port even where G4 alone could
accept the resulting graph. Callbacks must append, not move/reset/finish/retain
the borrowed Builder; the API is not a sandbox for hostile plugin code.

The candidate calls G4 finish once and analyze once; failure publishes nothing.
There is no second editable graph, duplicated analyzer or long-lived index.
Expected/structured errors and fatal ordinary-heap OOM follow project conventions.

## Behavior results

| Group | Evidence / result |
|---|---|
| A: Deferred/Lighting/Tonemap | All six input permutations have identical Analysis and canonical keys; independently expected order, edges and GBuffer lifetime; five live passes, one culled pass, two outputs; alternate Lighting works, simultaneous providers fail |
| B: HZB/Culling | Distinct Previous/Current resources, View storage requirements, v0 read and v1 output, one live compute pass; static missing-history fallback works; foreign private fallback rejected |
| C: MaterialStorage/Mesh | Zero-pass provider, one imported buffer declaration, Global owned storage metadata, one Mesh pass reading v0 |
| Identity | Null/invalid definitions, empty names, duplicate instance/type definitions rejected; same type/two instances produce separate private resources and four live passes |
| Config/Operation | Explicit byte limits; exact schema/version/layout/type; truncation/trailing/illegal enum; external index/range/overflow/limit negatives; duplicate/conflicting Operation metadata rejected |
| Claims | Missing REQUIRE fails; missing OPTIONAL succeeds; duplicate claims/providers fail; buffer/image/size/format/sample/origin/initial/scope/type/ABI mismatch rejected |
| Authorship/failure | Unauthorized write/export/private use, wrong shared version and in-place writes rejected; describe/build/finish/analyze errors propagate; previous successful result unchanged |
| Lifetime | Temporary Definition/config/name and counted byte owner die after assembly; destructor count exactly one; returned graph/storage names remain usable after move |
| Cold/concurrent | Actual callback counters change only on assembly; four barrier-started threads perform 100 independent assemblies each using shared immutable definitions |
| G4 counters | Invalid append leaves counts unchanged; successful resource/pass/output append counted; successful finish empties candidate counts |

G4's original eight tests and all Core/Data/Protocol tests remain unchanged.
No Native/GPU algorithm is represented as implemented by these fixtures.

## Build, SDK and protection results

| Independent configuration | Full all build | Second build | CTest | Fresh relocated SDK |
|---|---|---|---|---|
| RelWithDebInfo | PASS, 47 build steps | no work | 34/34 | PASS |
| Debug | PASS, 47 build steps | no work | 34/34 | PASS |
| ASan RelWithDebInfo | PASS, 47 build steps | no work | 34/34, no ASan findings | PASS |

The 34 tests consist of the original 23 Render tests plus ten Feature groups and
one installed Feature test. That installed test configures/builds an external
consumer with three actual A/B/C executions and independent header compilation.
All-target builds also compile the unchanged IPC targets; IPC pressure/performance
tests are not rerun or requalified.

Common configuration, after the VS x64 developer environment:

```text
cmake -DLUX_SOURCE_DIR=<independent-source> -P <independent-source>/cmake/ValidateTrackedSnapshot.cmake
cmake -S <independent-source>/cmake/render-v2-bootstrap -B <build> -G Ninja
  -DCMAKE_BUILD_TYPE=<Debug|RelWithDebInfo> -DBUILD_TESTING=ON
  -Dlux-cmake-toolset_DIR=E:/SyncForder/CodeRepos/lux-cmake-toolset
  -Dlux-cxx_DIR=E:/RenderV2Rebuild/G3/dependencies/share/lux-cxx
  "-DCMAKE_CXX_FLAGS=/EHsc <ASan flags when applicable>"
  -DCMAKE_CXX_SCAN_FOR_MODULES=OFF
  -DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF -DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=OFF
  -DCMAKE_INSTALL_PREFIX=<build>/sdk
cmake --build <build> --target all -j 4 -- -k 0
cmake --build <build> --target all -j 4 -- -k 0
ctest --test-dir <build> --output-on-failure -j 1 -R "^render\."
py -3 <source>/modules/function/render/features/test/check_g5.py
  --repo <source> --build <build> --clean --user-worktree E:/SyncForder/CodeRepos/lux-engine
```

Exact executable paths and invocations are in the archived run/qualification
scripts and CMake caches. Builds and runtime tests were serialized. PowerShell
transcripts retained commands and exit status but omitted some native stdout:
raw CTest LastTest logs and Ninja records are archived directly; no-work builds
and root configure were additionally captured with Tee-Object.

Source includes, compile commands, link properties and installed consumer commands
confirm Feature -> Graph/Data/Protocol, Graph -> Core. Feature public headers do
not expose IPC, Vulkan, Native, Runtime, ECS, Editor, MaterialGraph or stdexec.
Installed consumer compiles use relocated/include, not source public include trees.
Existing Core-only and Graph-only consumers still pass without importing Feature.

Protection: **719/719 Legacy blobs** match the frozen tree, HEAD and working
content. **3013 baseline protected files** match their Git blobs. Historical
reports stored as literal CRLF are checked against their exact raw Git blob when
autocrlf normalization would otherwise produce a false mismatch.

User file hashes before and after verification:

- settings.json: `f51fd038282aba9d31a4ad9ffb64f1e8bc3b525b4f6ca4c9ca3c2f306e64a57c`
- user Graph.hpp: `00c28abc7bd0a786666f101a5212400a5b467af6bdbbc5049c69d59d548c726a`

All 11 Render public headers from exact I were synchronized to Debug/include,
RelWithDebInfo/include and Android/lux-engine/include: **33 copies**, with original
backups, before/after SHA256 and source paths in header-sync.json. User's modified
Graph.hpp was not used. Android header sync is not Android build qualification.

## Cold assembly measurements

1800 raw samples: 18 combinations, five discarded warmups and 100 retained
observations per combination. Whole assembleFeatures is timed, including config
validation, typed callback decoding, claim resolution, all build callbacks,
authorship validation and G4 analysis. Input encoding/setup and printing are
excluded; result destruction is measured separately. Allocation instrumentation
covers the executable and linked static libraries during that same call.

Times below are microseconds, nearest-rank percentiles. Bytes are cumulative
allocation requests, not peak live memory or GPU footprint.

| Shape | Features / Passes | p50 | p95 | p99 | max | Allocations | Bytes |
|---|---:|---:|---:|---:|---:|---:|---:|
| Independent | 8 / 32 | 64.5 | 97.8 | 163.7 | 172.4 | 529 | 86016 |
| Independent | 8 / 128 | 219.1 | 284.2 | 328.9 | 357.2 | 1676 | 333180 |
| Independent | 8 / 512 | 984.4 | 1621.1 | 1801.8 | 2471.7 | 6000 | 1583669 |
| Independent | 32 / 32 | 66.1 | 128.7 | 216.2 | 218.9 | 553 | 81063 |
| Independent | 32 / 128 | 164.9 | 388.8 | 490.8 | 595.6 | 1828 | 312035 |
| Independent | 32 / 512 | 828.2 | 1326.6 | 1502.5 | 1872.4 | 6344 | 1503868 |
| Independent | 128 / 32 | 183.3 | 243.5 | 287.2 | 479.7 | 649 | 96423 |
| Independent | 128 / 128 | 319.7 | 556.9 | 612.3 | 613.1 | 1924 | 292067 |
| Independent | 128 / 512 | 757.2 | 1067.5 | 1742.9 | 1918.7 | 6952 | 1419132 |
| Chain | 8 / 32 | 92.4 | 100.5 | 104.6 | 113.1 | 607 | 98412 |
| Chain | 8 / 128 | 217.8 | 399.0 | 437.9 | 498.1 | 1932 | 351712 |
| Chain | 8 / 512 | 873.7 | 1202.3 | 1339.2 | 1453.8 | 7024 | 1659776 |
| Chain | 32 / 32 | 66.5 | 73.3 | 78.1 | 88.7 | 680 | 98147 |
| Chain | 32 / 128 | 184.0 | 325.5 | 476.2 | 531.8 | 2147 | 368167 |
| Chain | 32 / 512 | 912.5 | 1292.6 | 1626.8 | 1894.7 | 7368 | 1579975 |
| Chain | 128 / 32 | 171.2 | 281.7 | 306.1 | 315.0 | 776 | 113507 |
| Chain | 128 / 128 | 307.6 | 417.9 | 478.9 | 551.3 | 2436 | 365806 |
| Chain | 128 / 512 | 994.3 | 1479.3 | 1559.4 | 1922.7 | 8230 | 1635447 |

Allocation totals were identical across the 100 samples in each case. Timing
variation is retained, not assigned a speculative cause or hidden by averaging.
This is cold CPU assembly, with ordinary allocations and no claimed fixed
throughput threshold, per-frame zero-allocation guarantee or GPU performance.

## Actual failures, limitations and next entry

- Required G5 correctness/build/install/protection gates: PASS. No production
  defects required source repair during exact-I verification.
- The pre-I protection checker initially normalized historical CRLF and falsely
  flagged unchanged files. It was corrected before I to verify literal Git blobs.
- The first independent protection attempt lacked the historical frozen tree;
  explicit object fetch resolved this environment prerequisite without source edits.
- Root TOOLCHAIN configure: exit 1 in both implementation and independent trees.
  Missing render/client, features/cmake/feature_client.cmake and
  render/standard_content cause subsequent nonexistent-target install errors.
  Root all/product build remains BLOCKED; no target was stubbed or test disabled.
- Full Native Storage/ResourcePlan/Shader ABI, Runtime Operation handlers,
  Client/Server and GPU execution: NOT_IMPLEMENTED, outside G5.
- Dynamic History invalidation/rotation after camera cut or resize, Native Usage/
  Stage, relative/3D/cube resources and cross-feature in-place multiwriter ports
  are not qualified. Static History fallback is not runtime History support.
- Initial ranges use exact canonical list compatibility; equivalent but differently
  partitioned coverage is not automatically unified. This is conservative rejection.
- G6's next entry is Native Vulkan device/context/queue/retirement foundation.
  G7 must use this one Definition/Analysis plus external bindings and real Shader/
  Device inputs; no alternate Graph is needed. No G6 work was started.

After this report-only V, publish I/V to codex/render-v2-rebuild with a normal
fast-forward push, preserve the original dirty checkout, and STOP for review.
