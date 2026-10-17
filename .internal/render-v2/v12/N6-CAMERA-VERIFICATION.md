# N6 Camera — independent checkpoint

Exact I `432ab9979de31748ff3f6a77a17b192e9c649c17`, parent `0cbf920a990aaa42840dea77807e674779ce41d9`. Remote observed `78bcd220815f845466d85d31f994901e4db224ba`, no push.
Detached no-hardlinks clone, no alternates, tracked snapshot first, pristine source throughout.

Windows MSVC ASan /fsanitize=address /Zi /GR-, ASAN_OPTIONS=alloc_dealloc_mismatch=1:
**139/139**, 196.23s. Fresh all -j4 -- -k0, second no-work, serial CTest. All138 prior names and
original102 baseline names retained. Validation/SyncValidation Error0, ASan Error0. Duplicate EOS
overlay warnings are retained in raw logs. Implementation Release139/139140.36s is separate evidence;
fresh exact-I Debug/Release NOT_RUN at this sub-checkpoint. Final N7 still requires full R/D/ASan.

New real GPU Camera test verifies complete view/projection matrices, isolated Views, normalized viewport,
clear/draw pixels, Target resize, invalid finite/range inputs, stale generation and last-good retention.
Wrong ABI reaches the actual Runtime check (program callback count1), not the describe fixture.
Update Ack commits CPU state; the bounded pixel observation handles prior in-flight frames and uses
the existing +/-1 UNORM conversion tolerance. Archived failed attempt observed127 while its new waiting
predicate incorrectly required128; full pixel assertions were never removed. Production Frame/Queue,
IPC publication and Retirement algorithms are unchanged. Historical Runtime fixtures now supply the
actual Camera ABI; Native standalone generic parameter tests remain intact.

View uniquely owns CameraState. Runtime validates once, updates prepared CPU parameters and viewport on
the owner thread; Native setViewport requires that validated caller. Recording has no new Schema/name
lookup or validation. Resize preserves state. Context size344 (previous320); Binding32, Retirement104
and10k record ledger1,343,640 bytes/61 allocations remain recorded by existing cost tests. This is size
evidence, not a claim of faster execution. Offscreen stable-frame allocation/count regression is retained.

All public headers and fresh relocated SDK consumers pass, including pure Client without Vulkan and
pure synchronous configuration without stdexec. Exact-I three-prefix sync:75 headers,13 obsolete names,
264 backup/hash records. Android include sync is not Android build qualification.
Protection checker passes719 Legacy blobs and3 protected user files in source and clone; historical
reports/checkers unchanged. Commands: archive run.ps1 Source=E:\RenderV2Rebuild\V12\n6-camera-verification,
Build=E:/RenderV2Rebuild/V12/n6-camera-build/a, Actions snapshot/configure/build/build/test.

Camera checkpoint PASS in this scope. Instance group membership/indirect integration, BasicMesh and
DepthOnly, combined cold installation, full3D sample and N7 remain unfinished. No overall N6/N7 PASS.
Performance/IPC PARTIAL, root TOOLCHAIN historical BLOCKED; upper Engine/Editor/Linux/Android/TSan
NOT_RUN, GPU timestamp NOT_MEASURED. No culling/WSI/HDR qualification.
Portable `E:\RenderV2Rebuild\V12\archive\432ab9979de31748ff3f6a77a17b192e9c649c17\n6-camera-evidence.zip` SHA-256 `5ec14ba01f9b61fc5c39fdd861f7ee1923ed771ac9f7c99948e3edc7fbd068b4`; complete logs, failures, generator/tool evidence, SDK backups
and exact source diff indexed in N6-CAMERA-evidence-index.json. V contains no production repairs.
