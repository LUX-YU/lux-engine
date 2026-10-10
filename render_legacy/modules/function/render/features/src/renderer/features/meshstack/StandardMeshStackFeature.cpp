#include <lux/engine/function/render/features/genops/MeshStackOperation.ops.hpp>
#include <lux/engine/render/gpu/pipeline/EngineSetShapes.hpp>
#include <lux/engine/render/renderer/features/meshstack/StandardMeshStackFeature.hpp>

#include <lux/engine/function/render/features/core/VertexLayoutTypes.hpp> // kDefaultVertexLayoutId
#include <lux/engine/render/gpu/RenderContext.hpp>
#include <lux/engine/render/gpu/descriptor/SceneDomainDescriptorSets.hpp> // domain-set dual-write target
#include <lux/engine/render/gpu/transfer/TransferContributor.hpp>         // makeTransferContributor
#include <lux/engine/render/renderer/features/meshstack/MeshInstanceAssembly.hpp>
#include <lux/engine/render/resources/material/MaterialResources.hpp>
#include <lux/engine/render/resources/mesh/InstanceResources.hpp>
#include <lux/engine/render/resources/mesh/MeshResources.hpp>
#include <lux/engine/render/resources/vertex/StaticVertexPoolSet.hpp>
#include <lux/engine/render/resources/vertex/VertexPoolRegistry.hpp>
#include <lux/engine/render/resources/vertex/VertexProduction.hpp>
#include <lux/engine/render/scene/RenderScene.hpp>

#include <algorithm>
#include <utility>

namespace lux::render
{
    //(ensureGlobalMeshResources 的前向声明已删 —— 它现在住在 L3 的
    // resources/mesh/MeshResources.hpp,本 TU 已 include 那个头,直接调用即可。
    //
    // 此前这里写的是 "Exported by RenderServer.cpp" —— 注释早就过时了(它后来
    // 搬到了 L6 的 assembly),而**前向声明恰恰让编译器无法告诉我们这件事**:
    // 它把一条 L4→L6 向上两层的链接期依赖藏成了一行看不出问题的声明。)

    StandardMeshStackFeature::StandardMeshStackFeature(Config cfg)
        : RenderFeature(RenderFeature::Config{std::move(cfg.name)})
    {}

    lux::render::Expected<void> StandardMeshStackFeature::initAndAttachTo(RenderScene& sc)
    {
        // Own the per-scene 3D mesh-stack resources the RenderScene ctor used to
        // emplace unconditionally. ensure<T>: whoever attaches first builds them;
        // a second mesh feature gets the same instances. Order matters —
        // VertexPoolRegistry MUST precede StaticVertexPoolSet (the latter holds a
        // reference to it, and reverse-order teardown must drop the set first).
        auto& reg = sc.resources();
        auto& ctx = renderContext();

        // (0) GLOBAL mesh-resource arena (vertex 64MB + index 32MB). Built lazily
        //     here (or at the first uploadMesh) instead of unconditionally in
        //     RenderServer::init — adding this feature IS the opt-in to the arena.
        //     Idempotent + shared across scenes (lives in the global registry).
        if (auto ready = ensureGlobalMeshResources(ctx); !ready)
        {
            return ready;
        }

        // (1) Compute-vertex producer registry (skinning/morph/cloth) — no init,
        //     no Vulkan state; producers publish into it in their own init().
        reg.ensure<VertexProductionRegistry>();

        // (2) Bindless vertex-source array (descriptor set 7).
        auto* vpr = reg.find<VertexPoolRegistry>();
        if (!vpr)
        {
            auto* domains = sc.domainDescriptorSets();
            const auto sets =
                domains ? domains->setsFor(rdesc::EBindFrequency::FEATURE) : std::span<const VkDescriptorSet>{};
            auto candidate = VertexPoolRegistry::create(
                ctx.deviceContext(),
                sets,
                engineSetDomainOffset(static_cast<uint32_t>(EDescriptorSetSlot::VERTEX_POOL)),
                ctx.errorSink()
            );
            if (!candidate)
            {
                return lux::cxx::unexpected(candidate.error());
            }
            vpr = reg.insert(std::move(*candidate)).get();
        }

        // (3) Mandatory dependencies are complete above; construction only binds their lifetime.
        reg.ensure<StaticVertexPoolSet>(*vpr, ctx.globalRegistry().must<MeshResources>());

        // (4) Publish only complete streams and descriptor targets. The sole installer
        // registers maintenance/upload once; a rejected factory leaves no registry entry.
        if (!reg.find<InstanceResources>())
        {
            const auto instance_capacity = ctx.capacityPlan().effective(lux::render::kActiveInstancesCapacity);
            const bool is_invalid_capacity = instance_capacity == 0u || instance_capacity > 0xffffffffull;
            if (is_invalid_capacity)
            {
                return renderFailure<err::internal::InvalidArgument>();
            }
            const auto* domains = sc.domainDescriptorSets();
            const auto sets =
                domains ? domains->setsFor(rdesc::EBindFrequency::FEATURE) : std::span<const VkDescriptorSet>{};
            InstanceResources::CreateInfo info{ctx.deviceContext(), ctx.deferredDestroyQueue(), sets};
            info.domain_binding_offset = engineSetDomainOffset(static_cast<uint32_t>(EDescriptorSetSlot::INSTANCE));
            info.max_capacity = static_cast<std::uint32_t>(instance_capacity);
            info.sparse_bda = ctx.capacityPlan().device.buffer_device_address && ctx.capacityPlan().device.shader_int64;
            info.initial_capacity =
                info.sparse_bda ? std::min(info.max_capacity, kInstanceSlotsPerPage) : info.max_capacity;
            info.coordinate_page_size = sc.spatialTileSize();
            auto candidate = InstanceResources::create(info);
            if (!candidate)
            {
                return lux::cxx::unexpected(candidate.error());
            }
            auto* instances = reg.insert(std::move(*candidate)).get();
            reg.addBeginFrameHook(
                EUploadPhase::UPLOAD,
                [instances](const FrameStamp& stamp) { instances->onFrameBeginMaintenance(stamp); }
            );
            sc.transferScheduler().contributors().add(makeTransferContributor(instances, /*priority=*/0));
        }
        return {};
    }

