# L1 Worker Event Memory Safety Closure Evidence

## Status

- Verified production SHA: `07453d479538a0cae4037ea18725390c715b4bda`.
- Baseline evidence SHA: `06e18439fabf22253d7fd881de603dcc7e603ff2`.
- Previously verified production SHA: `636dbb77a45ef749096514f7046b99e16c1534c9`.
- Result: the `EventOccurrenceBuffer` active-writer data race is closed. This remains an L1 Freeze Candidate until
  independent API and semantic acceptance confirms GO for Process.

The production change makes the global active-writer count atomic while retaining the single-owner capability
for each producer lane. Four real `TaskExecutor` workers hold distinct lane writers concurrently for 256 epochs;
topology mutation and drain remain rejected until all writers release. Source architecture validation prevents the
plain counter and direct increment/decrement forms from returning.

The evidence commit contains no production or test changes and has the verified production SHA as its only parent.
The worktree was clean throughout exact-SHA validation. User-owned `.gitignore` and `.clang-format` changes were
backed up and stashed outside the validation window and are restored after this evidence is committed.

## Toolchain and sanitizer status

- Visual Studio 2022 Developer PowerShell `17.14.35`.
- MSVC `19.44.35228` x64.
- CMake `4.1.2`.
- Ninja `1.11.1`.
- Python `3.10.13`.
- MLIR/LLVM `18.1` in the TOOLCHAIN profile.
- Clang/TSAN: `NOT RUN`; no compatible Clang/TSAN environment is installed on this host. It is not a formal gate
  for this closure; the deterministic worker regression, atomic memory ordering, and source scan are the gates.
- Android configure/build/test was intentionally not executed under repository policy.

## Exact-SHA results

| Configuration | Result |
| --- | --- |
| RelWithDebInfo DEVELOPER | full `all` PASS; second build no-work; CTest 94/94 PASS |
| RelWithDebInfo PLAYER | full `all` PASS; second build no-work; CTest 78/78 PASS |
| PLAYER closure | zero FlowForge compiler, NodeGraph, MLIR, or LLVM target matches |
| RelWithDebInfo TOOLCHAIN | full MLIR/LLVM compiler PASS; second build no-work; CTest 79/79 PASS |
| Source architecture | PASS, including atomic active-writer regression scan |

## Benchmark v12

- 25 raw CSV files are committed under `csv/`.
- Every row identifies production SHA `07453d479538a0cae4037ea18725390c715b4bda`.
- Each performance size used five warmups and thirty samples.
- Evaluator result: `PASS: 7 scaling, 1 ratio, 3390 structural checks`.
- The retained `owned-worker-event-buffer` case uses four real TaskGraph workers and 100,000 owned occurrences.

See `manifest.json`, `commands.txt`, `evaluator.txt`, and `sha256sums.txt` for reproducible inputs and results.

## Process entry handoff

This closure does not create or migrate `engine/process`. The first Process Wave 0 production commit must add
`engine/process/*/{include,sinclude,pinclude,src}` and Process CMake files to the root architecture dependencies and
`ValidateSourceArchitecture.cmake` before adding Process implementation code.
