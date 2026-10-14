# IPC fixture bytes-view correction and remeasurement

The requested fixture correction and correctness verification **PASS**. This is
not a new production optimization or a blanket performance qualification. The
historical unexplained tail interval remains **PARTIAL**; no historical report is
rewritten or reclassified. Several new timing cells also retain tail variability.

## Exact change and source identities

- Branch: `codex/render-v2-rebuild`.
- Baseline, I parent and remote HEAD before delivery:
  `b3248e54f70cbe8dd320e8decc21b567593fb5b1`.
- Implementation I / V parent: `64d0280e47c6547385b78a18d17d36c85ac7962b`.
- lux-cxx: `bf779515a120350c7c5412c366eea73afdf59ccf`.
- lux-cmake-toolset: `c104eb6dd30a45f5d8e76f24e8d6f8de064bdf60`.

I changes only `test/Ipc.cpp`: three inserted lines and two removed lines. `decode`
caches `const auto bytes = message.bytes()` and uses that view for BinaryReader
and the loop bound. Scalar read, every byte read, zero-value checks, the 128-byte
reader limit, and requireFullyConsumed remain unchanged. An exact transformation
comparison proves that there are no other test changes. The view is borrowed only
for this synchronous decode of the unchanged input message.

SPSC, message movement, memory publication, production lifecycle, public headers,
test registration and all production files are unchanged. No checks were removed
or weakened. V adds only this report; its own SHA is recorded in delivery.json and
the delivery response, or obtained with `git log -1 --format=%H --
modules/platform/ipc/VERIFICATION_FIXTURE_BYTES.md`.

## Independent verification

Independent clean clone: `E:/RenderV2Rebuild/IPC-Fixture-Bytes/source`, detached at
exact I. ValidateTrackedSnapshot passed before configure; all 2,999 tracked file
SHA256 values match before/after verification. No clone repair was needed.

Windows x64, MSVC 19.44.35226, CMake 3.31.10, Ninja, C++20, /GR-.
The bootstrap uses the isolated SDK at `E:/RenderV2Rebuild/G3/dependencies/share/lux-cxx`.

```text
cmake -DLUX_SOURCE_DIR=<clone> -P <clone>/cmake/ValidateTrackedSnapshot.cmake
cmake -S <clone>/cmake/render-v2-bootstrap -B <build> -G Ninja
  -DCMAKE_BUILD_TYPE=<RelWithDebInfo|Debug> -DBUILD_TESTING=ON
  -Dlux-cmake-toolset_DIR=E:/SyncForder/CodeRepos/lux-cmake-toolset
  -Dlux-cxx_DIR=E:/RenderV2Rebuild/G3/dependencies/share/lux-cxx
  -DCMAKE_CXX_FLAGS="/EHsc [ASan: /fsanitize=address /Zi]"
  -DCMAKE_CXX_SCAN_FOR_MODULES=OFF
  -DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF -DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=OFF
cmake --build <build> --target all -j 4 -- -k 0
cmake --build <build> --target all -j 4 -- -k 0
ctest --test-dir <build> --output-on-failure -j 1
```

RelWithDebInfo, Debug and RelWithDebInfo+ASan each pass **25/25**, including all
existing Core/Data/Protocol, ownership, concurrent close, progress and installed
consumer checks. Each second build reports no work. ASan is present in actual
compile commands; ASAN_OPTIONS=alloc_dealloc_mismatch=1. No sanitizer error or
timeout occurred. Existing allocation assertions remain enabled and pass.

All three fresh relocated install sets contain the expected 12 headers with exact
bytes, and dependency audits pass. Legacy 719 frozen/committed/worktree blobs match.
The one-file I whitelist protects every other file, including historical reports.
User settings SHA256 remains
`f51fd038282aba9d31a4ad9ffb64f1e8bc3b525b4f6ca4c9ca3c2f306e64a57c`.
No public header changed; nine existing IPC header/visibility entries across the
Debug/RelWithDebInfo/Android prefixes were checked unchanged, not rewritten.

## Original repository benchmark: matched individual samples

Before executable: the independent closeout build, whose test source equals this
baseline. After executable: exact I's new independent RelWithDebInfo build. Same
compiler flags, dependency SDK, machine and capacities. The suspended main thread
is pinned to mask 1 before resuming; the two-core fixture uses physical-core masks
1 and 4. Three pairs run before/after, after/before, before/after, serially with no
concurrent build or benchmark. Each cell pools 30,000 samples, retaining all maxima.

Nanoseconds, p50 / p95 / p99:

