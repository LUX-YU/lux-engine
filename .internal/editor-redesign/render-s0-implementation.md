# S0 implementation gate

Parent: `be9d7af3d4378a1115d9373bc4e73bc5d7bb68c2`. Internal S0 gate PASS; final independent V is still required.

- External factories return move-only `AllocationPermit<Tag>` (16 B Session + existing SlotKey), not a raw release owner.
  `borrow()` resolves owner/key/state without reading a stale pointer. Retirement first validates the qualification,
  real queue ticket and capacity; successful admission marks the sole ledger record retired and invalidates the permit.
  `destroyFinished(pointer)` and raw factories are private trusted Context/Session/retirement paths. Explicit
  `destroyUnsubmitted` is only for never-submitted candidates, not an alternative to GPU retirement.
- Alias backing pinning is retained; released roots cannot be borrowed again. Forged duplicate qualifications exist
  only in the test audit friend, exercising retired, stale generation, same numeric key in another Session and full retry.
- S0 rejects GPU-ready Feature registration and ordinary GPU-ready reply. GPU readiness is NOT_IMPLEMENTED at this commit.
- Zero-pass contributions never call Program. Pure business Feature registers with nullptr. Live-pass missing Program
  fails accurately. The former zero-pass foreign-Program fixture now contributes one pruned pass so the existing authorship
  counterexample remains meaningful under the new zero-pass contract.
- Bound Session, OperationId and descriptor are read-only; typed binding construction remains Client-only. Existing stale
  session/generation tests now send actual raw IPC frames. No server boundary validation was removed.

Commands: `E:/RenderV2Rebuild/S0G10/run.ps1`, configure/build/build/test per variant; all builds use
`cmake --build <build> --target all -j 4 -- -k 0`; second build no work. CTest `-j 1 -R .`.
RelWithDebInfo 61/61 (49.98 s), Debug 61/61 (51.71 s), MSVC ASan 61/61 (63.70 s).
After adding a real 64 MiB Fill / zero-deadline Fence timeout test, all three full builds were repeated (second no work)
and both affected foundation tests passed in every variant. Timeout was actually observed; no mocked readiness.
ASan `/fsanitize=address /Zi`, `ASAN_OPTIONS=alloc_dealloc_mismatch=1`; errors 0. Actual RTX 4070 Laptop,
Validation + SyncValidation errors 0. Existing EOS layer warnings remain in the logs. Header and relocated install
consumers are part of these matrices. Full final independent matrix will repeat all tests on the final production I.

Binding 32 B, Context 200 B release, Retirement entry 32 B unchanged. Buffer record 48 -> 56 B for retirement state;
Image 72 B unchanged. 10k ledger/binding metadata 1,302,680 -> 1,343,640 B (+40,960), still 61 allocations.
Stable four-View window: 581 ticks / 2324 submissions / Engine new=0. R5 pipeline/bind/scope cases pass unchanged.
No performance improvement claimed. GPU timestamp NOT_MEASURED; Linux/Android/TSan NOT_RUN.

719 frozen Legacy blobs, 3101 protected baseline files and primary user hashes pass the new exact-baseline checker.
Historical R6 checker excludes its later V report and therefore is not applied as a current whitelist; it is unchanged.
IPC, G2, G4/G5 algorithms and Protocol production files are unchanged. Primary worktree was not used for implementation.
Evidence: `E:/RenderV2Rebuild/S0G10/evidence/s0-*-LastTest.log`, timestamped configure/build/test logs,
`preflight.json`, `legacy-baseline.json`, `s0-protection.json`. Samples and manifests are archived by this I after commit.
Root product closure and IPC historical PARTIAL remain separate from this gate. G10 may now proceed; G11–G13 may not.
