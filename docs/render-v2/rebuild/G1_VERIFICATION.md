# Draft4 G0/G1 Independent Verification — PASS

G0 and G1 mandatory gates passed. This qualifies Core only. Whole-engine/TOOLCHAIN
products remain EXPECTED-BLOCKED on removed upper Render components. GPU is N/A.
G2 has not started. No push, PR or merge was performed.

## Commit and environment identity

| Item | Exact identity |
|---|---|
| Original local baseline | `246fe21eb6d060801b9a16ddcadde2be89dbbecc` |
| User cleanup commit | `36e53c4bbf775152789be729454251ee09b8f402` |
| Implementation I / parent | `079f7de8bf860d50600d580230677a6f883ad024` / `36e53c4bbf775152789be729454251ee09b8f402` |
| Verification V | The commit adding this report only; parent is exact I above |
| Branch | `codex/render-v2-rebuild` |
| Remote codex/render-v2, checked again after tests | `c221b93d1a8eccf690c5e4e0b848150f3634fd8b` (unchanged) |
| Independent source | `E:/RenderV2Rebuild/G1/verify/source`, detached at I, clean |
| Normal / ASan builds | `E:/RenderV2Rebuild/G1/verify/build` / `E:/RenderV2Rebuild/G1/verify/asan` |
| Compiler / generator | MSVC 19.44.35226, CMake 3.31.10, Ninja; Windows x64 |
| Language / configuration | C++20, extensions off, RelWithDebInfo/NDEBUG, `/GR- /we4541`, `/EHsc` |
| ASan | `/fsanitize=address /Zi`, `ASAN_OPTIONS=alloc_dealloc_mismatch=1`; no STL annotation suppression |
| lux-cxx | `bf779515a120350c7c5412c366eea73afdf59ccf`, sibling source unmodified |
| lux-cmake-toolset | `c104eb6dd30a45f5d8e76f24e8d6f8de064bdf60`, sibling source unmodified |

The verification clone was created with `git clone --no-hardlinks --single-branch
--branch codex/render-v2-rebuild E:/SyncForder/CodeRepos/lux-engine
E:/RenderV2Rebuild/G1/verify/source`, then detached at exact I. No implementation was
edited in that clone. ValidateTrackedSnapshot passed before configuration. Since the
user's original tree retains settings edits, it was not falsely presented as clean.

## Actual commands and results

The environment wrapper `E:/RenderV2Rebuild/G1/run.ps1` loads the existing VS x64 shell.
For normal qualification, SOURCE and BUILD below mean the independent paths above:

```text
cmake -DLUX_SOURCE_DIR=<SOURCE> -P <SOURCE>/cmake/ValidateTrackedSnapshot.cmake
cmake -S <SOURCE>/cmake/render-v2-bootstrap -B <BUILD> -G Ninja
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBUILD_TESTING=ON
  -Dlux-cmake-toolset_DIR=E:/SyncForder/CodeRepos/lux-cmake-toolset
  -Dlux-cxx_DIR=E:/RenderV2Rebuild/G1/dependencies/share/lux-cxx
  -DCMAKE_CXX_FLAGS=/EHsc -DCMAKE_CXX_SCAN_FOR_MODULES=OFF
  -DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF -DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=OFF
  -DCMAKE_INSTALL_PREFIX=<BUILD>/sdk
cmake --build <BUILD> --target all -j 4 -- -k 0
ctest --test-dir <BUILD> --output-on-failure -j 1
cmake --build <BUILD> --target all -j 4 -- -k 0
```

ASan uses the same clean I source and commands, its separate build directory, and
`-DCMAKE_CXX_FLAGS="/EHsc /fsanitize=address /Zi"`. Installed consumers receive the same
flags. No warning or sanitizer setting was weakened to obtain a pass.