| Path | Before | After |
|---|---:|---:|
| Full codec 8 B, same thread | 200 / 600 / 700 | 200 / 400 / 600 |
| Full codec 32 B, same thread | 500 / 600 / 700 | 400 / 500 / 600 |
| Full codec 64 B, same thread | 700 / 800 / 800 | 400 / 1000 / 1300 |
| Full codec 128 B, same thread | 1200 / 1300 / 1400 | 600 / 800 / 1000 |
| Full codec 8 B, two cores | 600 / 800 / 1000 | 800 / 1000 / 1200 |
| Full codec 32 B, two cores | 1000 / 1100 / 1300 | 1000 / 1100 / 1200 |
| Full codec 64 B, two cores | 1200 / 1300 / 1400 | 1100 / 1200 / 1300 |
| Full codec 128 B, two cores | 1600 / 1800 / 2000 | 1200 / 1400 / 1500 |
| Complete Operation, same thread | 200 / 400 / 600 | 200 / 300 / 400 |
| Complete Operation, two cores | 700 / 1000 / 1100 | 800 / 1100 / 1200 |
| Preencoded queue baseline | 100 / 200 / 300 | 100 / 200 / 200 |

These runs retain the existing per-operation clock/allocation instrumentation and
CSV output between measurements. Resolution is visibly quantized. Improvements
are not uniform: 64 B single-thread tails and 8 B two-core results are worse in
this sample. Complete Operation does not call the changed decode helper, yet its
two-core sample also changes; do not attribute every timing difference to this
edit. Its after maximum is 2,213,900 ns, retained in the raw data. No samples were
discarded or silently rerun to obtain a preferred result.

## Production-DLL sustained measurement and VTune

To separate per-request instrumentation from CPU hotspots, the previous external
profiling harness is rebuilt with the decode function copied exactly from I.
All other workload text is unchanged. Both versions link the same previously
verified BUILD_TESTING=OFF production DLL, whose sources equal current production.
The workload still validates every field and byte; no check is bypassed. It is a
test fixture, not a G8 Client implementation.

Without VTune, three alternating pairs each run the full 128 B workload for two
seconds, checking elapsed time every 4,096 operations and writing output afterward:

| Version | Three run means, ns/roundtrip | Median of run means |
|---|---|---:|
| Before | 1084.347, 1081.704, 1090.540 | 1084.347 |
| After | 485.019, 485.463, 484.683 | 485.019 |

This is about **55.3% lower mean time**, not a p99 claim or a production queue speedup.

Intel VTune 2026.4.0 build 632893 then collects separate ten-second User-mode
Hotspots runs, fixed 10 ms sampling interval, on mask 1. `IpcMessage::bytes`
falls from **35.0% to 0.95% of CPU samples**. Before: BinaryError boolean checks
24.4%, decode 7.6%, check 7.0%. After: decode 17.8%, check 12.7%, writeBytes 10.7%,
readUnsigned 7.7%, prepare 6.5%. Relative shares describe the sampled workload,
not exact instruction costs. Caching also enables compiler optimization; the gain
cannot be attributed solely to the cost of an individual DLL call.

```text
vtune -collect hotspots -knob sampling-mode=sw
  -knob enable-characterization-insights=false -result-dir <result>
  -search-dir sym=E:/RenderV2Rebuild/IPC-R2-Closeout/verify-production/ipc
  -- <before-or-after-profile-executable> full128 10
vtune -report hotspots -r <result> -format csv -csv-delimiter comma
  -limit 1000 -report-output <csv>
```

Hardware PMU sampling was unavailable in the preceding investigation due to the
non-elevated process; it was not retried here. No cache/CPI/branch-miss conclusion
is claimed. Linux/TSan and Android builds are NOT_RUN; GPU checks are N/A. Root
TOOLCHAIN configure was not repeated for this test-only change; its prior missing
future Render consumer closure remains BLOCKED.

## Evidence and remaining limits

Evidence archive:
`E:/RenderV2Rebuild/IPC-Fixture-Bytes/qualification/64d0280e47c6547385b78a18d17d36c85ac7962b/`.
It contains configure/build/CTest logs, exact source snapshots, SHA manifests,
before/after binary hashes, pinning/measurement scripts, every raw timing CSV,
VTune raw results and function reports, and tracked/Legacy/install protection checks.

The repeated getter issue is corrected and its hotspot reduction reproduced.
This does not explain the historical transient slow interval or close all new
tail variability. Preserve historical PARTIAL; do not authorize an SPSC or
lifecycle rewrite from these measurements. Deliver I/V, push this branch, and stop.
