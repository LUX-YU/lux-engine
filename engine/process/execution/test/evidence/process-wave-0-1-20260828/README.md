# Process Wave 0/1 Freeze Candidate Evidence

## Status

- Baseline SHA: `25992bb578ccc7765a0f238c582f62d5424352d1`.
- Verified production SHA: `3a4a531bba6a778d5e105694a9b0833b12cea9ad`.
- Result: Process Wave 0/1 implementation and repository gates pass. This is a Freeze Candidate pending independent
  API, correctness and performance acceptance.
- Implemented scope: Process topology, bounded Timer sender, and allocation-free `OperationPort` sender adapter.
- Deferred scope: FileIO, Render sender, runtime-cardinality fan-out, coroutine runtime, and legacy deletion.

The evidence commit contains no production or test changes and has the verified production SHA as its only parent.
The validation worktree was clean. The user-owned `.gitignore` and hidden `.clang-format` were saved and stashed for
the exact-SHA window; their hashes are recorded in `manifest.json` and are checked again after restoration.

## Toolchain

- Git `2.48.1.windows.1`.
- Visual Studio 2022 Developer environment, MSVC `19.44.35228` x64.
- CMake `4.1.2`; Ninja `1.11.1`.
- Python `3.10.13` from the Codex bundled runtime.
- MLIR/LLVM `18.1` in the TOOLCHAIN profile.
- Android configure/build/test: `NOT RUN` by repository policy.

## Exact-SHA matrix

| Configuration | Result |
| --- | --- |
| RelWithDebInfo DEVELOPER | full `all` PASS; CTest 96/96; second build no-work |
| Debug DEVELOPER | full `all` PASS; CTest 83/83; second build no-work |
| RelWithDebInfo PLAYER | full `all` PASS; CTest 78/78; second build no-work |
| RelWithDebInfo EDITOR | full `all` PASS; CTest 83/83; second build no-work |
| RelWithDebInfo TOOLCHAIN | MLIR/LLVM full `all` PASS; CTest 81/81; second build no-work |
| Hardened Object/UI contracts | full `all` PASS; CTest 101/101; second build no-work |
| Fresh DEVELOPER build/install | full `all` PASS; CTest 96/96; install PASS; second build no-work |
| Installed consumers | 15/15 build PASS; every second build no-work; Process consumer executed PASS |
| Source and installed architecture | PASS; installed forbidden match count 0 |

The PLAYER `process_execution` link query contains only its two implementation objects. It has no World,
Simulation, Scene, Render, Authoring, Editor, Toolchain, Host, legacy, Asio, TBB, or private CPU-pool dependency.

## Process benchmark v1

- Nine CSV files: schedule/cancel/fire at 10K, 100K, and 1M operations.
- Every size uses five warmups and thirty retained samples.
- Every row identifies production SHA `3a4a531bba6a778d5e105694a9b0833b12cea9ad`.
- All measured paths report exact completion/wakeup counts and zero general-heap allocations after setup.
- Evaluator: `PASS: 6 scaling, 810 structural checks`.
- Worst observed adjacent-size exponent: `1.106`, below the `1.25` gate.
- p99 completion/cancellation lateness is recorded but has no hardware-specific absolute threshold.

The initial candidate exposed cancel scaling exponent `1.387`. Cancellation admission was changed to record intent
in O(1); the Timer owner now performs heap removal and terminal completion. The complete exact-SHA matrix and all
CSV evidence were rerun after that production fix.

See `manifest.json`, `commands.txt`, `evaluator.txt`, `csv/`, and `sha256sums.txt` for reproducible details.
