# I4 independent checkpoint — BLOCKED, not final convergence qualification

Implementation: `6bf9cbe037313c38193dee69183551bcb7b4e7fb`.
Parent: `9e238eafcf10d57af9562ab23b65ed8c85b936e9`.
Remote rechecked during verification: `9184c9f71c63af7f686822c76b89263b29ef7a63`.
This report-only checkpoint does not complete I5–I7 and does not authorize a push or upgrade historical performance.

## Exact source and results

Independent `--no-hardlinks` clone: `E:/RenderV2Rebuild/IPCConvergence/i4-source`, detached at I above,
clean tracked files, no object alternates. ValidateTrackedSnapshot passed before configuration.
Fresh build roots: `E:/RenderV2Rebuild/IPCConvergence/i4-verify/{r,d,asan}`.

| Configuration | Full all build / immediate second build | CTest | Qualification |
|---|---|---|---|
| RelWithDebInfo | 495 steps / no work | 146/146, 254.57 s | PASS for this checkpoint |
| Debug | 495 steps / no work | 143/146, 515.93 s | FAIL; three baseline failures below |
| MSVC ASan, RelWithDebInfo | 495 steps / no work | 146/146, 332.44 s | PASS; no ASan diagnostic |

Commands are retained verbatim in the archived runners and logs. The shared runner selects MSVC amd64,
Ninja, Vulkan SDK 1.3.280.0, the frozen lux-cxx SDK and lux-cmake-toolset. It uses:

```powershell
cmake -DLUX_SOURCE_DIR=<exact-clone> -P <exact-clone>/cmake/ValidateTrackedSnapshot.cmake
cmake -S <exact-clone>/cmake/render-v2-bootstrap -B <fresh-build> -G Ninja `
  -DCMAKE_BUILD_TYPE=<configuration> -DBUILD_TESTING=ON `
  -Dlux-cmake-toolset_DIR=E:/SyncForder/CodeRepos/lux-cmake-toolset `
  -Dlux-cxx_DIR=E:/RenderV2Rebuild/G3/dependencies/share/lux-cxx `
  -Dstdexec_DIR=D:/DevTools/vcpkg/installed/x64-windows/share/stdexec `
  -Dstduuid_DIR=D:/DevTools/vcpkg/installed/x64-windows/share/stduuid `
  -Dtomlplusplus_DIR=D:/DevTools/vcpkg/installed/x64-windows/share/tomlplusplus `
  -DCMAKE_CXX_SCAN_FOR_MODULES=OFF -DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF `
  -DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=OFF -DCMAKE_INSTALL_PREFIX=<fresh-build>/sdk
cmake --build <fresh-build> --target all -j 4 -- -k 0
cmake --build <fresh-build> --target all -j 4 -- -k 0
ctest --test-dir <fresh-build> -V --output-on-failure -j 1 -R .
```

The runner adds `/EHsc`; ASan additionally uses `/fsanitize=address /Zi` and
`ASAN_OPTIONS=alloc_dealloc_mismatch=1`. Builds and execution were serial. All tests remained enabled.
Public-header compilation and relocated SDK consumers ran as part of this matrix. The three shared SDK
prefixes have not been updated for I4: a failed complete gate is not a finished SDK release.

## Debug blockers and baseline reproduction

1. `object.retirement_contract`: expected-abort child `--reject-held` exceeds its 30 s subprocess timeout.
2. `object.target_contract`: expected-abort child `--destroy` exceeds its 30 s subprocess timeout.
3. `graph.edit`: MSVC Debug span assertion, `cannot compare incompatible span iterators for equality`.
   The diagnostic dialog was captured, then that exact test process was terminated; this is a failed test,
   not a successful cancellation or a natural test completion.

All three reproduce using the original `9184c9f` Debug binaries in `baseline-build/d/ipc/bin`.
The two Object children display the Debug CRT `abort() has been called` dialog, preventing unattended
completion. Their original markers remain present. The Graph failure has a concrete source cause:
`GraphTopology.cpp:320` compares the iterator from `find(links.subspan(index + 1), link)` with `links.end()`.
Those are different Debug iterator ranges. Comparing with the same subspan's end is the proposed minimal fix.

