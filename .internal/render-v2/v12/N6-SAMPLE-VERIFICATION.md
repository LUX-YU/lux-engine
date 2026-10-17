# N6.5 minimal production 3D consumer — independent checkpoint

Exact I `a77a7480fc03da76902f07cfbeaa96683531a15f`, parent `2525db78f8948f099f84005db7345742e801e744`, observed remote `78bcd220815f845466d85d31f994901e4db224ba`. No push.
Detached --no-hardlinks clean clone, no alternates; tracked snapshot before fresh configure, no source fix.
Windows MSVC ASan /fsanitize=address /Zi /GR-, ASAN_OPTIONS=alloc_dealloc_mismatch=1: **145/145**,
360.81s; all -j4 -- -k0 and second no-work, serial CTest. Prior143/original102 names retained.
Implementation Release145/145 173.34s is separate evidence. Final N7 R/D/ASan still required.
Validation/SyncValidation/ASan Error0; actual EOS duplicate-overlay warnings retained.

Source and relocated SDK sample both run one actual Core ExecutionRuntime, one RenderServer, Main and
Asset Clients, two shared-asset instances, two Cameras/Effects/Targets. Assertions verify tetrahedron
color and D32 outputs, unchanged compiled_graphs=2 after material selection updates, Texture Handler B
dispatch=1, typed batch vector address/capacity return and Last-Wins, detach/instance removal isolation,
and a 16,384-byte readback Owner surviving all participants. Client TU links neutral contracts only.
Assets use the existing cooked MaterialGraph shader generator; copied asset hashes checked for SDK use.

Readback adds one owned optional output key to the existing Attachment Request. Unknown key, existing
non-readable image, insufficient Reply capacity rejected before defer; named successful output and
normal Target readback continue through the same Pending/Native callback/Ticket ownership. Original
tests migrated their type arguments without dropping behavior. No implicit GPU copy or new Owner.

Three prefixes synchronized from exact I:77 public headers,13 obsolete paths, original backups/hashes.
Android synchronization is not a platform build. Source/clone protection:719 frozen Legacy blobs,
three original user files plus the newly observed source/RenderServer.hpp formatting edit unchanged.
That header was excluded from I; source-final and this clean clone contain only committed bytes.
Dependency versions and history unchanged. No source repairs in V.

N6 minimal resource/Feature/consumer gates PASS in this tested scope. N7 not complete: performance
attribution, explicit execution outcomes, wait/fairness and final cleanup/matrix remain. Historical
Native/Alias/IPC performance PARTIAL; Root product BLOCKED; upper Engine/Editor/Linux/Android/TSan
NOT_RUN. GPU timestamp performance not measured here. No claim of speedup or full product closure.
Raw commands/logs/samples/SDK backups are in `E:\RenderV2Rebuild\V12\archive\a77a7480fc03da76902f07cfbeaa96683531a15f\n6-sample-evidence.zip`, SHA-256 `10245970e5a25f3a6d99ec78bcbea8afe881e8608001c07ed644984c7e7fd555`; indexed alongside this
report. V adds only this report/index; final N7 qualification will reference all checkpoint SHAs.
