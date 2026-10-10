#include <lux/engine/render/gpu/pipeline/EngineSetShapes.hpp>
#include <lux/engine/render/renderer/features/light/LightFeature.hpp>

#include <lux/engine/render/gpu/RenderContext.hpp>
#include <lux/engine/render/gpu/descriptor/SceneDomainDescriptorSets.hpp> // domain-set dual-write target
#include <lux/engine/render/gpu/pipeline/GeneralDescriptorSetLayout.hpp>  // descriptorLayouts().getLightSetLayout()
#include <lux/engine/render/gpu/transfer/TransferContributor.hpp>         // makeTransferContributor
#include <lux/engine/render/resources/lighting/LightResources.hpp>
#include <lux/engine/render/scene/RenderScene.hpp>

#include <utility>

namespace lux::render
{
    LightFeature::LightFeature(Config cfg) : RenderFeature(RenderFeature::Config{std::move(cfg.name)}) {}

    lux::render::Expected<void> LightFeature::initAndAttachTo(RenderScene& sc)
    {
        auto& reg = sc.resources();
        if (reg.find<LightResources>())
        {
            return {}; // Shared scene resource and its hooks are installed once.
        }
        auto& ctx = renderContext();

        LightResources::CreateInfo li{};
        li.ssbo_config = SSBOInitConfig{
            .device_context = &ctx.deviceContext(),
            .deferred_queue = &ctx.deferredDestroyQueue(),
            .initial_dense_capacity = 256,
            .slices = ctx.framesInFlight(),
            .clear_on_remove = true,
        };
        // b11 default shading-input texture (no PARTIALLY_BOUND — must be
        // written before the first bind; the buffer configuration identifies the device).
        li.descriptor_svc = &ctx.descriptorService();
        // The Light set lives in the FEATURE domain, with a nonzero offset
        // (+2, to skip past Instance's two bindings). The offset value comes
        // from an engine-level constant, sourced from the same place as the
        // domain layout.
        if (auto* domains = sc.domainDescriptorSets())
        {
            li.domain_sets = domains->setsFor(rdesc::EBindFrequency::FEATURE);
            li.domain_binding_offset = engineSetDomainOffset(static_cast<uint32_t>(EDescriptorSetSlot::LIGHT));
        }
        // 光照资源建不起来,这个 feature 就无法有意义地工作 —— 装上一个「场景永远无光」
        // 的 LightFeature 只会让问题在别处以「东西不见了」的形式出现,该由上层决定要不
        // 要退而求其次。create/insert 失败时**什么都没发布**,find<LightResources>()
        // 仍为空,不会留下一个谁也删不掉的半成品。
        auto candidate = LightResources::create(li);
        if (!candidate)
        {
            return lux::cxx::unexpected(candidate.error());
        }
        auto* light_res = reg.insert(std::move(*candidate)).get();

        // 每帧维护由**安装点**登记 —— 资源自己不再继承帧接口。登记必须在
        // insert 成功**之后**:此前它写在 init 之前,而"失败即不发布"意味着失败对象
        // 会被销毁 —— 早登记的钩子捕获的裸指针就成了每帧一次的 use-after-free
        // (注册表没有 removeBeginFrameHook)。
        reg.addBeginFrameHook(EUploadPhase::UPLOAD, [light_res](const FrameStamp& s) {
            light_res->onFrameBeginMaintenance(s);
        });

        // Register the light SSBO as a per-scene transfer contributor (priority 6)
        // so its slices flush before this scene's draw passes bind set 3. The scene
        // ctor used to do this; the OWNER does it now.
        // WithPost: the post-transfer hook does the one-time clear + layout
        // transition of the b11 default texture on the first frame's cmd.
        sc.transferScheduler().contributors().add(makeTransferContributorWithPost(light_res, /*priority=*/6));
        return {};
    }

    void LightFeature::onFrameBegin(const FeatureFrameContext& /*context*/)
    {
        auto& scene = renderScene();
        if (auto* lights = scene.resources().find<LightResources>())
        {
            lights->advanceIntensityTransitions(scene.maintenanceTime());
        }
    }

    void LightFeature::onDetachFromScene(RenderScene& /*sc*/)
    {
        // LightResources lifetime is owned by the scene registry (torn down at
        // scene teardown, in reverse registration order — after consumers like
        // ShadowResources that hold a raw LightResources*); nothing to do here.
    }

    bool LightFeature::canRebaseSceneOrigin(const std::int64_t origin_delta[3]) const noexcept
    {
        const auto* lights = renderScene().resources().find<LightResources>();
        return lights == nullptr || lights->canRebaseSceneOrigin(origin_delta);
    }

    void LightFeature::rebaseSceneOrigin(const std::int64_t origin_delta[3]) noexcept
    {
        if (auto* lights = renderScene().resources().find<LightResources>())
        {
            lights->rebaseSceneOrigin(origin_delta);
        }
    }

    // (原先这里有一个空的 addPasses:本单元只拥有光照**数据**,不产 render-graph
    //  pass —— 光照计算在 DeferredLighting / Forward 这些消费者里。现在它继承
    //  RenderFeature,不再被迫实现 addPasses。)

} // namespace lux::render
