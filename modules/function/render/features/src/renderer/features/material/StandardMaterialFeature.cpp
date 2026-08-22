#include <lux/engine/render/renderer/features/material/StandardMaterialFeature.hpp>
#include <lux/engine/function/render/client/genops/MaterialOperation.ops.hpp>

#include <lux/engine/render/scene/RenderScene.hpp>
#include <lux/engine/render/gpu/RenderContext.hpp>
#include <lux/engine/render/resources/material/MaterialResources.hpp>   // ensureGlobalMaterialResources
#include <lux/engine/render/gpu/descriptor/SceneDomainDescriptorSets.hpp>
#include <lux/engine/render/gpu/pipeline/EngineSetShapes.hpp>
#include <lux/engine/function/render/client/core/RenderFatal.hpp>

#include <utility>

namespace lux::render
{
    //(ensureGlobalMaterialResources 的前向声明已删 —— 现住 L3 的
    // resources/material/MaterialResources.hpp。理由同 StandardMeshStackFeature.cpp。)

    StandardMaterialFeature::StandardMaterialFeature(Config cfg)
        : RenderFeature(RenderFeature::Config{std::move(cfg.name)})
    {}

    lux::render::Expected<void> StandardMaterialFeature::initAndAttachTo(RenderScene& sc){
        // Own the global material stack (registry-first; MaterialResources::init
        // Idempotent + shared across scenes (lives
        // in the global registry). Adding this feature IS the opt-in to the stack —
        // moved out of RenderServer::init so a 2D / unlit / headless server pays
        // nothing.
        if (auto ready = ensureGlobalMaterialResources(renderContext()); !ready)
            return ready;
        // 补注册本场景为材质域双写目标(幂等):MaterialResources 惰性创建,
        // 首个建的场景在构造时 find 不到它、错过注册,其域 set 的材质绑定
        // 将永不写入——域合并管线读到全零(黑材质竞态,材质预览首发现场)。
        auto& materials = renderContext().globalRegistry().must<MaterialResources>();
        const auto accepted = materials.addDomainWriteTarget(
            &sc,
            sc.domainDescriptorSets()->setsFor(rdesc::EBindFrequency::FEATURE),
            engineSetDomainOffset(
                static_cast<uint32_t>(EDescriptorSetSlot::Material)
            )
        );
        if (!accepted)
            renderFatal("StandardMaterialFeature: material domain target is empty");
        return {};
    }

    void StandardMaterialFeature::onDetachFromScene(RenderScene& sc)
    {
        // The descriptor sets belong to the scene arena. Remove the global
        // material owner's borrowed write target before the arena is destroyed.
        if (auto* materials = renderContext().globalRegistry().find<MaterialResources>())
            materials->removeDomainWriteTarget(&sc);
    }

    // (原先这里有一个空的 addPasses:本单元只拥有资源,不产 render-graph pass ——
    //  材质 set-4 绑定与逐 family 管线在网格绘制的消费者里(DeferredGBuffer /
    //  ForwardMesh / Highlight)。现在它继承 RenderFeature,不再被迫实现 addPasses。)

    // The factory (kMaterialFeatureFactory) + its createFn + the 3
    // feature-scoped material ops live in MaterialOperationHandlers.cpp, next to
    // the handlers their register_ops_fn binds (the grid / light layout).

} // namespace lux::render
