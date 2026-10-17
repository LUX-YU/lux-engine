# Offscreen three-dimensional Client / Host example

`Host.cpp` owns one Core ExecutionRuntime and RenderServer. It installs BasicMeshFeature and
DepthOnlyFeature through their Native providers; those providers install the necessary Storage/Command
dependencies atomically. The Host adds a real alternative Texture handler without changing Server logic.
The application owns the Render thread and joins it before releasing the Server and scheduler.

`Client.cpp` is a separate object target linked only to RenderClient and neutral Feature contracts. It
has no Vulkan/Native headers and rejects accidental Native platform includes at compile time. Main and
Asset are two long-lived Clients. Asset uploads one tetrahedron Mesh, one cooked textured Unlit Material
and one Texture. Two Scene instances share those assets. Two Views use different Camera matrices,
Effects and Targets; numeric color and depth readbacks verify their real GPU output.

The example demonstrates typed Command binding, immutable Feature configuration, Storage discovery and
placement, batched Last-Wins transform updates with returned vector backing, explicit Attachment leases,
and Server-global handler selection seen by an already bound second Client. Updating Texture alone keeps
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
