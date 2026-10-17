# N6.5 real two-Client 3D application and explicit output selection

Parent `2525db78f8948f099f84005db7345742e801e744`. Production example `modules/function/render/examples/basic_mesh` is a runnable
Host and separate neutral Client object target. One existing Core ExecutionRuntime schedules work;
Host owns Render thread. Main and Asset own two long-lived Clients. No additional scheduler, queue,
request Owner, Graph, Storage, or upload implementation. The installed SDK consumer builds/runs the
same source after prefix relocation, with copied/hash-checked cooked fragment from the existing
MaterialGraph/EngineShaderEmitter tool. Client compiles without Native/Vulkan/OS includes or link inputs.

Asset creates one tetrahedron Mesh, one textured Cooked Unlit Material and Texture. Two instances share
them; two Views/Camera matrices/Effects/Targets use real BasicMesh and DepthOnly providers. Assertions
check actual RGBA and D32 pixels, shared Storage IDs, typed batch Last-Wins, original vector address and
capacity, and surviving geometry after one View detaches and one instance is deleted. Global Texture
Command selection switches from A to B; the already bound Asset Client invokes B exactly once. Texture
replacement preserves Material's pinned version until SetMaterial publishes its new selection; the
Graph compilation count stays two. No claim of full culling, WSI/HDR or Engine/ECS integration.

ReadbackRequest now owns optional exported buffer name plus the existing Attachment identity. Empty
name retains explicit Target-copy semantics. Runtime resolves a name only at Command handling, checks
the buffer export and Reply capacity before defer, and passes its compiled index to the existing Native
ReadbackSender. No per-frame lookup, new Pending kind, implicit copy or separate GPU callback Owner.
Unknown key, non-readable image and insufficient Reply capacity fail before deferred work. Existing
typed test callers migrate directly; no shim or weakened original assertions. A real returned pixels
Owner remains readable after Clients/Server/scheduler destruction. Device shutdown still drains real tickets.

Implementation Release all -j4 -- -k0, second no-work; 145/145 tests in 173.34s. Prior143
and original102 preserved. Targeted camera/source sample/relocated sample passed3/3. Validation and
SyncValidation Error0; existing EOS warning retained. Independent exact-I ASan and SDK sync follow.
77 public headers, with Readback contracts changed; precise backed-up three-prefix synchronization
must use committed I. Android header sync does not qualify an Android build.

This completes the N6 minimal actual consumer shape, pending independent checkpoint. N7 explicit
execution outcomes, measured waiting/fairness, cost attribution, cleanup and final R/D/ASan qualification
remain. Native/Alias/IPC historical performance stays PARTIAL; product Root closure BLOCKED; upper
Engine/Editor/Linux/Android/TSan NOT_RUN. No performance improvement or GPU timestamp claim.

During final diff review a user formatting edit appeared in the current implementation checkout's
RenderServer.hpp (SHA-256 80a98f9e08ff0eb18b5f6ae0b3dea862f59384b71d9d36efe9c041acbec957df).
It is excluded from I and preserved byte-for-byte. The next construction checkout moves to source-final;
independent V and SDK use committed source only. check_sample_protection.py extends, without modifying,
the prior protection checker. The existing source checkout remains at I with that user file dirty.
