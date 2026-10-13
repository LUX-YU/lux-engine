# Draft4 G2 independent verification

**G2: PASS. Full product TOOLCHAIN closure: BLOCKED. G3: NOT_IMPLEMENTED.**

Verification date: 2026-10-13. Scope is the approved G2 Schema/Wire/owned-bytes plan.
GPU and SyncValidation are N/A, not PASS. Linux and Android builds were not run.

## Exact Git identity and independence

| Item | SHA |
|---|---|
| Implementation I | `cc8a7615c71fc077d50e2ec8559391814a713fd5` |
| I parent / accepted G1 V | `9526b5efd61891b96cd8594122243db994f69729` |
| Remote codex/render-v2 observed | `c221b93d1a8eccf690c5e4e0b848150f3634fd8b` |
| Frozen Legacy source | `a669409a289a6fa4092f21176397795b1cdb7f3e` |

V is the report-only commit containing this file; its parent is exact I. Its final
SHA is recorded in the delivery message and external delivery.json, avoiding a
self-referential report hash. Work remains on `codex/render-v2-rebuild`; no push,
merge, main change, or G3 implementation was performed.

Independent source: `E:/RenderV2Rebuild/G2/verify/source`, cloned with
`--no-hardlinks --single-branch`, then detached at exact I. Frozen Legacy objects
were fetched by exact SHA from the local repository. Git emitted its shallow-root
ref warning; object/tree access and all 719 blob comparisons succeeded. HEAD did
not change. ValidateTrackedSnapshot.cmake passed before configuring qualification.

All **2,964 tracked source files** retained their raw SHA256 across normal, ASan,
installed-consumer and root configuration checks. The verification checkout was
clean before and after. No implementation repair was made in that checkout.

## Results

| Gate | Actual result |
|---|---|
| Windows C++20 / no RTTI, Core + Data all targets | PASS, 10 translation units / 14 Ninja actions |
| Second all build | PASS, `ninja: no work to do` |
| Normal CTest | PASS, 14/14, 4.36 seconds |
| AddressSanitizer CTest | PASS, 14/14, 4.82 seconds |
| Existing G1 regression | PASS, all original 7 cases retained unchanged |
| New G2 behavior and installation | PASS, 7 cases |
| Public header isolation | PASS, 3 Core + 2 Data source header TUs and installed equivalents |
| Relocated installed consumers | PASS, Core 1/1 and Data 1/1 under both normal and ASan |
| Fresh installed header closure | PASS, exactly 8 canonical headers, byte-equal to qualified source |
| Production and installed dependency boundary | PASS, no old Render SDK, Vulkan, Legacy, Runtime, Transport, ECS or Editor |
| Legacy protection | PASS, 719 frozen/HEAD/worktree blobs plus unchanged CMake guard |
| Historical reports and G1 Core | PASS, no changes |
| User settings protection | PASS, bytes and original binary patch unchanged |
| Public header prefix sync | PASS, 2 Data headers x 3 prefixes; all 6 destinations previously absent |
| Root TOOLCHAIN configuration | BLOCKED, actual missing subsequent-stage inputs; exit 1 |
| Root product build / CTest, Editor / Player | NOT-RUN because root configuration failed |
| GPU / Vulkan Validation / SyncValidation | N/A to G2 |

ASan used `/fsanitize=address /Zi` and `ASAN_OPTIONS=alloc_dealloc_mismatch=1`.
No sanitizer suppression was used. Both installed consumers inherited these flags
in the ASan run. Tests use checks active with NDEBUG, not assert-only validation.

## Behavior established

- Schema comparison checks full canonical identity, exact Schema/Wire versions,
  field order/name/kind/count, enum domains, size and alignment. Invalid descriptors
  fail, including unknown scalar encodings, duplicate fields and invalid domains.
- Fixed-record roundtrip has an exact little-endian golden. Unaligned input is
  accepted without typed dereference. Canonical negative zero and NaN encoding are
  verified; invalid boolean, enum and noncanonical floating encodings are rejected.
  Every supported scalar kind is exercised by the common validator.
- Every truncation of the fixed record, extra tail bytes and caller byte-limit
  violations fail. A mismatch fails before the typed decoder runs. Explicit faulty
  codec fixtures verify size, incomplete/overflow writes, incomplete reads and
  recoverable callback errors do not publish a result.
- Blob and external references reject invalid attachment indexes, full-range
  overflow, silent-clamp candidates, incorrect relative alignment and excess size.
  Inline arrays exercise zero alignment padding and complete tail accounting.
  Empty legal ranges are accepted; references into a moved-from nonempty owner fail.
- An actual counted backing owner survives original source destruction, packet
  destruction, copying/moving, retained subranges and cross-thread consumption.
  The final release destroys it exactly once. Owned decoded binary data outlives
  its packet/attachment input collection.
- Compile-time checks reject pointers, spans, string_views and vectors as default
  WireData. There is no raw memcpy fallback; explicit codec specializations remain
  trusted C++ code rather than a sandbox for arbitrary callbacks.

## Ownership and module review

DataTypeId/View directly use lux-cxx SchemaId/View. They are semantic names, not
new runtime identity allocators. Scene/View/Target/Effect SlotKey generation remains
the unchanged G1 contract. No hash container, Registry, Manager or duplicate ID
authority was introduced.

DataSchema<T> owns no runtime state; its descriptor metadata is static. WireView
borrows for one synchronous call. SharedBytes is the sole byte-owner mechanism;
resolved ranges share its owner. External source storage must remain immutable and
stable. G2 does not claim to verify arbitrary pointers passed to fromOwner.

The exact public target dependency list is checked at configure time for source
and installed consumers:

