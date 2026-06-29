#include <lux/engine/render/renderer/features/meshstack/StandardMeshStackFeature.hpp>
#include <lux/engine/render/renderer/features/meshstack/MeshStackOperation.hpp>

#include <lux/engine/render/scene/RenderScene.hpp>
#include <lux/engine/render/RendererContext.hpp>
#include <lux/engine/render/core/VertexLayoutTypes.hpp>          // kDefaultVertexLayoutId
#include <lux/engine/render/resources/mesh/InstanceResources.hpp>
#include <lux/engine/render/resources/mesh/MeshResources.hpp>
#include <lux/engine/render/resources/vertex/VertexPoolRegistry.hpp>
#include <lux/engine/render/resources/vertex/MeshInputPool.hpp>
#include <lux/engine/render/resources/vertex/VertexProduction.hpp>
#include <lux/engine/render/transfer/TransferContributor.hpp>     // makeTransferContributor

#include <iostream>
#include <utility>

namespace lux::render
{
    // Exported by RenderServer.cpp: lazily builds the GLOBAL 96MB mesh arena
    // (moved out of RenderServer::init so a scene that never adds this feature
    // pays nothing). Forward-declared to avoid pulling the server header in.
    void ensureGlobalMeshResources(RenderContext& ctx);

    StandardMeshStackFeature::StandardMeshStackFeature(Config cfg)
        : RenderFeature(RenderFeature::Config{std::move(cfg.name)})
    {}

    void StandardMeshStackFeature::initAndAttachTo(RenderScene& sc)
    {
        // Own the per-scene 3D mesh-stack resources the RenderScene ctor used to
        // emplace unconditionally. ensure<T>: whoever attaches first builds them;
        // a second mesh feature gets the same instances. Order matters —
        // VertexPoolRegistry MUST precede MeshInputPool (the latter holds a pointer
        // to it, and reverse-order teardown must drop MeshInputPool first).
        auto& reg = sc.sceneRegistry();
        auto& ctx = renderContext();

        // (0) GLOBAL mesh-resource arena (vertex 64MB + index 32MB). Built lazily
        //     here (or at the first uploadMesh) instead of unconditionally in
        //     RenderServer::init — adding this feature IS the opt-in to the arena.
        //     Idempotent + shared across scenes (lives in the global registry).
        ensureGlobalMeshResources(ctx);

        // (1) Compute-vertex producer registry (skinning/morph/cloth) — no init,
        //     no Vulkan state; producers publish into it in their own init().
        reg.ensure<VertexProductionRegistry>();

        // (2) Bindless vertex-source array (descriptor set 7). Init failure is
        //     non-fatal: set 7 stays unavailable, meshes won't render, rest is fine.
        {
            const bool fresh = (reg.find<VertexPoolRegistry>() == nullptr);
            auto* vpr = reg.ensure<VertexPoolRegistry>();
            if (fresh && !vpr->init(ctx.deviceContext(), ctx.descriptorService(), sc.descriptorArena()))
                std::cerr << "[StandardMeshStack] VertexPoolRegistry init failed; "
                             "meshes will not render.\n";
        }

        // (3) Global static VBO exposed as a bindless pool entry. Needs (2) + the
        //     GLOBAL MeshResources; lazy — the VBO may not exist yet at scene create.
        {
            const bool fresh = (reg.find<MeshInputPool>() == nullptr);
            auto* mip = reg.ensure<MeshInputPool>();
            if (fresh)
            {
                auto* vpr      = reg.find<VertexPoolRegistry>();
                auto* mesh_res = ctx.globalRegistry().find<MeshResources>();
                if (vpr && mesh_res)
                {
                    MeshInputPool::InitInfo ii{};
                    ii.vertex_pool_registry = vpr;
                    ii.mesh_resources       = mesh_res;
                    ii.layout_id            = kDefaultVertexLayoutId;
                    ii.vbo_segment          = 0;
                    (void)mip->init(ii);
                }
            }
        }

        // (4) Per-scene instance streams (transform/cullmeta/property/mdc). init()
        //     is deferred — the mesh features call it idempotently at their attach.
        //     Here we carry only the one-time side effects the scene ctor did:
        //     the deferred-destroy queue + the transfer contributor (priority 0).
        {
            const bool fresh = (reg.find<InstanceResources>() == nullptr);
            auto* inst = reg.ensure<InstanceResources>();
            if (fresh)
            {
                inst->setDeferredQueue(&ctx.deferredDestroyQueue());
                sc.transferScheduler().contributors().add(
                    makeTransferContributor(inst, /*priority=*/0));
            }
        }
    }

    void StandardMeshStackFeature::onDetachFromScene(RenderScene& /*sc*/)
    {
        // Resources are owned by the scene registry, torn down in reverse
        // registration order at scene teardown — after the mesh-draw features that
        // cached raw pointers into them (this feature registers first → tears down
        // last). Nothing to do here.
    }

    void StandardMeshStackFeature::addPasses(RGBuilder& /*builder*/)
    {
        // Resource owner only — no render-graph passes. The mesh-draw passes live
        // in ForwardMesh / DeferredGBuffer / MeshShadow (consumers of the resources
        // this feature owns).
    }

    // The factory (kStandardMeshStackFeatureFactory) + its createFn + the 8
    // feature-scoped instance ops live in MeshStackOperationHandlers.cpp, next to
    // the handlers their register_ops_fn binds (the grid / light layout).

} // namespace lux::render
