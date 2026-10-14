# G4 independent verification — 2026-10-14

**G4 module qualification: PASS. Root TOOLCHAIN/product closure: BLOCKED.**
No G5 work started. No Native/GPU or renderer performance qualification is implied.
IPC-R2's historical performance PARTIAL remains unchanged.

## Commits, inputs and independent environment

- Branch: `codex/render-v2-rebuild`.
- Actual fetched origin HEAD / construction baseline / I parent:
  `4d0d6378558174622d8a9a7d6aca63fb1d608284`.
- Implementation I: `57c1264867aca765124bc1f1f9567ed8e34c613e`.
- V parent: exactly I. V is the report-only commit adding this file; its full SHA
  is supplied in the delivery message and external delivery receipt. A commit
  cannot embed its own content-dependent hash.
- Independent source: `E:/RenderV2Rebuild/G4/verify/source`, detached at exact I.
  Created using `git clone --no-local --depth 2 --single-branch --branch
  codex/render-v2-rebuild file:///E:/SyncForder/CodeRepos/lux-engine ...`.
  No alternates/shared object store; no user changes copied. Source remained clean
  before and after verification. No implementation repairs occurred in this clone.
- `cmake -DLUX_SOURCE_DIR=<independent-source> -P
  <independent-source>/cmake/ValidateTrackedSnapshot.cmake`: PASS before configuring.
- lux-cxx: `bf779515a120350c7c5412c366eea73afdf59ccf`, clean sibling repository;
  all 64 headers in the isolated dependency SDK byte-match their source files.
- lux-cmake-toolset: `c104eb6dd30a45f5d8e76f24e8d6f8de064bdf60`.
- Windows x64, Intel Core i9-13900H (14 cores / 20 logical processors), MSVC
  19.44.35226, CMake 3.31.10, Ninja, C++20, `/GR-`, `/EHsc`.

All four supplied documents matched the supplied SHA256 manifest. The four
pasted attachments had identical content to the command (line ending differences).

| Input | SHA256 |
|---|---|
| G4 command | `4052139e193b222787bead0f43535ae34c74578288a3ca772d7cbfbd0669e3c4` |
| Draft 7.1 | `08dfe707fc6853869e1bc7fae950df786478e598f2f0a3c0692234f9fc2fee04` |
| Plan v5 | `d4a7f542ca23b12ee39ad52fd34a1cb348ba5e2a037b15688cff12e4c4b06362` |
| Consistency audit | `484eb45f5a852465aee6f3c023a42260a08df1fbc50c383aaa1ee153a1857c35` |

## Change boundary and ownership

I adds 14 files under `modules/function/render/graph/` and modifies only the Render
component registration outside that directory. No deleted files/types or aliases.

| Files | Purpose |
|---|---|
| `../CMakeLists.txt`, `CMakeLists.txt` | Real `render_graph` static component and relocatable export |
| `include/lux/engine/render/graph/{Graph,Builder,Analysis}.hpp` | Owned values, author builder, derived analysis |
| `src/{Builder.cpp,Analysis.cpp,Range.hpp}` | Validation, one logical analyzer, shared private range algorithms |
| `test/{Graph.cpp,Cost.cpp,CMakeLists.txt,check_g4.py}` | Durable semantic tests, cold cost samples, protection audit |
| `test/installed/{CMakeLists.txt,Main.cpp}` | Graph-only external SDK consumer and independent header probes |
| `README.md` | G4-0 ownership contract, API limits and Legacy algorithm migration notes |

Allowed: these Graph files and Render component registration. Read-only: Core,
Data, Protocol, IPC, historical reports/checkers, all Legacy and user settings.
Forbidden/not changed: Native, Feature SDK, Client/Server, Engine/Editor, Shader
toolchains, IPC lifecycle/SPSC implementation, fake build targets.

Builder uniquely owns candidate arrays; `finish()` transfers an independently
owned Definition with const accessors. Analysis owns only derived value arrays and
retains no Definition/Builder pointers. Resources, passes, accesses, outputs,
dependencies, versions, hazards and lifetimes are values. No Manager, registry,
virtual pass hierarchy, callback ownership or alternate graph authority exists.

Pass/Resource IDs are range-checked graph-local strong indices. Equal values from
different graphs are **not provenance-checked**; ownership must accompany usage.
They are not Server SlotKeys/Native handles. Versions are explicit author numbers,
not generated runtime identities. Version 0 denotes declared initial content;
positive versions are writes. Unwritten parts are not implicitly inherited.
Duplicate complete keys fail at finish; duplicate identical dependency edges are
canonicalized. Forward resource names/placeholders are unsupported and unknown
IDs fail immediately.

## Behavioral evidence

Seven Graph groups run the actual production builder/analyzer. Checks remain active
under NDEBUG and are not assertions compiled away in the release configuration.