The eight relevant source/test files have identical SHA-256 between baseline and I4; see
`debug-baseline-comparison.json`. Baseline diagnostics use a bounded 5 s observation and retain dialog
text, stdout and stderr. The first diagnostic script encountered a process-exit race during cleanup;
that failed attempt is preserved and the remaining target was reproduced separately. No production source
or verification clone was modified. These failures are preexisting, but are not waived.

Core Object and generic function/graph are outside this task's recorded change boundary. The pending user
question proposes only the same-span fix and test-only CRT stderr routing for expected aborts, preserving
the aborts, exit-code assertions and timeouts. I5 production work is held until this gate is resolved.

## Observed behavior and limits

I4 changes 101 files (92 modified, 3 added, 6 deleted), +2186/-4396 lines. Exact paths are in
`independent/change-list.json`; ownership and superseded contracts are in `I4_MIGRATION.md`.
Client sends/receives actual IpcMessage; caller owns RequestId. There is one accepted source owner and
one actual response retained on failed reply growth. Retrying delivery does not invoke the Handler again.
True asynchronous requests use the existing stable slots and their own ready links; synchronous requests
do not reserve a deferred slot. Remaining Native bootstrap/Factory/pin cleanup is I5, not claimed complete.

The tested GPU paths retain actual uploads, three Queue modes, partial submit failure, LastUse,
Mesh/Material/Instance, camera/depth, readback, two-Client source isolation, close/stop and four-View oracles.
Validation/SyncValidation errors are zero in those reported cases. Existing warnings, including the
duplicate EOS overlay layer warning, remain in the raw logs. Release four-View hot measurement:
580 ticks, 2318 submissions, first-party new=0. External Storage hot measurement also reports new=0.

Measured Release sizes: IpcMessage 128 B, RenderRequest 320 B, DeferredCompletion 688 B,
SubmissionNode 48 B, CommandInvocation 24 B, Buffer permit 16 B. IPC backlog proof uses 257 messages,
130 segments at configured segment size 4, followed by 20 reuse cycles without new segment allocation.

Single-run small Command sample count is 1000. CPU p95: construct/submit 300 ns, Handler/reply 2900 ns,
successful typed consume 300 ns; wait-and-consume 2.0347 ms (includes scheduling/wait). Measured new calls
are zero in this narrow prewarmed workload, not in all asset construction or queue growth.
GPU-ready inbound-to-consume p95 is 31.4676 ms dedicated, 46.9248 ms default, 46.8016 ms same-family.
Observation-to-publication tails remain unexplained. These are unpaired qualification samples, not proof
of improvement or equivalence. All raw outliers are retained; GPU timestamp is NOT_MEASURED.

Protection passes: 719 frozen Legacy blobs, four protected user files, dependency commits and historical
files. lux-cxx is `bf779515a120350c7c5412c366eea73afdf59ccf`; lux-cmake-toolset is
`c104eb6dd30a45f5d8e76f24e8d6f8de064bdf60`. Original user worktrees were not changed.
Root TOOLCHAIN remains historically BLOCKED and was not requalified here. Linux/Android/TSan NOT_RUN.
Historical IPC/Native/Alias performance remains PARTIAL. No push was performed.

## Immutable evidence

Archive: `E:/RenderV2Rebuild/IPCConvergence/archives/6bf9cbe037313c38193dee69183551bcb7b4e7fb/i4-independent-checkpoint.zip`

SHA-256: `b6f5873fac022afe60d3b05ff42bde5ef84382a6fbf90c2690659d9bbb8cb70d`.
Size: 14,687,360 bytes; 252 source evidence files plus the archive's `SHA256SUMS.json`.
Includes failed implementation attempts, exact independent build/test logs, samples, baseline diagnostics,
protection results, CMake caches and runners. This archive is an I4 checkpoint, not final I7 Verification V.
