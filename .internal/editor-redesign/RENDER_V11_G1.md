# v11 G1 — typed IPC checkpoint

Parent: `35069de3991732c13d69d76a2ad3c40f9345a0b7`. This is an internal G1 checkpoint, not final S1/S2 verification.

IpcMessage now constructs, moves and destroys one real C++ object through a static TypeOps table. Cold binding compares
the full TypeToken, size, alignment and compilation ABI. CodePin survives the destroy thunk's return. Over-aligned,
immovable and potentially throwing-move objects use external storage; moving the message transfers their pointer.
The project-only erased construction/borrow seam uses the same storage and operations, with no second object owner.

The old byte adapters temporarily use this same object storage, preserving every existing byte-path behavior while
production Runtime consumers await G2/G3 migration. Temporary layout: Message 208 B / align 16, Reply 432 B, TypeOps 112 B.
Measured candidate layouts are 96 B without a code pin, 112 B with pin + 48 B SBO, and 144 B with pin + 80 B SBO.
These are layout measurements, not final performance qualifications. Release Batch is 32 B, Debug Batch is 40 B.

The new test covers actual two-Client SPSC traffic (2,000 roundtrips), vector address/capacity reuse, NotReady/Full/Closed,
ABORTED original return, over-alignment, nontrivial destruction, canonical operations from a dynamically loaded DLL,
and unloading that DLL only after the last pinned object's destructor has returned. The relocated installed consumer
also runs a real typed vector roundtrip. Source comparison confirms all Channel/Link/SPSC/credit/closeout code unchanged.

## Gate evidence

Each configuration completed full `all -j 4 -- -k 0`, a second no-work build, and all 80 registered tests (the original
79 names plus platform.ipc.typed). Initial failures below were corrected and their affected tests rerun; the aggregate
set is passing. This does not claim each initial full run was 80/80. Final independent V will run the complete matrix.

- RelWithDebInfo: old GPU/Runtime/IPC behavior and typed test passed; installed test initially retained the old 144/296 B
  ABI assertion. Its explicit new-layout assertion and additional installed typed roundtrip pass after migration.
- Debug: three direct vector moves allocate three MSVC iterator proxies. The test now compares the same three direct
  moves with the IPC path: direct=3, IPC=3, extra IPC allocation=0. The initial abort produced a 30-second timeout and is
  retained as a failed attempt. New test failure exits are noninteractive; no timeout is treated as cancellation.
- ASan with `alloc_dealloc_mismatch=1`: initial DLL fixture used default delete against the EXE counting allocator's
  malloc. Both sides now use the same allocation instrumentation. The same cross-DLL destruction/unload test passes;
  no sanitizer option was relaxed. Release/ASan direct=0, IPC=0 for the measured vector handoff.
- All preexisting GPU tests passed in all three configurations. Existing loader warnings remain visible in raw logs.

Commands are recorded in `E:/RenderV2Rebuild/V11/runners/run.ps1`; configuration-specific command/exit logs, failures,
and detailed CTest output are indexed in `E:/RenderV2Rebuild/V11/evidence/g1-evidence-index.json` with SHA-256 values.
The 719-file/user/dependency checks remain enforced by check_render_v11.py. No push or historical qualification change.

G1 internal gate: PASS. S1 and S2: NOT_COMPLETE. Continue only with G2/G3's authorized Command migration.
