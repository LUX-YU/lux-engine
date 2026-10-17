# N6 Instance — independent checkpoint

Exact I `10f7fb13c4639d7c62f83be9b34f76db00901943`, parent `13e87416b07ada66fbe8bf1339fae01fb64a1e89`. Remote observed `78bcd220815f845466d85d31f994901e4db224ba`, no push.
Detached no-hardlinks clone without alternates; tracked snapshot first, all source pristine throughout.

Windows MSVC ASan /fsanitize=address /Zi /GR-, ASAN_OPTIONS=alloc_dealloc_mismatch=1:
**138/138**, 194.87s. Fresh all-j4--k0, second no-work, serial CTest. Original102 names and all136 prior
names retained; two new Native/Runtime Instance tests. Fresh/relocated SDK uploads Mesh/Material and
creates, updates, queries and releases Instance through exported APIs. All public headers, pure Client
without Vulkan and synchronous Client without stdexec pass. Validation/Sync Error0, ASan Error0;
duplicate EOS overlay warnings retained. Exact-I fresh Debug/Release not run at this sub-checkpoint;
implementation Release138/138139.78s is separately recorded. Final N7 still requires full R/D/ASan.

Native uses real GPU Copy/Readback in three Queue topologies and actual accepted submits with completion
observation held. Three physical snapshot backing lifetime, Busy admission, stale generation, full-batch
failure, Last-Wins, injected flush failure and repair, logical teardown before GPU-safe release all pass.
Full update batches1/100/1000/10000; sparse-only updates at those live counts inspect1 input and write2
records, with192/24576/196608/3145728 snapshot bytes. Structural growth and prior full dirty work are not
included in that sparse-only count, and no latency improvement is claimed.

Runtime uses production typed commands and returns the original101-element vector with reserved257
capacity after failure/success, retaining its pointer. Exact refund, sequence echo, stale revision and
no partial batch commit pass. Qualified StorageId+SlotKey remains sole logical identity; Registry borrows
StorageSet-owned instances and DeviceSession uniquely owns Native allocations. Retirement is unchanged.
Descriptor/Copy now honour compiled logical prefix size when physical allocation has grown; failure logs
include the original Validation rejection, then the actual corrected implementation and clean validation.

Instance component is qualified only for this tested scope. N6.3 Camera, N6.4 BasicMesh/DepthOnly,
N6.5 integrated3D and N7 remain incomplete. Current GPU table contains matrices; indirect instance-record
integration is not claimed. Delete requires withdrawing compiled selection; it does not silently reuse a
slot referenced by an old Graph. Mesh/Material references are logical IDs, not additional resource owners.
No GPU culling/WSI/HDR or complete-product claim. Performance/IPC PARTIAL and root TOOLCHAIN historical
BLOCKED remain; upper Engine/Editor/Linux/Android/TSan NOT_RUN, GPU timestamp NOT_MEASURED.

Protection checker passes719 frozen blobs and3 user files in source and clone; historical reports and
checkers remain unchanged. SDK sync73 headers13 obsolete removals258backup/hash records from exact I.
Commands: archive run.ps1 Source=E:\RenderV2Rebuild\V12\n6-instance-verification, Build=E:/RenderV2Rebuild/V12/n6-instance-build/a,
Actions snapshot/configure/build/build/test. All raw logs, failures, source diff, cooked inputs, SDK backup
and hashes are indexed in N6-INSTANCE-evidence-index.json.
Portable `E:\RenderV2Rebuild\V12\archive\10f7fb13c4639d7c62f83be9b34f76db00901943\n6-instance-evidence.zip` SHA-256 `8e3366dd0f2f1586d00facf8b1b443afe2da3aecf1573c0f1138969e560ce9bc`. V source contains no repairs. V self SHA follows commit.