| Gate / test group | Normal | ASan | Evidence meaning |
|---|---|---|---|
| Configure and `all -j 4 -- -k 0` | PASS | PASS | Core library, behavior executable, three independent public-header TUs |
| `render.core.ids` | PASS | PASS | All four IDs: null, erase/reuse, clear, stale key rejection |
| `render.core.scopes` | PASS | PASS | Scene-local View collisions, Scene recreation, documented independent-owner collisions |
| `render.core.exhaustion` | PASS | PASS | Actual lux-cxx small-generation exhaustion retires a slot without resurrecting an old key |
| `render.core.target` | PASS | PASS | Color-output descriptions, legal sample values, invalid extent/format/sample/kind, structured errors |
| `render.core.epochs` | PASS | PASS | In-place specification revision does not replace identity; object recycle does |
| `render.core.errors` | PASS | PASS | Static error metadata matches stable IDs and has stable storage |
| `render.core.installed` | PASS | PASS | Fresh install, relocation, independent headers and Core-only executable |
| CTest total | **7/7, 2.15 s** | **7/7, 2.35 s** | Includes installed-consumer gate; not 8 top-level tests |
| Nested installed consumer | 1/1 | 1/1 | Separately built executable exercises IDs, success/error result and linked descriptors |
| Second normal build | PASS | N/A | `ninja: no work to do.` |
| GPU / Vulkan / SyncValidation | N/A | N/A | No Native/GPU implementation exists in this stage |
| Linux / Android build | NOT-RUN | NOT-RUN | Not authorized requirements for this stage |

The source and installed header probes include only one public header per translation
unit, require C++20, and reject leaked Vulkan/Windows header macros. Tests use checks
that execute under NDEBUG. Test fixtures are local values, not production Owner stubs.

### Installation and dependency closure

`render.core.installed` invokes `cmake --install ... --prefix <fresh>/original`, renames
that prefix to `<fresh>/relocated`, then configures a new consumer with
`find_package(lux-engine-render CONFIG REQUIRED COMPONENTS render_core)` and links only
`lux::engine::render::render_core` with its actual lux-cxx dependencies. It builds `all`
and runs CTest. No source include path is supplied to the consumer.

Normal prefix: `verify/build/installed-check-3577b6dcaf1e/relocated`.
ASan prefix: `verify/asan/installed-check-929cfad1288c/relocated`.
Both are relative to `E:/RenderV2Rebuild/G1/`.

The package contains exactly these six headers, each byte-equal to the corresponding
clean-clone source, plus the static Core library and CMake package metadata:

```text
lux/engine/render/core/Ids.hpp
lux/engine/render/core/Error.hpp
lux/engine/render/core/Target.hpp
lux/engine/error/Error.hpp
lux/engine/error/ErrorDescriptor.hpp
lux/engine/description/Image.hpp
```

The last three come directly from unchanged canonical source. No ErrorRegistry, resource
library, Script, Native, Graph, Transport, ECS, Editor or UI target is linked. Core's
checked PUBLIC links are exactly `lux::cxx::container`, `lux::cxx::compile_time`, and
`lux::cxx::algorithm`. Compile-command inspection confirms that all four consumer TUs
use only the relocated include prefix and isolated dependency SDK, not the shared old
Engine installation or Legacy. Normal and ASan commands both carry C++20 and no RTTI.

The dependency SDK has no `lux/engine` headers. Its 30 lux-cxx header files were checked
byte-for-byte against the pinned sibling source and installed headers. Existing CMake
dependency metadata was relocated to this isolated prefix; dependency code was not
changed. Thus an old Render SDK could not satisfy missing G1 includes.

As required by AGENTS, the three new Core headers and their three canonical value-header
dependencies were synchronized to Debug, RelWithDebInfo and Android include prefixes:
18 files, all previously absent, with per-file SHA-256 records. Unrelated legacy SDK
headers were not deleted. Those shared prefixes are not qualification inputs; this does
not certify the old SDK or Android builds.

## Protection, file scope and ownership review

`check_g1.py` was run from the clean clone with `--base
36e53c4bbf775152789be729454251ee09b8f402 --clean --installed <prefix> --user-root
E:/SyncForder/CodeRepos/lux-engine --snapshot E:/RenderV2Rebuild/G1/evidence/g0` for both
installation prefixes.

- **719/719 Legacy files** match frozen source, HEAD and Git-filtered working blobs.
- The additional Legacy CMake guard is unchanged. Legacy is not built or linked.
- All 15 I files lie within the agreed G1 allowlist; the only Render subdirectory is Core.
- All **2,950 tracked files** in the verification clone retained their recorded raw
  SHA-256 values through the final checks, and `git status --porcelain` remained empty.
