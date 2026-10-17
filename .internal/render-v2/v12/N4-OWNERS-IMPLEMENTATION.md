# N4 actual owners, qualified resources and physical retirement

Parent: dc6ed902fb57a4f788d512702865e7cdc58c8fff. This completes the production implementation
for N4.2–N4.6 after the separately verified N4.1/type and N4.2/identity prerequisites. Independent
verification is required before the N4 correctness gate closes. N5–N7 remain NOT_STARTED.

## Ownership and publication

Scene/View now contain nonmovable StorageSet members (stable Scene/View tables). DeviceSession has
Global StorageSet; ExecutionContext has the Attachment Set and move-only StorageLeases. Registry alone
allocates StorageId and borrows {actual Set, dense position, type, Native address}; it owns no instance.
Same actual owner/type/key/config shares one instance; different owners do not. Explicit preload and
Attach/operation leases are independent counts. Final logical use erases the ID before invoking the
Native deleter; dense indices are coherent before indirect release. Owner teardown requires outstanding
asynchronous borrowers to have reached terminal state, as v12 §9.6.6 specifies; it does not create an
implicit task/owner protection manager. Runtime stop drains work before withdrawing contexts and Sets.

StorageResourceId<Tag> is StorageId plus the professional local SlotKey. Texture and external Buffer
commands use this one qualified handle; Query/Release have no conflicting extra storage selector.
FindStorage/PrepareStorage/ReleasePreparedStorage resolve owner addresses only in Runtime. Attach
returns owned logical-key/StorageId mappings, preflighting reply funding before publishing a group.
ExecutionGroup retains exactly Scene/View/Target/Context; no second execution relationship is added.

ResourceClaims/GraphStorageBindings have four explicit placement lists. No Scope enum or ancestor
lookup remains. Feature Assembly still uses the sole Builder/Analysis. Native compilation stores
resource indices in the corresponding four lists; Attach supplies actual owners and prepares stable
borrows once. Per-requirement typed Config and Binding encoding migration belong to N5, not a second
Scope implementation.

## GPU lifetime and allocation

RetirementReservation is a move-only publication-time right in the existing RetirementQueue SlotMap.
Context and physical candidates reserve it before publication/submission. Retirement commit cannot fail
for capacity or allocate. The existing Queue/Ticket/SubmissionRequirements remain the sole GPU truth.
Three physical-queue frontier lists plus a ready list replace whole-entry scans. Nodes are detached and
SlotMap entries erased before invoking deleters; nested release is supported. No new Queue/callback
system is introduced. The private inspected counter measures actual candidate visits, not live records.

Withdraw stops future recording, cancels readbacks and removes Use subscriptions while Storage still
exists. Texture logical identity/subscriptions may die immediately; physical versions, descriptor lease
and accepted LastUse live in preallocated retirement custody. External Buffer proves the same interface.
Neither keeps Scene/View/configuration alive. Destroy paths do not allocate or fence-wait. The previous
T1/T2 LastUse merge and allocation permits are preserved.

DescriptorArray owns one real device sampled-image table shared by compatible Texture Storage
instances. DeviceSession keeps only cold weak allocation lookup; physical leases keep the table until
all accepted GPU uses end. SlotMap uniquely allocates descriptor slots, which are returned only after
completion. Texture local backing keys no longer double as descriptor indices. Default version/byte
business limits are removed; nonzero resident budgets are explicit, and actual device descriptor limits
and allocation errors remain enforced. Final descriptor shape/relocation is N5's single LayoutPlan task;
the external test Buffer's private descriptor layout is also migrated there, not a new layout authority.

## Deleted / preserved contracts

Deleted ResourceDomain/DomainTag/Scope, root/ancestor/findVisible, Registry instance ownership,
identityOf reverse lookup, Factory.collect, Storage/backing full scans and release-time retirement
capacity admission. Header migration has no compatibility aliases. IPC/SPSC, Command/Handler tables,
G4 analysis, Queue admission/real tickets, Transfer, protected histories and user trees are unchanged.
The historical resource_domains test name is retained, with equivalent actual-owner assertions.

## Tests and diagnostic evidence

- Registry: actual owners, two users, distinct same-key owners, wrong Config/Ops before create,
  provisional failure/empty result, stale identity, preloads vs leases, indirect release and rollback.
- Native execution/resource_domains: actual sharing/private resources, six preparation fault points,
  cross-Session actual owner rejection, last-owner close while accepted GPU work is held, exact release.
- TextureSampling: two actual Scene Texture instances, same device Descriptor table and Graph,
  distinct qualified IDs/slots, exact pixels32/96, first Scene closes while second continues, and final
  logical owner disappearance without early Sampler/Pool destruction or heap allocation.
- TextureStorage: explicit saturation/bytes budgets retained; default growth129 real uploads,
  first/last readback; actual RTX device descriptor capacity1048576, not a promised universal capacity.
- StorageBindings: final external Buffer owner release in-flight, exact Buffer destroys after Ticket.
- StorageRegistration: real Find/Prepare/Release, Global/two Scenes/View, wrong config, stale scene,
  repeat preloads and rejected reply capacity. A new fixture initially dereferenced rejected submit(0)
  and waited for a nonexistent request; corrected to assert rejection and input retention. Failure log kept.
- BufferExtension: Attach mappings, Attachment-private preparation, detach invalidation, other uses intact.
- Foundation/Shutdown: publication-time capacity failure; legal detach never needs new capacity;
  wrong owner/moved reservation/nested retirement, pending Readback and stop semantics retained.
- MemoryCost: 1/100/1000/10000 real VkBuffers and actual StorageSets with prepared retirement records;
  each1000 zero-retirement polls observe zero candidate visits and engine allocations, exact cleanup.
  Raw timings are diagnostic, not equivalent-workload speed qualification.

All106 Render/Platform tests passed before the final candidate-visit instrumentation (125.39s), then
MemoryCost passed with all four scales. Full final-I independent R/Debug/ASan validation follows.
All original102 names/behavior mappings remain. The initial qualified Buffer config size and TypedBatch
local Scene-array assumptions were corrected in test fixtures; failures are retained in raw evidence.

Measured RelWithDebInfo: Binding32, Context264, RetirementEntry104, PreparedBinding64, StorageUse88,
Registry168, StorageSet40, Factory176, SubmissionRequirements48, SubmissionCallback48 bytes.
TextureBacking568, GpuWork40. Host-only10000 allocation records+bindings1343640 bytes/61 new calls.
These are exact diagnostic sizes, not claims of lower overall memory or faster rendering.

Evidence currently external in E:/RenderV2Rebuild/V12/evidence/n4*-*, n44-groups-diagnostic,
n45-descriptors-diagnostic, n45-scene-descriptors, n46-physical-diagnostic and n46-zero-retirement.
Independent V will archive immutable logs/samples/failures/SDK backups under the exact I SHA and add
only its report/index. Legacy719, three user file hashes, dependencies and history are checked.
Performance remains PARTIAL; root TOOLCHAIN historical BLOCKED; upper Engine/Editor, Linux/Android
and TSan NOT_RUN. No push until authorized final sequence completes; no N5 implementation before V.