    void StandardMeshStackFeature::onFrameBegin(const FeatureFrameContext& /*context*/)
    {
        auto& scene = renderScene();
        auto* instances = scene.resources().find<InstanceResources>();
        if (!instances)
        {
            return;
        }
        for (const auto object : instances->collectExpiredFadeRetirements(scene.maintenanceTime()))
        {
            detail::destroyMeshInstance(scene, renderContext(), object);
        }
    }

    void StandardMeshStackFeature::retainSubmissions(const FrameRuntime& frame) const noexcept
    {
        if (const auto* instances = renderScene().resources().find<InstanceResources>())
            instances->retainSubmissions(frame);
    }

    void StandardMeshStackFeature::onDetachFromScene(RenderScene& sc)
    {
        // Scene teardown bypasses future frame hooks, so finish the ownership
        // transaction synchronously on the render owner: detach every instance
        // pin before the scene registry destroys InstanceResources. GPU memory
        // retirement itself remains FIF-deferred by the global resource owners.
        auto* instances = sc.resources().find<InstanceResources>();
        if (!instances)
        {
            return;
        }
        auto* materials = renderContext().globalRegistry().find<MaterialResources>();
        auto* meshes = renderContext().globalRegistry().find<MeshResources>();
        for (const auto binding : instances->takeAllResources())
        {
            if (materials)
            {
                materials->releaseFromInstance(binding.material);
            }
            if (meshes)
            {
                meshes->releaseFromInstance(binding.mesh);
            }
        }
    }

    bool StandardMeshStackFeature::canRebaseSceneOrigin(const std::int64_t origin_delta[3]) const noexcept
    {
        const auto* instances = renderScene().resources().find<InstanceResources>();
        return instances == nullptr || instances->canRebaseSceneOrigin(origin_delta);
    }

    void StandardMeshStackFeature::rebaseSceneOrigin(const std::int64_t origin_delta[3]) noexcept
    {
        if (auto* instances = renderScene().resources().find<InstanceResources>())
        {
            instances->rebaseSceneOrigin(origin_delta);
        }
    }

    // (原先这里有一个空的 addPasses:本单元只拥有资源,不产 render-graph pass ——
    //  网格绘制的 pass 在 ForwardMesh / DeferredGBuffer / MeshShadow 里,它们是这些
    //  资源的消费者。现在它继承 RenderFeature,不再被迫实现 addPasses。)

    // The factory (kMeshStackFeatureFactory) + its createFn + the 8
    // feature-scoped instance ops live in MeshStackOperationHandlers.cpp, next to
    // the handlers their register_ops_fn binds (the grid / light layout).

} // namespace lux::render