```text
render_data -> lux::engine::render::render_core
            -> lux::cxx::core
            -> lux::cxx::binary
            -> lux::cxx::memory
```

Core retains its prior container/compile_time/algorithm dependencies. lux-cxx source
is `bf779515a120350c7c5412c366eea73afdf59ccf`; toolset source is
`c104eb6dd30a45f5d8e76f24e8d6f8de064bdf60`. Both sibling worktrees were clean.
The isolated dependency prefix has 51 source-matching lux-cxx headers and relocated
existing package metadata; it has no old lux/engine headers. Actual compile commands
and installed interfaces were inspected, not only declared dependency prose.

## Reproduction commands and evidence

`E:/RenderV2Rebuild/G2/run.ps1` loads the VS 2022 amd64 developer shell, CMake
3.31.10 and Ninja, selects the isolated dependency prefix and captures its exit
status. Qualification invocations were:

```powershell
$source = 'E:/RenderV2Rebuild/G2/verify/source'
$build = 'E:/RenderV2Rebuild/G2/verify/build'
$asan = 'E:/RenderV2Rebuild/G2/verify/asan'
& E:/RenderV2Rebuild/G2/run.ps1 -Source $source -Action snapshot
& E:/RenderV2Rebuild/G2/run.ps1 -Source $source -Build $build -Action configure
& E:/RenderV2Rebuild/G2/run.ps1 -Source $source -Build $build -Action build
& E:/RenderV2Rebuild/G2/run.ps1 -Source $source -Build $build -Action build
& E:/RenderV2Rebuild/G2/run.ps1 -Source $source -Build $build -Action test
& E:/RenderV2Rebuild/G2/run.ps1 -Source $source -Build $asan -Action configure -ExtraFlags '/fsanitize=address /Zi'
& E:/RenderV2Rebuild/G2/run.ps1 -Source $source -Build $asan -Action build
& E:/RenderV2Rebuild/G2/run.ps1 -Source $source -Build $asan -Action test
```

Configure uses `-S <source>/cmake/render-v2-bootstrap -G Ninja`,
`-DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_TESTING=ON`,
`-Dlux-cxx_DIR=E:/RenderV2Rebuild/G2/dependencies/share/lux-cxx`,
`-Dlux-cmake-toolset_DIR=E:/SyncForder/CodeRepos/lux-cmake-toolset`, `/EHsc`,
package registries OFF and CMAKE_CXX_SCAN_FOR_MODULES=OFF. Build expands to
`cmake --build <build> --target all -j 4 -- -k 0`; test expands to
`ctest --test-dir <build> --output-on-failure -j 1`.

`check_g2.py --root <source> --clean --build <build> --installed <relocated-prefix>
--user-root E:/SyncForder/CodeRepos/lux-engine --snapshot E:/RenderV2Rebuild/G1/evidence/g0`
passed for all four fresh installed prefixes. User settings SHA256 remains
`f51fd038282aba9d31a4ad9ffb64f1e8bc3b525b4f6ca4c9ca3c2f306e64a57c`.

Evidence root: `E:/RenderV2Rebuild/G2/evidence/`:

- `verify-snapshot.log`, `verify-configure.log`, `verify-build.log`,
  `verify-no-work.log`, `verify-test.log`, `verify-asan-*.log`: actual commands/results.
- `ctest-build.log`, `ctest-asan.log`: full nested install/consumer command output.
- `verification-build.json`, `verification-asan.json`: identities, protection,
  exact installed header sets and all prefix paths.
- `*-compile-commands.json`, `*-consumer-commands.json`, `*-ninja.log`:
  actual source/header compilation and dependency evidence.
- `verified-source-before.json`, `verification-summary.json`: all 2,964 file hashes
  and post-verification equality/cleanliness result.
- `dependency-headers.json`, `design-inputs.json`, `include-sync.json`: dependency,
  authoritative input and three-prefix public header hashes. Shared prefixes were
  synchronized from the qualified clone and were not used for module qualification.
- `verify-root-toolchain.log`: exact-I root configuration failure, preserved in full.

## Change boundary and remaining work

I adds 13 files and changes only the Render component entry among existing files:

```text
docs/render-v2/rebuild/G2_IMPLEMENTATION.md
docs/render-v2/rebuild/check_g2.py
modules/function/render/CMakeLists.txt
modules/function/render/data/CMakeLists.txt
modules/function/render/data/README.md
modules/function/render/data/include/lux/engine/render/data/Schema.hpp
modules/function/render/data/include/lux/engine/render/data/Wire.hpp
modules/function/render/data/src/Schema.cpp
modules/function/render/data/src/Wire.cpp
modules/function/render/data/test/CMakeLists.txt
modules/function/render/data/test/Data.cpp
modules/function/render/data/test/Fixtures.hpp
modules/function/render/data/test/installed/CMakeLists.txt
modules/function/render/data/test/installed/Main.cpp
```

V adds only this report. G1 Core, Legacy and previous reports are read-only; later
stage production changes, placeholder targets, disabled tests, pushes and merges
are forbidden in this delivery and were not performed.

Root TOOLCHAIN fails at missing `render/client`,
`render/features/cmake/feature_client.cmake`, and `render/standard_content`, followed
by nonexistent-target export errors. This is the reproduced G1 product-closure
blocker. Neither root production CMake nor consumers were altered to hide it.

No unresolved G2 implementation failure remains. Codegen, automatic version
migration, strings/general codecs, plugin lifetime pins, transport ownership/
admission/settlement, aggregate byte budgets and GPU integration remain absent.
The next G3 entry is binding actual compatible descriptors and retaining packet
plus attachment owners through transport completion. That is a recommendation
only: stop here for user review.
