# F4 — Native Graph implementation work order

User authorization: 2026-10-11, attached F4 work order. Entry HEAD and actual remote:
`25bd4e16358bfe3a8c1163a18b17b807bd6a5505`.
Independent construction worktree: `C:/Users/ChenHui/.codex/worktrees/render-v2-f4/lux-engine`.
Preflight evidence: `E:/SyncForder/CodeRepos/archives/lux-engine/RenderV2/F4/preflight/`.
The original attachment, byte hash, Git objects, six user files and staged/unstaged/
untracked/status captures are preserved there. No original worktree file is edited.

FINAL 02/04/05/06/07/12/13/15/16/17/19 controls the implementation. The user's F4
authorization supersedes historical F0-only permission statements, not architecture.
F4.0–F4.5 are internal gates of this one stage; F5 is not authorized.

## Responsibilities reviewed before editing

- F2 LogicalGraphPlan remains the single source of versions, hazards, order, scope,
  imports and conditional input choices. Native compilation consumes these outputs.
- F3 NativeShaderProgram owns Layout/Shader/PSO candidates. BoundDescriptorSets owns
  pools/sets and borrows their layout/backing. No shadow reflection or pipeline owner.
- R4 VulkanDevice/VulkanAllocator/Buffer/Image/ImageView/SubmissionQueue own native
  mechanisms. Extensions stay in those types; no second allocator or queue scheduler.
- One ExecutableGraphPlan composes native owners and immutable resource/sync/record
  recipes. Slot scratch only manages bounded recording state and real completion.
- Imports remain external ownership, with explicit native state/readiness/last use;
  neither GraphBackingId nor a frame counter proves completion.

## Initial precise file inventory and conditional consumers

Existing paths below were enumerated with `git ls-files` (raw inventory in preflight).
All paths are relative to `modules/function/render/vulkan/` unless stated otherwise.

| Existing file pair | Necessary consumer / extension | Direct regression |
|---|---|---|
| `include/lux/engine/render/vulkan/device/Device.hpp`, `src/device/Device.cpp` | C12 real queue selection, enabled timeline/Multiview/local-read caps | old default device, unsupported features, actual family/queue handles |
| `include/lux/engine/render/vulkan/memory/Memory.hpp`, `src/memory/Memory.cpp` | C10 actual mip/layer/alias backing | old create, invalid extent/range/sample, allocation rollback, alias on/off |
| `include/lux/engine/render/vulkan/descriptor/ImageBindings.hpp`, `src/descriptor/ImageBindings.cpp` | W02/W16 exact view subresources | old one-level view, range/aspect/type failures |
| `include/lux/engine/render/vulkan/pipeline/Graphics.hpp`, `src/pipeline/Graphics.cpp` | W16/local-read pipeline state in native identity | existing PSO states, viewMask capability and layered output |
| `include/lux/engine/render/vulkan/transfer/Submission.hpp`, `src/transfer/Submission.cpp` | C12 Submit2 cross-queue waits and real tickets | old begin/submit/poll, wrong owner, capacity, lost, wait/signal |
| `include/lux/engine/render/vulkan/retirement/Retirement.hpp`, `src/retirement/Retirement.cpp` | composite plan retirement with actual last-use evidence | original retirement, last-use join, candidate replacement |
| `src/shader/Bindings.cpp`, `include/lux/engine/render/vulkan/shader/Bindings.hpp` | native Graph descriptor ranges/import slots | existing F3 full-shape fixtures, range mismatch and slot lifetime |
| `src/shader/Program.cpp`, `include/lux/engine/render/vulkan/shader/Program.hpp` | enabled InputAttachment/Multiview interface capability and identity | disabled capability rejection retained; enabled native positive |
| `src/descriptor/Descriptors.cpp` | input attachment local-read layout | incompatible descriptor/layout rejection retained |
| `CMakeLists.txt`, `test/CMakeLists.txt`, `test/shader/CMakeLists.txt` | production native Graph and generated fixtures | dependency closure; all 137 existing obligations retained |

The shader changes are conditional native wiring, not an F3 redesign. Concrete
current blockers: `Bindings.cpp` rejects `texture.mip_count != 1`, `array_layers != 1`
and any nonzero view base; its immutable set creation cannot refresh changing imports
within a completed slot. `Program.cpp::supportedInterface()` rejects InputAttachment
and MultiView SPIR-V capabilities; Layout already budgets input attachments correctly.
Each extension must reuse its validation/owner and
have a direct negative case; no raw descriptor bypass in Graph is allowed.

