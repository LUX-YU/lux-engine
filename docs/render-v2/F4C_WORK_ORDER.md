# RD2-F4-C implementation and dependency impact

Entry: `6b777c96bbe3986c940cafe4a3e213ae2e0a3194`; user approved RD2 and only F4-C.
Construction reuses the clean detached RD2 worktree. Original six edits and V2 Operation.hpp are isolated.
Preflight and all failed reproduction logs: `E:/SyncForder/CodeRepos/archives/lux-engine/RenderV2/F4-C/`.

## Evidence before production edits

New real GPU fixture uses production meta/emitter/reflection and Native compiler/recorder, unchanged production.
C1 byte readback succeeds but shader-to-host dependency is absent; distinct transfer/compute families also trigger
VUID-VkBufferMemoryBarrier2-srcStageMask-03851 because an intermediate readback changed GPU last-access to HOST.
C2 two real Image::alias handles sharing memory are admitted; SyncValidation reports WRITE_AFTER_WRITE.
C3 both slots rebound B, completed A image/external view destroyed, further B readback and plan destruction: 0 errors.
C3 currently does not justify changing ownership; expanded lifetime regression and ASan remain necessary.

## Minimal fixes and real impact

- GraphNative.hpp, Sync.cpp, Record.cpp: cold precompute readback buffer cells from actual writing uses and output
  ranges; last unconditional writer subsumes earlier writers, conditional writers retain necessary publication.
  Execute a Sync2 host availability barrier without replacing GPU last-access/family evidence with HOST state.
  Existing HOST_READBACK staging copies already contain their own barrier and remain unchanged.
- Memory.hpp/Memory.cpp: expose BufferMemoryBinding value (memory/offset/size) using existing VMA allocation.
  This is an observation, not another owner/allocator or mutable resource registry. No Buffer ABI storage change.
  Consumer: imported Buffer/Image physical overlap check, including cross-kind actual memory domains.
- Frame.cpp: reject unproven overlapping import allocation ranges even for distinct native handles; use subtraction
  bounds to avoid offset+size overflow. Same-handle guard remains; active identical imports need matching ready order.
  A different native handle that overlaps an active allocation is refused even with a ready ticket: ordering alone
  does not prove the missing alias dependency/layout recipe. Native validity is checked before querying VMA facts.
- Output buffer boundaries enter cold cell partitioning. Readback rejects bytes outside the declared publication
  range; the real partial-export fixture reproduced acceptance of undeclared bytes before that guard was added.
- No F2/Core/Transport/Description/Generator changes. C3 remains a regression, not an unproven bug repair.

Memory header is consumed by Foundation owners, F3 descriptors and Native Graph. Necessary final regression:
original 146 Normal + Full ASan obligations, Foundation fault/move/retirement, F3 binding/descriptor/GPU,
Native Graph multi-queue/alias/readback/import regression, real dependency closure and unchanged-workload performance.
Development runs only direct targets. Final I is qualified once in a new no-hardlinks clone. No current PASS claim.

New private state is one vector of host-publication cell indices within existing UseRecipe, cold-owned by plan.
No new production service/manager/RAII authority. BufferMemoryBinding is a borrowed native allocation fact snapshot;
its handles are not lifetime pins and are valid only while the underlying Buffer and allocator live.

C3 additionally destroys the actual heap-allocated A owner while one completed slot still retains its old borrow;
later B submission and plan teardown must pass both real validation and Full ASan. There is no C3 production rewrite.
C1 uses a real host-signaled Vulkan event to prove incomplete GPU receipts return BUSY, and rejects not-ready input.
C2 covers actual overlapping aliases, same handles, independent allocations, and aliasing an in-flight prior slot.

The user's subsequent §G decision authorizes the bounded draw extension recorded in F4C_SCOPE_AUDIT.md.
F4C_PERFORMANCE_INVESTIGATION.md retains all pre-extension comparisons. Final qualification remains required.
No F5/F6/F7 work is authorized.
