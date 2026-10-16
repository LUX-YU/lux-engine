# N1 implementation checkpoint

The only generic ExecutionRuntime moved from engine/process/execution to modules/core/execution,
with the target lux::engine::core::core_execution and public lux::execution namespace. Scheduler.hpp
contains scheduling without Task/TaskInfo. Existing TaskScope, Timer, PortSender, CompletionWork and
worker algorithms are retained; their original test names remain. Execution diagnostic identity
strings remain stable deliberately. Business ObjectScheduler, asset/world loading and Editor logic
remain in their owning modules and only use the new names/target.

WorkSchedule.hpp contains the former generic CpuSchedule/CompletionPost implementation, parameterized
by an error type and cold error mapping. Render's AsyncWork.hpp is the actual Render specialization,
not a compatibility header at the old path. There is no new queue, worker pool or TaskId. The existing
Render pending event collection remains its sole request owner; its business types are addressed by N3.

LUX_RENDER_CLIENT_ONLY selects a real synchronous Client/Protocol source closure. It does not disable
full-build tests: only selected SDK components enter that independent configuration, and their tests
still run. Full Render selects Core Execution as a production dependency. No old process package or
CpuSchedule forwarding header is installed.

## Implementation-side checks

- Full RelWithDebInfo Render build all -j 4 -- -k 0 and second no-work: passed.
- Existing tests plus generic WorkSchedule test: 103/103 passed; GPU validation errors zero.
- New independent Core-only build/install/relocation and owner-thread CompletionWork consumer: passed.
- Sync source configuration with stdexec/Vulkan package lookup disabled: 32/32 passed in this worktree.
- Ten Execution public headers compile independently; existing Render installed/header tests passed.
- 719 frozen blobs, three protected user files and dependency SHAs unchanged.

These runs precede the final checkpoint commit and do not replace independent V. A final compile-option
ordering fix ensures the independent Core build also compiles its production targets with /GR-;
independent V must rerun it. Validation warnings about duplicate EOS overlay layers remain in raw logs.
Performance samples are observations only; Native/Alias qualification remains PARTIAL.

The user explicitly restricted upper Engine/Editor changes to finite reference migration and excluded
their full builds from this round. Earlier attempted production consumer compilation found pre-existing
Legacy-only RenderContext/EngineRendering/PluginRendering dependencies and an ungenerated world-loading
reflection header; those logs remain. They are not Render migration PASS evidence. Root TOOLCHAIN is
historically BLOCKED; upper product qualification is NOT_RUN under the revised scope.

Evidence: E:/RenderV2Rebuild/V12/evidence/N1. Independent verification will bind its separate archive and
report to the exact implementation SHA. No push and no N2 completion is claimed by this checkpoint.
