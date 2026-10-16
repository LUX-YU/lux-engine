# v12 N1 independent verification

Implementation: `cd6f093d31627aa5d894625dbf1d02a3eb4538fc`.
Parent: `fd00e5bcc5b42ee559f81836427bae1f3fa87b0a`.
Original production baseline and last observed remote: `78bcd220815f845466d85d31f994901e4db224ba`.
Verification source: `E:/RenderV2Rebuild/V12/n1-verification`, detached exact I, clean tracked snapshot,
created with `git clone --no-hardlinks --no-checkout`. No object alternates. No verification source fixes.
This commit adds only this report and its evidence index. N2/N3 remains a separate, unfinished gate.

## Qualification

| Gate | Result |
| --- | --- |
| RelWithDebInfo all / second no-work / tests | PASS, 104/104, 112.25 s |
| Debug all / second no-work / tests | PASS, 104/104, 113.42 s |
| MSVC ASan all / second no-work / tests | PASS, 104/104, 145.58 s |
| Synchronous source closure, stdexec and Vulkan lookup disabled | PASS, 32/32 |
| Independent Core-only installation, relocation and actual Owner callback | PASS |
| Render public headers / installed relocated consumers | PASS, included in each full matrix |
| Original test names | All 102 retained; added core.execution_work_schedule and core.execution_standalone |
| GPU Validation / SyncValidation / ASan errors | 0 in executed tests; warnings retained in raw logs |
| Legacy / user files / dependencies / whitelist | PASS, 719 blobs, three user files unchanged |
| Performance equivalence | PARTIAL; observations below do not close historical tails |
| Root product closure | Historical BLOCKED; Engine/Editor product testing NOT_RUN under revised scope |
| Linux / Android / TSan | NOT_RUN; Android header synchronization is not build qualification |

MSVC 19.44, Windows, NVIDIA GeForce RTX 4070 Laptop GPU, reported Vulkan API integer 4211039 and driver
2588573696. Validation and Synchronization Validation enabled by the GPU fixtures. Duplicate EOS layer
warnings and all other emitted messages remain in the logs; warnings were not converted into passes.
Dependencies: lux-cxx `bf779515a120350c7c5412c366eea73afdf59ccf`, toolset
`c104eb6dd30a45f5d8e76f24e8d6f8de064bdf60`; exact source and protection facts are indexed.

## Reproduction

The archived PowerShell runners contain exact paths and environment setup. Main commands for each of
`n1-build/r`, `n1-build/d`, `n1-build/asan` were:

```text
cmake -DLUX_SOURCE_DIR=E:/RenderV2Rebuild/V12/n1-verification -P <source>/cmake/ValidateTrackedSnapshot.cmake
cmake -S <source>/cmake/render-v2-bootstrap -B <build> -G Ninja -DBUILD_TESTING=ON
  -DCMAKE_BUILD_TYPE=<RelWithDebInfo|Debug> -DCMAKE_CXX_SCAN_FOR_MODULES=OFF
  -Dlux-cmake-toolset_DIR=E:/SyncForder/CodeRepos/lux-cmake-toolset
  -Dlux-cxx_DIR=E:/RenderV2Rebuild/G3/dependencies/share/lux-cxx
cmake --build <build> --target all -j 4 -- -k 0
cmake --build <build> --target all -j 4 -- -k 0
ctest --test-dir <build> --output-on-failure -j 1 -R .
```

ASan additionally used `/fsanitize=address /Zi` and `ASAN_OPTIONS=alloc_dealloc_mismatch=1`.
The synchronous configuration additionally set `LUX_RENDER_CLIENT_ONLY=ON`,
`CMAKE_DISABLE_FIND_PACKAGE_stdexec=ON`, `CMAKE_DISABLE_FIND_PACKAGE_Vulkan=ON`.
All builds and executions were serialized. Independent Core installation creates its own source closure,
fresh prefix, relocates it and runs an external consumer; it does not obtain Execution through Render.

56 public headers from exact I, including the generated Execution visibility header, were backed up and
synchronized to the Debug, RelWithDebInfo and Android include prefixes. Ten obsolete header paths were
removed where present. Every before/after SHA and backup path is in `sdk-sync/manifest.json`.
The user's uncommitted Graph.hpp was not used as an SDK source.

## Ownership and source review

The single ExecutionRuntime, Task, Timer, TaskScope and PortSender implementations moved to Core.
Ten core source/header files are identical to the baseline after normalizing namespace, include paths
and whitespace (`algorithm-delta.json`). WorkSchedule moved the existing generic scheduler/code-pin
handoff into Core with an Error template; Render keeps its actual error mapping and bounded completion
collection. No new pool, queue or task identity was introduced. Scheduler.hpp is independent of TaskInfo.
There is no old Execution forwarding header. Upper consumers received finite reference migration only.

The traceability checker validates 38 work packages, 86 requirements, 229 audit groups and 34 deletion
rows. These are allocations of future work, not 86 or 229 completed implementation claims. N0's corrected
source supplement excludes the unrelated Editor LayoutPlan; historical N0 reports remain unchanged.

## Performance observations

Ten paired independent-process runs alternate b427 and N1. Each Command and Native run has 1,000 samples.
The external Alias fixture fixes 16 uploads, 256 MiB, 256 chunks, 4,096 draws, 256 upload submits and 16
completion/publication events. The same external instrumentation and tick-boundary window apply to both
versions; patches, executable hashes and all samples are archived. No verification source was patched.

| Per-run p95 paired change (N1 versus b427) | Median | Range across ten pairs |
| --- | ---: | ---: |
| Full Command roundtrip | -0.23% | -1.09% to +0.82% |
| Native record/execute | +2.72% | -35.33% to +28.53% |
| Four View interval during Alias uploads | -0.53% | -4.55% to +6.76% |
| Observed GPU-ready callback to reply publication | +153.91% | -77.33% to +356.14% |

For context only, pooled Ready-to-Reply p95 is 7.7 us versus 24.3 us; pooled full Command p95 is 2.0737 ms
versus 2.0692 ms. Paired run statistics, not pooled samples, are the primary comparison. The Native and
callback tails have not been attributed; neither a regression-free claim nor a speedup claim is made.
The unchanged 1 ms host sleep is included in end-to-end waiting, not described as IPC queue latency.
These runs use validation; validation-off comparison and VTune attribution remain for N7.
Existing GPU timestamp samples concern only the measured upload fixture, not a general GPU speed claim.
Command fixtures report zero measured first-party EXE/Client-DLL allocations for the prebound small path;
this is not a claim that every API or cold configuration is allocation-free.

## Evidence and continuation

`N1-evidence-index.json` records 454 files and the portable archive SHA-256:
`d7b3f223407ffccede3327743efe3093eb82a89095355ae5b12de4af6019c025`.
Archive: `E:/RenderV2Rebuild/V12/evidence/N1/N1-cd6f093d-evidence.zip`.
Precommit attempts remain separately at `E:/RenderV2Rebuild/V12/evidence/N1/latest` and do not replace this
exact-I qualification. Main and Layer user worktrees and old reports/checkers were not modified.

N1 structural/function qualification is PASS within the authorized Render/direct-dependency scope.
Performance remains PARTIAL. This permits N2/N3 joint work; it does not qualify registration, Reply,
Storage ownership, layouts or the later 3D path. Final push remains after the complete N7 verification.
