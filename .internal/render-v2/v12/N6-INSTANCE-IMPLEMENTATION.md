# N6.3 Instance sparse-record production checkpoint

Parent `13e87416b07ada66fbe8bf1339fae01fb64a1e89`. This is the Instance part of N6.3, not completion of Camera or N6 integrated rendering.

## Actual production behaviour

InstanceStorage is installed by the same typed Factory/Handler path; four methods share one Handler Owner.
Qualified InstanceId uses the unique StorageId plus lux-cxx SlotMap generation. CPU entries retain
MeshId/MaterialId/submesh and matrix/revision. Creation validates actual Mesh/Material and owner scope;
references are logical IDs, not a second Mesh/Material owner. A future cold Draw selection must validate
those IDs; its existing prepared bindings retain physical resources independently.

Three mapped GPU snapshots are allocated by the one DeviceSession/VMA ledger. They are not another
upload queue/state machine. Frequent matrix writes use existing checked mapped write/flush. Each snapshot
owns its actual LastUse and reserves Retirement before publication. Capacity grows geometrically up to
actual maxStorageBufferRange; an optional resident budget counts current and retired snapshots. The three
snapshots bound physical buffering, not the number of live instances.

Update validates the entire batch, sorts sparse indices and keeps the last duplicate. All duplicate
entries must be individually valid. It chooses a completed **non-current** snapshot, repairs that bank's
unique dirty indices, writes changes, then commits CPU revisions and rebinds prepared uses. A failed flush
cannot damage the current snapshot; the unpublished bank stays dirty for retry. If no bank is available,
real Busy precedes writes/commit. Success means mapped data prepared/published, not that a frame displayed.
Sequence is echoed exactly; stale expected_revision is rejected. Returned original vector keeps backing
and capacity, and full capacity is charged through existing command ownership accounting.

No frame-time live-record scan: sparse update touches its batch and the selected bank's outstanding
unique dirty list. Structural growth copies CPU shadow deliberately and is separate from sparse cost.
Delete requires cold selections withdrawn (Busy while a Graph binding exists), so an old Draw cannot
silently select a reused slot. Logical Storage can close after Context withdrawal with GPU work pending;
physical snapshots survive through existing Retirement until actual Queue completion.

## Narrow existing executor correction

A professional Buffer can be larger than a Graph-declared prefix. Descriptor range and Buffer Copy size
now use that compiled logical range, rather than physical allocation size. Native cold preparation still
checks range compatibility. No Graph analysis, SPSC, Queue/submit, memory publication or GPU completion
algorithm changed. No indirect-instance execution claim yet; indexed-indirect integration is N6.4 work.

## Implementation evidence

Windows RelWithDebInfo full all -j4 -- -k0; second build no-work; complete CTest **138/138,139.78s**.
Two new tests retain all136 earlier names. Public headers compile independently; fresh relocated SDK
consumer actually uploads Mesh/Material, creates/updates/queries/releases Instance. Pure Client and sync
SDK tests retained. Main/Layer user files and719 Legacy blobs pass check_material_protection.py.

Native test runs three physical Queue configurations, real Buffer Copy/Readback, held completion with
three actual accepted submits, Busy before mutation, logical close before physical completion, stale and
reused generation, invalid owner, full-batch failure, Last-Wins, faulted flush with retry, and zero buffers
after Session destruction. For1/100/1000/10000 live records, a measured sparse update checks1 input and
writes2 records; the respective snapshot ledger is192/24576/196608/3145728 bytes. Full batches of all four
sizes are also applied. These are access/byte counts, not a claimed timing improvement. Validation and
SyncValidation errors0; three duplicate EOS-overlay warnings retained.

Runtime test uses actual typed Commands and GPU-populated Mesh/Material:100-instance batch,
FAILED original vector pointer/capacity recovery, exact refund, duplicate Last-Wins, stale revision rejection
and release. No fake Handler or mock Graph. Camera, two Feature implementations, final3D sample and N7
remain unfinished; historical performance PARTIAL and root product BLOCKED unchanged.

Failures archived: unsubmitted mapped snapshot initially used releaseCompleted(empty), correctly rejected
by existing ownership boundary; destructor now chooses destroyUnsubmitted only for no-submit backing.
The first real GPU prefix Copy exposed full physical-size Copy and Validation rejected it; corrected to
Graph range. Initial Runtime fixture omitted explicit upload configuration, corrected in fixture. Temporary
DbgHelp stack instrumentation was removed after diagnosis; old Validation/failure logs retained.

Commands and logs: E:/RenderV2Rebuild/V12/evidence/N6-instance/implementation/r. Runner
E:/RenderV2Rebuild/V12/run-material.ps1 -Source .../source -Build .../build/r -Action build/build/test
-Config RelWithDebInfo, test Filter '.'. Independent exact-I ASan and clean-clone protection follow.
