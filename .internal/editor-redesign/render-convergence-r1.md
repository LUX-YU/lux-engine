# Architecture convergence R1 implementation

Parent implementation: `5c70db4b8ae834bbbd2eb7763954a60fdc01dcb9` (R0).

DeviceSession privately owns Device and Allocator, and two lux-cxx StableSlotMaps of BufferAllocation and
ImageAllocation. The maps alone allocate generations and own physical allocation records; records contain
no Device/Allocator. VkBuffer/VkImage are existing lightweight carriers, so no duplicate handle class was added.
Allocation pointers are bounded Native borrows, not remotely usable business IDs. Cold ownership checks
compare both the SlotKey lookup and the stable record address. Context/Scope cannot destroy allocator state.

Creation retains Vulkan/VMA error results. Mapping failure destroys the not-yet-published buffer; alias bind
failure destroys the temporary VkImage. Only successfully constructed records enter the ledger. Image aliases
pin their source allocation even if the original image is explicitly released first. Caller synchronization is
still required for overlapping alias GPU access. destroyFinished requires all borrows and last GPU use to end.

Retirement now stores Ticket + borrowed ledger record, or a composite Context pin. It never receives a second
physical Buffer/Image owner. Completion calls the DeviceSession destruction path. Context destruction releases
views and transient records, then scope leases; persistent scope records live until explicit Scene/View deletion
or Session shutdown. Failed candidate preparation never publishes a persistent business hold. Attachment-only
records are collected at the last lease, which cannot occur before the retired GPU Context releases it.

The old zero-lease persistent deletion assertion was replaced with stronger semantics: after all Attachments
are gone, Global + Scene + two View buffers remain; reattach creates only Attachment-private backing and
produces the original pixels. Explicit View/Scene release removes the matching scopes while Global remains.
The old assertion and all historical test conclusions remain available at R0 and prior commits.

The Native fault fixture additionally covers upload flush failures and alias creation/bind failures. A real GPU
clear uses an alias after releasing the original-image business reference; real Ticket completion retires the
alias and its pinned allocation. Existing partial Submit, queue alias, shutdown and last-good tests remain.

## Host memory (RelWithDebInfo, x64)

Baseline layout was compiled against the exact prior independent I SDK. Its private Binding/Entry field lists
are reproduced in the external measurement source using the original public types; no historical source changed.
New measurements use a test-only friend to measure the actual private Binding/Entry and real StableSlotMap
host storage for 5,000 Buffer and 5,000 Image records. No dummy record is published to a Native Session/GPU.

| Item | Before | After |
| --- | ---: | ---: |
| Buffer owner / allocation record | 56 B | 48 B |
| Image owner / allocation record | 72 B | 72 B |
| Context resource Binding | 160 B | 32 B |
| ExecutionContext | 192 B | 200 B |
| Retirement entry | 88 B | 32 B |
| 10,000 bindings + allocation records/index storage | 1,600,024 B | 1,302,680 B |

The new host storage requires 61 allocations including pages/index growth; its records are not individually
heap allocated. GPU/VMA allocation internals and OS allocator bookkeeping are outside this host metadata count.
The size reduction is about 18.6% in this declared mix, not a claim that every type or workload is smaller.
Raw source, compiler command and output: `E:/RenderV2Rebuild/ConvergenceR1/memory-baseline.*`,
`evidence/R1-memory-before.log`, `evidence/R1-memory-after.log` and the checked-in MemoryCost.cpp.

R1 RelWithDebInfo: all build + second no-work; 47/47 Render tests including relocated SDK consumers.
ASan gate and independent final batch verification are tracked separately; no historical qualification changes.

R1 ASan implementation gate: all build + second no-work; 47/47 Render tests, no sanitizer errors.
