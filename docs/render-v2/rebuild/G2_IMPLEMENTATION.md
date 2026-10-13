# Draft4 G2 Implementation

Baseline: `9526b5efd61891b96cd8594122243db994f69729` (G1 Verification), branch
`codex/render-v2-rebuild`. This stage implements only neutral Render Data. Draft4
and the 2026-10-13 Implementation Plan govern this stage. Historical RD documents
and verdicts remain unchanged.

## Implementation and ownership

- New static `render_data` target, two installed headers (`Schema.hpp`, `Wire.hpp`),
  two implementation files (`Schema.cpp`, `Wire.cpp`). Render's component export
  adds render_data; Core production, tests and its historical audit are unchanged.
- DataTypeId / DataTypeIdView directly alias lux-cxx SchemaId / SchemaIdView.
  Schema versions use lux-cxx SchemaVersion but compatibility requires equality.
  Full names, fields, enum domains and layout are compared, not hash alone.
- DataSchemaDescriptor and DataField describe packed wire metadata. Static
  DataSchema<T> specializations implement explicit codecs; there is no raw object
  copy fallback or RTTI. Two test consumers exercise fixed and owned-binary values.
- BinaryReader/SpanWriter, ScalarKind, checked arithmetic and SharedBytes reuse
  lux-cxx. OwnedWireBytes is an alias. BlobRef and ExternalDataRef are byte ranges,
  not generational runtime identities and not secondary owners.
- WireView is a synchronous borrow. Resolved ranges retain the original SharedBytes
  owner; callers retain the external attachment collection. Inline blobs use an
  ordered, fully accounted tail with checked relative alignment and zero padding.
- The common validator rejects malformed schemas, mismatch, truncation, trailing
  bytes, invalid booleans/enums/floats, invalid ranges and caller limit violations
  before invoking typed decode. Incomplete codec reads/writes cannot publish values.
- Errors are static structured RenderError values. No log dependency, exception
  recovery, normal OOM error category, registry, manager or G3 envelope is introduced.

The detailed public contract and remaining boundaries are in the Data README.
Not implemented: Codegen, version migration, general string codecs, route binding,
admission, aggregate upload budgeting, plugin pins, Feature/GPU/Scope schemas.

## Allowed / read-only / forbidden

ALLOWED: `modules/function/render/data/**`, the Render component-entry CMake file,
this report and the new G2 audit/verification report.

READ-ONLY: G1 Core, all historical reports, shared canonical error/image headers,
lux-cxx and toolset source. Legacy's 719 source blobs and CMake guard remain exact.
User `.vscode/settings.json` is protected separately, never staged.

FORBIDDEN: transport/graph/runtime/native/features implementation, Legacy linkage,
placeholder targets, root product dependency workarounds, push and merge.

## Validation procedure

External workspace: `E:/RenderV2Rebuild/G2`. Windows MSVC 19.44.35226, Ninja,
CMake 3.31.10, C++20, RelWithDebInfo, no RTTI. Dependency commits:

- lux-cxx: `bf779515a120350c7c5412c366eea73afdf59ccf`
- lux-cmake-toolset: `c104eb6dd30a45f5d8e76f24e8d6f8de064bdf60`

Isolated dependency SDK contains 51 headers from those lux-cxx components, with
byte hashes recorded outside the repository. Installed component metadata is
relocated from the existing package to that isolated prefix; no old Render headers
are available to satisfy the module or consumer builds.

Configure the existing `cmake/render-v2-bootstrap` with BUILD_TESTING=ON,
RelWithDebInfo, the isolated lux-cxx_DIR, and CMAKE_CXX_FLAGS=/EHsc. Run:

```text
cmake --build <build> --target all -j 4 -- -k 0
cmake --build <build> --target all -j 4 -- -k 0
ctest --test-dir <build> --output-on-failure -j 1
```

Repeat from a fresh build with `/fsanitize=address /Zi` and
`ASAN_OPTIONS=alloc_dealloc_mismatch=1`. Tests include all 7 existing G1 cases and
7 G2 cases: schema, roundtrip, malformed, references, ownership, contracts, installed.
Installed tests build standalone consumers and individual public headers after
moving a newly installed SDK prefix. Assertions stay active under NDEBUG.

Before qualification run ValidateTrackedSnapshot.cmake. Verification must use an
independent clean clone at exact I, with no implementation edits. Recheck root
TOOLCHAIN separately and report its actual existing consumer dependency blockers.
Run check_g2.py with --clean, --installed and --build; preserve all logs and header
sync hashes outside the source tree. V records the actual final outcomes.

GPU/SyncValidation: N/A. Linux/Android builds: not run. Android public header sync
is required by the existing meta-gen installation convention and is not a build
qualification. Stop after G2 I/V; no G3 implementation or remote publication.