| Group | Positive oracle and rejection evidence |
|---|---|
| authoring | Two author chunks share one resource; moved Definition survives caller/builder storage; duplicate resource/pass names, unknown/sentinel IDs, self dependencies and overlapping outputs reject |
| ranges | Byte interval join from disjoint writers; depth/stencil/mip/layer partitions; no false edges for disjoint ranges; zero, overflow, wrong resource kind, bad aspects/mips/layers/format/access and invalid import range reject |
| versions | One writer/two readers; explicit v1→v2 writes with RAW/WAR/WAW; READ_WRITE and attachment LOAD consume prior content; missing/partial/ambiguous producers reject with pass/resource diagnostics |
| topology | All six writer/reader/overwrite declaration permutations preserve semantic order; explicit/data edge union; self/two/three-node cycles reject; reported cycle member excludes an innocent downstream node; output/side-effect roots and dead semantic branch culling |
| conditional_history | Conditional v2 retains explicit v1 fallback; missing fallback or its producer rejects; intervening overwrite creates a rejected cycle; imported ranges need complete initialization; previous-history valid/fallback source selection and current-history output remain separate; previous writes reject |
| lifetimes | Declared reader-before-writer executes writer-before-reader; positions `[0,1]` vs independent exported resource `[2,3]`; subranges have different final reads, exports extend to N, dead resources have no lifetime; exporting overwritten old contents rejects |
| oracle | 160 deterministic tiny-buffer producer/coverage cases; 36 interval cases for exact RAW/WAR/WAW overlap; nine depth/stencil mip/layer lifetime cell oracles independent of production range helpers |

Repeated analysis of the same input compares the entire derived result (order,
edges, hazards, version uses and lifetimes); repeated negative analysis compares
the complete RenderError. Lifetime values are final live **positions**, never
declaration indices. All versions share their resource's physical identity: future
Native assignment must union their requirements, not alias individual versions
without a separate Native proof. Previous/current history origins never masquerade
as transient storage.

Static errors distinguish unknown pass/resource, invalid descriptor/range,
duplicate key/output conflict, undefined read, ambiguous producer, cycle, missing
conditional fallback and missing imported/history initialization. Fallback means
preserving initialized same-resource content for a conditional write; history
reads may explicitly choose a descriptor-compatible alternate resource/version.
There is no hidden fallback copy or runtime predicate executor.

Legacy review retained resource-use indexing and deterministic Kahn ordering as
algorithmic ideas. G4 deliberately replaces declaration-order last-writer inference,
whole-resource hazard approximation, silent first-name-wins declarations, unresolved
forward-reference pruning and original-pass-index lifetimes. Legacy Native binding,
barrier, render-pass and submission programs were not copied, edited or linked.

## Builds, SDK and memory safety

For each configuration, configure the independent source's
`cmake/render-v2-bootstrap` with:

```text
cmake -S <source>/cmake/render-v2-bootstrap -B <build> -G Ninja
  -DCMAKE_BUILD_TYPE=<Debug|RelWithDebInfo> -DBUILD_TESTING=ON
  -Dlux-cmake-toolset_DIR=E:/SyncForder/CodeRepos/lux-cmake-toolset
  -Dlux-cxx_DIR=E:/RenderV2Rebuild/G3/dependencies/share/lux-cxx
  -DCMAKE_CXX_FLAGS="/EHsc <ASan flags when applicable>"
  -DCMAKE_CXX_SCAN_FOR_MODULES=OFF
  -DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF
  -DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=OFF
  -DCMAKE_INSTALL_PREFIX=<build>/sdk
cmake --build <build> --target all -j 4 -- -k 0
cmake --build <build> --target all -j 4 -- -k 0
ctest --test-dir <build> --output-on-failure -j 1 -R "^render\."
py -3 <source>/modules/function/render/graph/test/check_g4.py
  --repo <source> --clean --build <build>
```

The actual fully expanded commands and VS environment initialization are retained
in `run.ps1` and transcripts. ASan uses RelWithDebInfo plus `/fsanitize=address /Zi`
and `ASAN_OPTIONS=alloc_dealloc_mismatch=1`.

| Independent configuration | Full all build | Second build | CTest | Address errors |
|---|---|---|---|---|
| RelWithDebInfo `/O2 /DNDEBUG` | PASS | no work to do | 23/23 | N/A |
| Debug | PASS | no work to do | 23/23 | N/A |
| ASan | PASS | no work to do | 23/23 | 0 reported |

Each 23-test selection is Core 7 + Data 7 + Protocol 1 + Graph 8 (including SDK).
All bootstrap targets were built; unrelated IPC stress/performance tests were not
rerun and were not disabled or deleted. No ASan claim of race detection or universal
leak detection is made.

All three Graph headers compile alone in each build and again after installation.
Every configuration installs into a **new** prefix, renames the prefix, then
configures/builds/runs an external consumer: Core/Data/Protocol regressions and the
Graph-only consumer all pass. Graph consumer finds only `render_graph`/Core plus
required lux-cxx targets; it asserts no Data/Protocol/IPC target is imported.
Its four compile units (three header probes + Main) use the relocated SDK.

Public dependency DAG: `render_graph → render_core → lux-cxx
container/compile_time/algorithm`, plus Core's canonical error/image value headers.
Source include audit, generated target links, Graph compile commands and installed
consumer compile commands show no Vulkan/VMA/Native/Legacy/IPC/Protocol/Data,
ECS/Editor or stdexec dependency. The bootstrap SDK contains no Native SDK.

