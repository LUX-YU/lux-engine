# Object/UI H5 controlled Release benchmark

This directory records the reproducible evidence run for the final Foundation
hardening pass. The two CSV files contain all 5 warm-up / 30 measured runs,
followed by median and p95 summaries. Timing is evidence for audit, not a CTest
threshold.

## Environment

- Source base: `2b314d366ec1f810190d6d28420d0b3250dfbc34` plus the uncommitted H0-H5 working tree
- Branch: `codex/object-ui-foundation`
- Configuration: MSVC `RelWithDebInfo`, `NDEBUG`, LTO disabled for benchmark and baseline support
- Compiler: Microsoft C/C++ Optimizing Compiler 19.44.35228, x64
- CPU: Intel Core i7-13700KF, 16 cores / 24 logical processors
- OS: Windows 11 Pro 10.0.26200, 64-bit
- Warm-up/sample shape: 5 / 30
- Build tree: `E:/SyncForder/CodeRepos/build/RelWithDebInfo/lux-engine-foundation-final`

## Structural results

- `Connection` handle: 16 bytes.
- Production `ConnectionControl`: 80 bytes; Candidate A remains the production
  listener layout.
- The private Candidate B fixture adds 16 bytes per connection. Its 4/16/64
  direct-listener median changes in this run are approximately +12.9%, +13.9%
  and -6.0%; the geometric-mean gain is about 7.4%, so it does not satisfy the
  locked 10% admission rule.
- Typed/scoped/dynamic Direct steady samples report zero process allocations.
- Queued payload samples cover 4/64/256/1024-byte payloads with 1/4/16
  listeners; dispatcher contention covers 1 and 4 producers. DLL diagnostics
  show the 4-byte payload messages on inline storage and the larger three on
  heap fallback.
- UI samples use the final `CommandHandle` and real contextual bindings. Steady
  frame, route rebuild, state/invoke, bind churn, focus, and drag payload paths
  are present, with per-sample process allocation counts.

The global allocation replacement exists only in the benchmark executables;
production Object/UI binaries do not contain that instrumentation.
