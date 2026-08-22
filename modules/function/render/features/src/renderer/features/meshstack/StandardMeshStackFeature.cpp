#include <lux/engine/render/renderer/features/meshstack/StandardMeshStackFeature.hpp>
#include <lux/engine/function/render/client/genops/MeshStackOperation.ops.hpp>

#include <lux/engine/render/scene/RenderScene.hpp>
#include <lux/engine/render/gpu/RenderContext.hpp>
#include <lux/engine/function/render/client/core/VertexLayoutTypes.hpp>          // kDefaultVertexLayoutId
#include <lux/engine/render/resources/mesh/InstanceResources.hpp>
#include <lux/engine/render/resources/mesh/MeshResources.hpp>
#include <lux/engine/render/resources/vertex/VertexPoolRegistry.hpp>
#include <lux/engine/render/resources/vertex/StaticVertexPoolSet.hpp>
#include <lux/engine/render/resources/vertex/VertexProduction.hpp>
#include <lux/engine/render/resources/material/MaterialResources.hpp>
#include <lux/engine/render/gpu/transfer/TransferContributor.hpp>     // makeTransferContributor
#include <lux/engine/render/gpu/descriptor/SceneDomainDescriptorSets.hpp>   // domain-set dual-write target
#include <lux/engine/render/renderer/features/meshstack/MeshInstanceAssembly.hpp>

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

    lux::render::Expected<void> StandardMeshStackFeature::initAndAttachTo(RenderScene& sc){
        // Own the per-scene 3D mesh-stack resources the RenderScene ctor used to
        // emplace unconditionally. ensure<T>: whoever attaches first builds them;
        // a second mesh feature gets the same instances. Order matters —
        // VertexPoolRegistry MUST precede StaticVertexPoolSet (the latter holds a
        // pointer to it, and reverse-order teardown must drop the set first).
        auto& reg = sc.sceneRegistry();
        auto& ctx = renderContext();

        // (0) GLOBAL mesh-resource arena (vertex 64MB + index 32MB). Built lazily
        //     here (or at the first uploadMesh) instead of unconditionally in
        //     RenderServer::init — adding this feature IS the opt-in to the arena.
        //     Idempotent + shared across scenes (lives in the global registry).
        if (auto ready = ensureGlobalMeshResources(ctx); !ready)
            return ready;

        // (1) Compute-vertex producer registry (skinning/morph/cloth) — no init,
        //     no Vulkan state; producers publish into it in their own init().
        reg.ensure<VertexProductionRegistry>();

        // (2) Bindless vertex-source array (descriptor set 7).
        const bool fresh = (reg.find<VertexPoolRegistry>() == nullptr);
        auto vpr_r = reg.ensure<VertexPoolRegistry>(
            ctx.deviceContext(), ctx.descriptorService(), sc.descriptorArena());
        if (!vpr_r)
            return lux::cxx::unexpected<RenderError>(vpr_r.error());
        auto* vpr = *vpr_r;
        if (fresh)
        {
            // 注册表满是每帧的仲裁结果,没有调用方可以处置 —— 接上自发上报通道。
            vpr->setErrorSink(ctx.errorSink());
            // The vertex pool lives in the FEATURE domain, with an offset of
            // +23 (placed after Instance/Light/Material/Particle/Compute).
            if (auto* domains = sc.domainDescriptorSets())
            {
                const auto accepted = vpr->setDomainWriteTarget(
                    domains->setsFor(rdesc::EBindFrequency::FEATURE),
                    engineSetDomainOffset(
                        static_cast<uint32_t>(EDescriptorSetSlot::VertexPool)));
                if (!accepted)
                    return accepted;
            }
        }

        // (3) Global static VBO exposed as a bindless pool entry. Needs (2) + the
        //     GLOBAL MeshResources.
        //     两个依赖此处**必然齐备**:(2) 刚成功返回,(0) 的
        //     ensureGlobalMeshResources 成功即意味着全局注册表里有一个已初始化的
        //     MeshResources。原先的 `if (vpr && mesh_res)` 因此是死守卫,而它的
        //     else 分支——把一个未初始化的 StaticVertexPoolSet 留在注册表里——正是
        //     ensure<T>(init_args) 要消灭的形状,所以改成 must<>。
        auto mip_r = reg.ensure<StaticVertexPoolSet>(StaticVertexPoolSet::InitInfo{
            .vertex_pool_registry = vpr,
            .mesh_resources       = &ctx.globalRegistry().must<MeshResources>(),
        });
        if (!mip_r)
            return lux::cxx::unexpected<RenderError>(mip_r.error());

        // (4) Per-scene instance streams (transform/cullmeta/property/mdc).
        //     This feature is the sole ensure<>-er, so it is also the one that
        //     init()s. It used to DEFER init to whichever mesh feature attached
        //     next, which meant a scene carrying only StandardMeshStack (no
        //     GPU-driven mesh, no shadow) left the resource permanently
        //     uninitialized: allocateObject() then returned invalid, the mesh-stack
        //     handler read that as CapacityExhausted, and the bridge retried it
        //     forever under backoff — silently. Deferring also made the effective
        //     capacity depend on WHICH feature attached first, since init() is
        //     first-one-wins.
        //     现在连"忘了 init"这件事本身都写不出来了:InstanceResources 声明了
        //     init(),ensure<InstanceResources>() 的无参形态编译不过。
        const bool fresh_inst = (reg.find<InstanceResources>() == nullptr);
        InstanceResources::InitInfo si{};
        si.device_context   = &ctx.deviceContext();
        si.descriptor_svc   = &ctx.descriptorService();
        si.arena            = &sc.descriptorArena();
        const auto instance_capacity = ctx.capacityPlan().effective(
            lux::render::kActiveInstancesCapacity);
        if (instance_capacity == 0u || instance_capacity > 0xffffffffull)
            return renderFailure<err::internal::Unspecified>();
        si.max_capacity = static_cast<std::uint32_t>(instance_capacity);
        si.sparse_bda = ctx.capacityPlan().device.buffer_device_address &&
            ctx.capacityPlan().device.shader_int64;
        // BDA scenes materialize one 16K physical page and grow page-by-page;
        // the root descriptor remains stable. Legacy keeps the admitted flat
        // buffer allocation because it cannot dereference the page table.
        si.initial_capacity = si.sparse_bda
            ? std::min(si.max_capacity, kInstanceSlotsPerPage)
            : si.max_capacity;
        si.coordinate_page_size = sc.spatialTileSize();
        auto inst_r = reg.ensure<InstanceResources>(si);
        if (!inst_r)
            return lux::cxx::unexpected<RenderError>(inst_r.error());
        auto* inst = *inst_r;
        if (fresh_inst)
        {
            // 每帧维护由**安装点**登记 —— 资源自己不再继承帧接口。写在
            // fresh_inst 守卫内:InstanceResources 是 ensure<> 出来的,第二个
            // 网格单元会拿到同一个实例,登记若在守卫外就会每帧驱动两次。
            reg.addBeginFrameHook(EUploadPhase::Upload,
                                  [inst](const FrameStamp& s) { inst->onFrameBeginMaintenance(s); });
            inst->setDeferredQueue(&ctx.deferredDestroyQueue());
            sc.transferScheduler().contributors().add(
                makeTransferContributor(inst, /*priority=*/0));
        }

        // Wiring is idempotent and belongs outside the fresh-resource branch.
        // A failed feature-install transaction can leave the resource object in
        // the scene registry while the contribution is retried; skipping this
        // step on retry leaves the FEATURE-domain Instance bindings forever
        // unwritten. Re-applying the target also refreshes both descriptors.
        if (auto* domains = sc.domainDescriptorSets())
        {
            const auto accepted = inst->setDomainWriteTarget(
                domains->setsFor(rdesc::EBindFrequency::FEATURE),
                engineSetDomainOffset(
                    static_cast<uint32_t>(EDescriptorSetSlot::Instance)));
            if (!accepted)
                return accepted;
        }
        return {};
    }

    void StandardMeshStackFeature::onFrameBegin(
        const FeatureFrameContext& /*context*/)
    {
        auto& scene = renderScene();
        auto* instances = scene.sceneRegistry().find<InstanceResources>();
        if (!instances)
            return;
        for (const auto object :
             instances->collectExpiredFadeRetirements(scene.sceneTime()))
        {
            detail::destroyMeshInstance(scene, renderContext(), object);
        }
    }

    void StandardMeshStackFeature::onDetachFromScene(RenderScene& sc)
    {
        // Scene teardown bypasses future frame hooks, so finish the ownership
        // transaction synchronously on the render owner: detach every instance
        // pin before the scene registry destroys InstanceResources. GPU memory
        // retirement itself remains FIF-deferred by the global resource owners.
        auto* instances = sc.sceneRegistry().find<InstanceResources>();
        if (!instances)
            return;
        auto* materials = renderContext().globalRegistry().find<
            MaterialResources>();
        auto* meshes = renderContext().globalRegistry().find<MeshResources>();
        for (const auto binding : instances->takeAllResources())
        {
            if (materials)
                materials->releaseFromInstance(binding.material);
            if (meshes)
                meshes->releaseFromInstance(binding.mesh);
        }
    }

    bool StandardMeshStackFeature::canRebaseSceneOrigin(
        const std::int64_t origin_delta[3]) const noexcept
    {
        const auto* instances = renderScene().sceneRegistry().find<
            InstanceResources>();
        return instances == nullptr ||
            instances->canRebaseSceneOrigin(origin_delta);
    }

    void StandardMeshStackFeature::rebaseSceneOrigin(
        const std::int64_t origin_delta[3]) noexcept
    {
        if (auto* instances = renderScene().sceneRegistry().find<
                InstanceResources>())
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
