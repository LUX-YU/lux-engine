# I5 — selected Native bootstrap and cold installation

Implementation parent: 69e0665ef38679ae7d4c1d1891dd721b37aed4d3. Exact I is recorded by the following checkpoint/final report.

The user's latest direction is to continue implementation instead of repeatedly investigating the three baseline Debug failures. Their I4 FAIL evidence remains unchanged. They are outside this change and do not block its dependent implementation; they are not relabelled PASS.

## Production changes

- `RenderServer.hpp` has no Native/device/Texture or public CPU-injection configuration. The selected `VulkanServer.hpp` supplies `vulkan::createRenderServer(config, ExecutionRuntime&, DeviceOptions)` and typed installation definitions.
- The existing Impl borrows the Host CpuScheduler, creates the sole Instance/DeviceSession, and sends compilation and upload preparation to that same pool. ServerBootstrap is stateless private-access plumbing, not a second owner or initialization implementation.
- Storage/Feature provider specializations retain their existing implementations in Native while using neutral trait names. No compatibility aliases remain.
- StorageFactory erased fields are immutable after typed construction. Config equality reuses ConfigValue; its duplicate installation comparator and separate featureStorages forwarding step are removed. Existing Handler reuse avoids repeated member validation. New directory publication follows the recoverable checks, retaining active selections and rollback semantics.
- Module pins are removed from ConfigValue, RenderPayload, Handler/Feature/Command metadata, async receivers, Core WorkSchedule, Transfer and GpuWork. Data ownership, source capacity accounting, true GPU Ticket/LastUse and callback-tail state snapshots remain. Host retains the module until all foreign values, handlers, configs and callback stacks have ended.
- Existing DLL fixtures now explicitly keep their module loaded through the actual final destructor/callback return. This supersedes automatic unload through message/work pins; it does not waive native resource lifetime.

## Construction checks

Windows RelWithDebInfo `all -j 4 -- -k 0` completed, followed by `ninja: no work to do`. Public header probes include the new selected bootstrap and the existing neutral Server probe. The fresh relocated Runtime SDK consumer and external TestBuffer consumer passed.

The focused set comprised 14 tests: Core WorkSchedule; Data Payload/Config; Resource contract/registry; Runtime Server DLL, Storage registration, Offscreen, installed consumers, Texture progress, Handlers, Material operations, Feature bundle and asynchronous DLL callback tail. First run: 13/14. Server DLL fixture explicitly joined and then its CpuFixture destructor joined again; the fixture now destroys the Host once at its existing shutdown point. Only that failed test was rerun, and all four late-Reply cases passed. Initial compile failures from old API call sites are retained in evidence.

Offscreen observed 2 Scenes / 4 Views, 584 hot ticks, 2326 submissions, first-party hot allocations 0, Validation/SyncValidation errors 0 and 1 warning. These are construction observations, not a new latency qualification. Texture failure/stop/callback and real installed Buffer results passed. Final independent configuration, ASan and comprehensive regression remain I7 work; no duplicate full matrix is claimed here.

The two controlled scheduler-failure fixtures retain a test-only compile-time hook around their actual Host scheduler. Production initialization has no injection field or secondary pool. This preserves rejection/pause evidence without restoring a public scheduler-adapter configuration.

Evidence: `E:/RenderV2Rebuild/IPCConvergence/evidence/i5-build/`, `i5-targeted/`, and `i5-protection.json`; final evidence index will hash/archive these. Protection check: 719 Legacy blobs, four protected user files, dependency identities, historical files and whitelist PASS. Performance remains PARTIAL.