Three installed header prefixes were synchronized to exact I's verified headers:
Debug, RelWithDebInfo and Android/lux-engine. The three Graph headers were absent
before this task, so no original user files were overwritten; intermediate
pre-verification copies were backed up. `header-sync.json` and
`header-sync-verified.json` record before/after SHA256, including clone checkout
line-ending normalization. Android header synchronization is not an Android build.

## Cold analysis cost (measured, no latency SLA)

40 samples per shape/size, three discarded warmups; three independent process runs.
Timing surrounds `analyze()` only. Builder/Definition creation, result destruction
and printing are excluded. Allocator counting covers the test EXE and statically
linked Graph code, including analysis temporaries and returned arrays; bytes are
cumulative allocated bytes, **not peak resident memory**. No core pinning or
frequency lock was imposed. p50 is median; p95/p99 use nearest rank (with only 40
samples p99 is the maximum). This is not per-frame rendering or a throughput SLA.

First independent run (`verification-cost.csv`):

| Shape | P / R / uses / edges | p50 / p95 / p99 µs | Allocation calls / bytes |
|---|---|---|---|
| Independent | 32 / 32 / 32 / 0 | 13.75 / 15.0 / 20.3 | 340 / 32,144 |
| Independent | 128 / 128 / 128 / 0 | 47.4 / 57.4 / 60.7 | 1,216 / 118,772 |
| Independent | 512 / 512 / 512 / 0 | 306.3 / 355.4 / 437.5 | 4,688 / 527,528 |
| Chain | 32 / 32 / 63 / 31 | 19.7 / 26.7 / 139.9 | 382 / 40,908 |
| Chain | 128 / 128 / 255 / 127 | 68.95 / 76.7 / 109.2 | 1,357 / 148,262 |
| Chain | 512 / 512 / 1,023 / 511 | 336.0 / 376.7 / 387.7 | 5,217 / 675,165 |

Variability is retained rather than explained away: chain-32 has a 139.9 µs tail
sample in run 1; repeat p99 values are 25.8 and 21.4 µs. Chain-128 p50 across runs
is 68.95 / 137.25 / 107.2 µs. Chain-512 p50 is 336.0 / 318.7 / 264.5 µs. These runs
do not establish a root cause or latency bound. No speculative production tuning
was made and no historical IPC sample was reused. All 720 measured samples remain
available. Cold allocations are expected; no zero-allocation claim is made.
Dense overlapping graphs can emit quadratically many real hazards; these sparse
workloads do not certify dense-graph scaling or final Native compilation cost.

## Protection, actual failures and remaining limits

- Legacy: **719/719** frozen source blobs at
  `a669409a289a6fa4092f21176397795b1cdb7f3e` equal current committed and working
  Legacy blobs, both in implementation and independent verification.
- Baseline diff whitelist verifies Core/Data/Protocol/IPC, historical reports,
  Legacy guards and other tracked files unchanged. User `.vscode/settings.json`
  remains uncommitted with SHA256
  `f51fd038282aba9d31a4ad9ffb64f1e8bc3b525b4f6ca4c9ca3c2f306e64a57c`.
- An initial implementation build exposed a generated header probe named
  `Graph.cpp` shadowing the behavior test source in CMake. Fixed before I by
  generating probes under `headers/`; clean independent builds execute the actual
  behavior test. That failed build log is preserved. No verification-source repair.
- Root TOOLCHAIN configure was retried in implementation **and exact-I independent
  source**. Both fail on missing `render/client`,
  `render/features/cmake/feature_client.cmake`, `render/standard_content` and their
  install targets. Product closure remains **BLOCKED**; no stubs or hidden legacy
  dependencies were introduced.
- GPU, WSI, Vulkan Validation, SyncValidation, FrameLoop, Scoped Storage, Shader
  toolchain and renderer performance: **N/A / DEFERRED** for G4.
- Linux, Android and TSan: **NOT_RUN**. IPC historical performance stays PARTIAL.
- G4 has no known remaining correctness blocker from this verification. Unsupported
  forward-name resolution, automatic latest-version inference and predicate
  implication are explicit API limits, not silently accepted malformed graphs.
- G5 next entry: combine real Feature resource requirements with this Definition
  and Analysis. Full ResourcePlan, Storage ABI, Shader/Native binding, history
  rotation, allocation/alias proofs and GPU completion belong to later stages.
  **G5 NOT_STARTED; stop after I/V push and await review.**

## Evidence archive

Raw logs, exact input documents, compile commands, CTest outputs, installation
manifests, protection results, environment and all performance samples are outside
the source tree under:

`E:/RenderV2Rebuild/G4/qualification/57c1264867aca765124bc1f1f9567ed8e34c613e/`

The archive contains 86 evidence files and `SHA256SUMS.json`; manifest SHA256:
`89d4590324b1c58fd94019534f21c41e0ea1e9abfec8ef1ae65c2c309cfe9783`.
Build trees remain under `E:/RenderV2Rebuild/G4/verify/{release,debug,asan}`.
