# Offscreen three-dimensional Client / Host integration test

`Host.cpp` owns one Core ExecutionRuntime and RenderServer. It installs BasicMeshFeature and
DepthOnlyFeature through their Native providers; those providers install the necessary Storage/Command
dependencies atomically. The Host adds a real alternative Texture handler without changing Server logic.
The application creates the Server with `vulkan::createRenderServer(config, execution, deviceOptions)`.
The selected Vulkan header supplies typed installation definitions; `RenderServer.hpp` remains neutral.
The application owns the Render thread and joins it before releasing the Server and scheduler.
Host code modules remain loaded through message/config destruction and all callback tails; transport
and scheduling do not carry per-object module pins.

`Client.cpp` is a separate object target linked only to builtin Command values, IPC and neutral Feature contracts. It
has no Vulkan/Native headers and rejects accidental Native platform includes at compile time. Main and
Asset are two long-lived Clients. Asset uploads one tetrahedron Mesh, one cooked textured Unlit Material
and one Texture. Two Scene instances share those assets. Two Views use different Camera matrices,
Effects and Targets; numeric color and depth readbacks verify their real GPU output.

The Host supplies the real ListCommands ID. Clients query the directory with ordinary IpcMessage values,
assign their own RequestIds, and consume actual response messages. The example demonstrates immutable
Feature configuration, Storage discovery and placement, batched Last-Wins transform updates, explicit
Attachment leases, and Server-global handler selection seen by the second Client. Successful sends
transfer their input; retries use caller-owned authority rather than implicit original-request returns. Updating Texture alone keeps
the Material's previous version; a subsequent SetMaterial publishes the new Texture selection without
Shader/Pipeline compilation. Detaching one View leaves the other's assets and output intact. Cleanup
releases all logical uses; Native tickets retain accepted GPU work. One real readback payload survives
both Clients, Server and execution shutdown.

The fragment input must be cooked by the existing MaterialGraph/EngineShaderEmitter path. The Render
bootstrap produces `UnlitTexture.frag.spv` using the existing offline test asset generator and records
its provenance/hash. This executable neither contains a MaterialGraph parser nor invokes an Engine
product at runtime. Run `render_basic_mesh <path-to-UnlitTexture.frag.spv>`.

For installed use, configure this directory against the relocated SDK with `RENDER_TEST_ASSET_DIR`
pointing to the copied cooked asset directory, build `all -j 4 -- -k 0`, then run CTest. The internal
qualification also runs the same source as an independent installed consumer.

Scope: RGBA8/D32 offscreen Unlit geometry; no WSI/HDR, full GPU culling, ECS integration or product-level
performance qualification. DepthOnly's independently declared depth map has the Feature's configured
resolution; resizing its separate color Target does not change that map. Readback uses explicitly
declared Graph copy outputs, and the Client chooses a named exported buffer when it needs depth.