Planned new paths (small files by actual responsibility, not persistent compiler classes):

```text
include/lux/engine/render/vulkan/graph/Executable.hpp
src/graph/Compile.cpp
src/graph/Resources.cpp
src/graph/Sync.cpp
src/graph/Record.cpp
pinclude/GraphNative.hpp
test/NativeGraph.cpp
test/NativeGraphLifetime.cpp
test/NativeGraphBenchmark.cpp
test/shader/F4*.hpp
test/shader/F4*.lglsl
cmake/render-v2-bootstrap/verify_f4.py
docs/render-v2/F4_*.md
```

Actual new type/algorithm inventories must name concrete types/functions and replace
these planned paths where necessary before I. No production class is justified merely
by appearing in this plan. Changes to any additional existing file require a recorded
consumer and permission check before editing. FINAL/Core/Transport/LogicalGraph/
Description/Toolchain/history/Legacy/Runtime/product remain read-only or forbidden.

## Internal execution and proof sequence

1. F4.0: real typed LogicalGraph → F3 candidates → R4 backing → production recorder
   compute/graphics/readback; complete native identity and ownership chain.
2. F4.1: exact subresources/imports/exports; VMA-backed alias proof and no-alias oracle.
3. F4.2: exact Sync2 scopes/layouts; queue dependencies, family transfers, fallback;
   attachments/resolve/local-read, deterministic diagnostic trace.
4. F4.3: bounded 2/3 FIF, receipts, no premature slot reuse; candidate rollback and
   old-plan retirement; explicit device-lost terminal failure.
5. F4.4: W02/W03/W04/W16 and I36/I37 GPU numerical/image oracles via one production
   compiler/recorder. Available native capabilities must actually execute.
6. F4.5: cold compile/VRAM/alias data, stable record/submit zero first-party allocation,
   GPU timestamps, no lazy Shader/PSO/Layout compilation, actual dependency closure.

Tracks: I03/I04/I05/I13/I14/I35/I36/I37/I44, REQ19–24/38/46.
Device preflight reports graphics+compute family (16 queues), transfer (2), dedicated
compute (8), multiview/timeline/sync2/local-read support. Enabled features must be
reported separately; support is not enablement or completed implementation.

## Test impact and final gate

Development builds/tests follow actual source/header/link/generated dependencies.
Unchanged Core/Transport/F2/Meta evidence is reused with exact source SHA. F3's narrow
test-impact script is not reused as a general selector. Conditional Foundation or
Shader edits trigger their concrete lifetime/GPU/ASan regressions.

After final I: independent no-hardlinks clean clone, normal/full-ASan two full builds,
second no work; applicable original 137 obligations plus real F4 tests, GPU+SyncValidation
0 errors, source/header/codegen/link closure, provenance and protected objects. V only
adds F4_VERIFICATION.md. Then push codex/render-v2 and STOP.

Current status: IN_PROGRESS. No F4 capability is yet qualified. Product remains
EXPECTED_UNAVAILABLE; Runtime/F5 is not implemented or authorized.

## Descriptor refresh consumer (development decision, not qualification)

`FrameGraphBindings::resourceFor()` may choose a different fallback, and an imported
resource may change native backing without changing F2/F3 compile identity. Current
`BoundDescriptorSets::create()` permanently captures the first values; merely changing
Graph barriers would therefore still draw the wrong resource. The minimal extension
is an ordered, prevalidated descriptor rewrite on the existing owner after its real
last-use tickets have completed. Cold recipes capture numeric native shape/range
constraints and initial value positions. Refresh performs no semantic-name lookup,
reflection, pool allocation or pipeline creation. The caller explicitly supplies all
last-use tickets; it retains backing/views until subsequent completion. Busy/wrong-device/
wrong-shape failures occur before any descriptor write. Tests must change a conditional
fallback and an imported backing on a completed slot, reject pending/foreign tickets
and shape mismatches, and rerun existing F3 descriptor/GPU/ASan obligations.

Files: existing `shader/Bindings.hpp` and `src/shader/Bindings.cpp`, native Graph
private recipes/record and F4 tests only. This extends the existing F3 descriptor owner;
it does not add another DescriptorPool or resource authority. Native imports provide
prepared views as synchronous input; no hot `vkCreateImageView` is permitted.
