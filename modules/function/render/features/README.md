# G5: Feature contracts and single-graph assembly

## G5-0 frozen boundary

Baseline: f80902e100879b114e848cabe1469540aba4d25d. Authority: Draft 7.3
sections 4.7–4.11, Plan v6 and G5 command dated 2026-10-14.

FeatureDefinition is a borrowed immutable type/function table, never a Scene or
GPU owner. SelectedFeature borrows its definition, instance key and optional G2
WireView for one synchronous call. The caller keeps their modules and bytes alive.
ResourceClaim owns its strings and initial ranges. StorageRequirement owns its
SchemaId name, ABI and scope. ResolvedResource borrows assembly-local names only.
FeatureAssembly owns exactly one G4 Definition, its one Analysis and external
storage mappings; it retains no input/config/callback pointers.

Only features/**, Render component registration, necessary install tests, and
three readonly Builder counters are allowed. Core/Data/Protocol/IPC, Legacy 719
blobs, historical reports/checkers and user changes are protected. Native, GPU,
Client/Server, Registry, Handler, Shader and MaterialGraph work is out of scope.

The counter seam has a concrete consumer: if A PROVIDEs a resource but B REQUIREs
and writes it, G4 alone can accept the graph. Per-callback append intervals let
G5 reject B's unauthorized write without duplicating G4 analysis or introducing
another graph. resourceCount/passCount/outputCount do not expose mutable arrays.
Callbacks append only: they must not finish, replace, move or retain the Builder.

Assembly validates exact identities/config/operations; sorts complete type and
instance names; collects all claims; resolves unique providers; declares shared
resources once; invokes cold builders; finishes once; audits authorship; analyzes
once. Errors discard only the local candidate. Ordinary heap OOM remains fatal.

Same type/different instance uses the same Definition object. Duplicate operation
names across distinct definitions are rejected; repeated instances validate their
type metadata once. Config/schema are both present or both absent; callers supply
explicit WireLimits. Version and ABI comparisons are exact.

Shared Previous History and initialized Imported resources expose v0; other
shared outputs expose v1. Only PROVIDE may write that output. Cross-feature
in-place version chains are rejected. Private scratch may use G4's internal
multi-pass/version contracts. Initial range lists expand whole-resource syntax,
sort and deduplicate; differently partitioned coverage is not inferred equivalent.
Scope is Global/Scene/View/Attachment, never Transient. Transient has no Storage.

Pass/private resource names use featureKey(type, instance, local), with length
prefixes separating components. Shared resource names occupy a separate internal
namespace. Missing OPTIONAL bindings are absent; findResource returns null.

## Required examples and future boundary

A: independent Deferred, Lighting and Tonemap callbacks compose one graph;
alternate Lighting implementations are mutually exclusive cold selections.
B: View-scoped previous HZB and current output are distinct logical resources.
C: zero-pass MaterialStorage provides an initialized imported buffer to Mesh.
These are CPU authoring fixtures, not implemented GPU algorithms.

G4 Access is not Native Usage/Stage. Dynamic History after camera cuts/resize,
3D/cube/relative images, Shader ABI and ResourcePlan/Storage factories remain
G6–G9 work. Static initial content may cull a fallback; it is not runtime history
selection. G5 neither changes G4's historical PASS nor IPC's performance PARTIAL.

Legacy FeatureFactory's cold function table and resource-only Feature behavior
are retained as concepts. Its native-layout config memcpy, dynamic type/operation
allocation, FeatureCatalog/FeatureSet ownership and scene/view lifecycle are not
copied or linked. G2 Wire and pure Protocol descriptors are the only contracts.

## Verification and costs

Use the existing render-v2-bootstrap with the verified sibling lux-cxx SDK:

```text
cmake --build <build> --target all -j 4 -- -k 0
cmake --build <build> --target all -j 4 -- -k 0
ctest --test-dir <build> --output-on-failure -j 1 -R "^render\."
<build>/render/features/test/render_features_cost.exe > <external-evidence>/cost.csv
py -3 modules/function/render/features/test/check_g5.py --clean --build <build>
```

Ten behavior groups cover composition with independent order/edge/lifetime
expectations, identities/private versions, config Wire, Operation contracts,
claims, authorship/failure atomicity, static History fallback, input lifetime,
parallel independent assemblies and Builder counters. Installed consumers run
A/B/C through a fresh relocated SDK. Core/Data/Protocol/G4 tests are unchanged.

Cost samples cover the Cartesian product of 8/32/128 Features and 32/128/512
Passes, for independent and chained graphs (18 cases). Each case discards five
warmups and keeps 100 observations. One measured call includes config validation,
typed callback decoding, all claim/ownership work, construction and G4 analysis.
Config encoding and input setup are outside timing; result destruction has its
own column. Allocation calls/bytes cover the static libraries and executable
while assembly is measured, not retained/peak GPU memory. There is no throughput
threshold, per-frame renderer measurement or claim of zero cold allocations.

Independent qualification must use an exact clean tracked I, snapshot validation,
Debug/RelWithDebInfo/ASan, all targets, second no-work builds, relocated SDK and
the G5 checker. Root TOOLCHAIN closure is separate; no missing future target is
stubbed. GPU/Validation/SyncValidation are N/A, unrun platforms/TSan NOT_RUN.