- User settings bytes and binary diff are unchanged. SHA-256 remains
  `f51fd038282aba9d31a4ad9ffb64f1e8bc3b525b4f6ca4c9ca3c2f306e64a57c`.
- Original workspace status after I: only ` M .vscode/settings.json`.
- No SlotKey wrapper, duplicate generation, runtime Owner, fake Client/Server/Compiler,
  Manager/Host/Port, identity hash map or G2 source was added.
- Compared with old APIs, IDs are now SceneId/ViewId/TargetId/EffectId without aliases
  for legacy RenderSceneId/RenderViewHandle. The Target API describes values only;
  there is no Runtime/Backend or Vulkan public-header compatibility path.

I file list:

```text
cmake/render-v2-bootstrap/CMakeLists.txt
docs/render-v2/rebuild/G1_IMPLEMENTATION.md
docs/render-v2/rebuild/check_g1.py
modules/function/render/CMakeLists.txt
modules/function/render/core/CMakeLists.txt
modules/function/render/core/README.md
modules/function/render/core/include/lux/engine/render/core/Error.hpp
modules/function/render/core/include/lux/engine/render/core/Ids.hpp
modules/function/render/core/include/lux/engine/render/core/Target.hpp
modules/function/render/core/src/Target.cpp
modules/function/render/core/test/CMakeLists.txt
modules/function/render/core/test/Core.cpp
modules/function/render/core/test/Installed.cmake
modules/function/render/core/test/installed/CMakeLists.txt
modules/function/render/core/test/installed/Main.cpp
```

## Actual failures, limitations and retained evidence

1. Root TOOLCHAIN configuration initially lacked a SPIR-V Tools library path. Retrying
   with the already installed SDK's exact library/include paths exposed the actual
   missing `render/client`, `render/features/cmake/feature_client.cmake`, and
   `render/standard_content` inputs, plus their nonexistent component exports. The
   same failure was reproduced from exact I in the independent clone (exit 1).
   No production CMake workaround, dummy target or disabled profile was introduced.
   Root build/CTest were not run because configure failed. Editor/Player builds are
   NOT-RUN; their known missing consumer dependencies remain in the implementation ledger.
2. The first independent Legacy audit could not access the historical frozen commit:
   the single-branch local clone did not include it. Fetching its exact objects from
   the original local repository left I and all files unchanged. Git warned about
   shallow-root ref updates; `cat-file`, `ls-tree` and all 719 blob comparisons then
   succeeded. The original failure is preserved in the evidence directory.
3. An external compile-command audit initially expected `/std:c++20`; CMake generated
   the equivalent MSVC spelling `-std:c++20`. The audit matcher was corrected to accept
   both spellings. No compiler flag, implementation, test or acceptance contract changed.
4. PowerShell transcript omitted native stdout in this tool runner. CTest's complete
   LastTest logs and Ninja logs were preserved, and root configure/no-work build output
   was captured with explicit full-stream redirection. No test failure was concealed.

No unresolved G0/G1 implementation failure remains. Core validation does not claim
physical-device support, cross-endpoint admission, render output or product integration.

Evidence root: `E:/RenderV2Rebuild/G1/evidence/`:

- `g0/`: input copies/hashes, cleanup list, user settings and binary patches.
- `implementation.json`: exact I, parent and changed files.
- `ctest-build.log`, `ctest-asan.log`: all behavior and nested install commands/results.
- `verification-build.json`, `verification-asan.json`: protection, boundary, installed headers.
- `consumer-*-compile-commands.json`, `build-_ninja_log`, `asan-_ninja_log`: compilation evidence.
- `verified-source-sha256.json`: all 2,950 verification source files.
- `dependency-headers.json`, `include-sync.json`: dependency and shared-prefix header identities.
- `root-toolchain-full.log`, `no-work-full.log`, `legacy-check-before-reference-fetch.log`,
  `audit-notes.txt`: expected blocking failure, no-work result and setup issues.

**Final scope result: G0 PASS; G1 PASS; full product EXPECTED-BLOCKED; G2 NOT_IMPLEMENTED.**
Stop here for user review.
