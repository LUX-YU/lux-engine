#include <lux/engine/render/renderer/features/light/LightFeature.hpp>

#include <lux/engine/render/scene/RenderScene.hpp>
#include <lux/engine/render/RendererContext.hpp>
#include <lux/engine/render/pipeline/GeneralDescriptorSetLayout.hpp>  // descriptorLayouts().getLightSetLayout()
#include <lux/engine/render/resources/lighting/LightResources.hpp>
#include <lux/engine/render/transfer/TransferContributor.hpp>   // makeTransferContributor

#include <iostream>
#include <utility>

namespace lux::render
{
    LightFeature::LightFeature(Config cfg)
        : RenderFeature(RenderFeature::Config{std::move(cfg.name)})
    {}

    void LightFeature::initAndAttachTo(RenderScene& sc)
    {
        // Feature owns its scene resource (PointCloud/Trajectory pattern): emplace
        // LightResources here, NOT in the general RenderScene constructor. Must be
        // attached BEFORE the lighting consumers — ShadowMapFeature caches a raw
        // LightResources* at its own initAndAttachTo (the editor orders LightFeature
        // first in the feature list). Idempotent.
        // ensure<T>: whoever attaches first builds this scene's LightResources; a
        // second LightFeature receives the same instance. Only the first-time creator
        // runs init() + registers the transfer contributor below.
        auto& reg        = sc.sceneRegistry();
        const bool fresh = (reg.find<LightResources>() == nullptr);
        auto* light_res  = reg.ensure<LightResources>();
        if (!fresh)
            return;

        auto& ctx = renderContext();

        // Mirrors the per-scene init the core RenderScene ctor used to do: per-scene
        // SSBO + the SHARED light set layout (one handle so all pipelines stay
        // compatible; only the SETS/buffers are per-scene).
        LightResources::InitInfo li{};
        li.ssbo_config = SSBOInitConfig{
            .device_context         = &ctx.deviceContext(),
            .initial_dense_capacity = 256,
            .slices                 = ctx.framesInFlight(),
            .clear_on_remove        = true,
        };
        li.arena      = &sc.descriptorArena();
        li.set_layout = ctx.descriptorLayouts().getLightSetLayout();
        if (!light_res->init(li))
            std::cerr << "[LightFeature] LightResources init failed; scene unlit.\n";
        light_res->setDeferredQueue(&ctx.deferredDestroyQueue());

        // Register the light SSBO as a per-scene transfer contributor (priority 6)
        // so its slices flush before this scene's draw passes bind set 3. The scene
        // ctor used to do this; the OWNER does it now.
        sc.transferScheduler().contributors().add(
            makeTransferContributor(light_res, /*priority=*/6));
    }

    void LightFeature::onDetachFromScene(RenderScene& /*sc*/)
    {
        // LightResources lifetime is owned by the scene registry (torn down at
        // scene teardown, in reverse registration order — after consumers like
        // ShadowResources that hold a raw LightResources*); nothing to do here.
    }

    void LightFeature::addPasses(RGBuilder& /*builder*/)
    {
        // No render-graph passes: LightFeature only owns the light DATA. The
        // lighting COMPUTE happens in DeferredLighting/Forward (consumers).
    }

} // namespace lux::render
