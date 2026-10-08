#if defined(LUX_SHADOW_NATIVE_FAULTS)
#include "shadow_native.hpp"
#endif

#include <lux/engine/render/gpu/RenderContext.hpp>
#include <lux/engine/render/gpu/pipeline/GeneralDescriptorSetLayout.hpp>
#include <lux/engine/render/gpu/pipeline/PipelineManager.hpp>
#include <lux/engine/render/renderer/features/light/LightFeature.hpp>
#include <lux/engine/render/renderer/features/shadow/EVSMShadowTechnique.hpp>
#include <lux/engine/render/renderer/features/shadow/ShadowMapFeature.hpp>
#include <lux/engine/render/resources/ShaderResources.hpp>
#include <lux/engine/render/resources/lighting/ShadowResources.hpp>
#include <lux/engine/render/scene/RenderScene.hpp>

#include <cassert>
#include <cstdio>
#include <memory>

int main()
{
    using namespace lux::render;
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    auto instance_owner = InstanceContext::create({});
    assert(instance_owner);
    auto& instance = **instance_owner;
    auto device_owner = DeviceContext::create(instance, EPhysicalDeviceSelectionPolicy::DISCRETE_GPU_PREFERRED);
    assert(device_owner);
    auto& device = **device_owner;
    auto resources = ResourceContext::create(device);
    auto layouts = GeneralDescriptorSetLayout::create(device);
    assert(resources && layouts);
    auto registry = std::make_unique<ResourceRegistry>();
    registry->ensure<ShaderResources>(device.logicalDevice(), false);
    RenderContext::CreateInfo
        info{std::make_unique<PipelineManager>(device, true), std::move(*layouts), std::move(registry), 2};
    auto context = RenderContext::create(**resources, std::move(info));
    assert(context);
    {
        auto made_scene = RenderScene::create(*context);
        assert(made_scene);
        auto& scene = **made_scene;
        assert(scene.addFeature<LightFeature>());
        ShadowMapFeature::Config config;
        config.shadow_config.atlas_page_resolution = 16;
        config.shadow_config.atlas_page_count = 1;
        config.shadow_config.evsm_atlas_page_count = 1;
        config.shadow_config.max_shadow_slices = 4;
        config.shadow_config.default_technique = EShadowTechnique::EVSM;
        auto installed = scene.addFeature<ShadowMapFeature>(config);
        assert(installed);
        auto& feature = *scene.getFeatureAs<ShadowMapFeature>(*installed);
        auto& shadow = scene.resources().must<ShadowResources>();
        assert(feature.currentTechnique().id() == EShadowTechnique::EVSM);
        const auto& original = static_cast<const EVSMShadowTechnique&>(feature.currentTechnique()).resources();
        assert(original.pageResolution() == 16 && shadow.atlasPageResolution() == 16);
        const auto* technique_identity = &feature.currentTechnique();
#if defined(LUX_SHADOW_NATIVE_FAULTS)
        const auto original_atlas = shadow.atlasImage();
        const auto original_evsm = original.momentImage();
        const auto original_params = *static_cast<const ShadowQualityParams*>(feature.paramData());
        const auto original_technique = shadow.currentTechnique();
        const ShadowSliceGPU slice{};
        shadow.setCachedData(1, 1, std::span(&slice, 1), {}, {}, {}, 1, 0);
        const auto cached = shadow.findViewCache(1, 1);
        ShadowQualityParams desired = original_params;
        desired.atlas_page_resolution = 32;
        desired.atlas_page_count = 2;
        desired.max_shadow_slices = 8;
        for (const auto [boundary, count] :
             {std::pair{shadow_fault::EBoundary::IMAGE, 3u},
              std::pair{shadow_fault::EBoundary::VIEW, 3u},
              std::pair{shadow_fault::EBoundary::BUFFER, 10u},
              std::pair{shadow_fault::EBoundary::MAPPING, 10u},
              std::pair{shadow_fault::EBoundary::FLUSH, 12u},
              std::pair{shadow_fault::EBoundary::IDLE, 1u}})
        {
            for (unsigned index = 0; index != count; ++index)
            {
                shadow_fault::boundary = boundary;
                shadow_fault::skip = index;
                const auto rejected = shadow_fault::rejected;
                const auto writes = shadow_fault::writes;
                const auto direct = feature.updateQuality(32, 2, 8, 40.0f);
                assert(!direct && isError<err::device::VulkanCallFailed>(direct.error()));
                const auto expected_error =
                    boundary == shadow_fault::EBoundary::IDLE ? VK_ERROR_DEVICE_LOST
                    : boundary == shadow_fault::EBoundary::MAPPING || boundary == shadow_fault::EBoundary::FLUSH
                        ? VK_ERROR_MEMORY_MAP_FAILED
                        : VK_ERROR_OUT_OF_DEVICE_MEMORY;
                assert(direct.error().args[0] == encodeVkResult(expected_error));
                shadow_fault::skip = index;
                assert(feature.applyParams(&desired, sizeof(desired)) == RenderFeature::EParamApply::REJECTED);
                assert(shadow_fault::rejected == rejected + 2 && shadow_fault::writes == writes);
                shadow_fault::boundary = shadow_fault::EBoundary::NONE;
                assert(shadow.atlasImage() == original_atlas && shadow.atlasPageResolution() == 16);
                assert(shadow.currentTechnique() == original_technique);
                assert(original.momentImage() == original_evsm && original.pageResolution() == 16);
                assert(shadow.findViewCache(1, 1) == cached);
                const auto& params = *static_cast<const ShadowQualityParams*>(feature.paramData());
                assert(params.atlas_page_resolution == original_params.atlas_page_resolution);
                assert(params.atlas_page_count == original_params.atlas_page_count);
                assert(params.max_shadow_slices == original_params.max_shadow_slices);
                (*context)->deferredDestroyQueue().flushAll();
            }
        }
        std::puts("Actual applyParams: all coupled native failures retain both atlases, bindings, cache and params");
#endif
        auto changed = feature.updateQuality(32, 2, 8, 40.0f);
        assert(changed && *changed);
        const auto& current = static_cast<const EVSMShadowTechnique&>(feature.currentTechnique()).resources();
        std::printf(
            "Actual ShadowMapFeature quality adoption: depth=%u/%u EVSM=%u/%u expected=32/2\n",
            shadow.atlasPageResolution(),
            shadow.atlasPageCount(),
            current.pageResolution(),
            current.pageCount()
        );
        assert(current.pageResolution() == shadow.atlasPageResolution());
        assert(current.pageCount() >= shadow.atlasPageCount());
        assert(&feature.currentTechnique() == technique_identity);
        const auto accepted_image = current.momentImage();
        const auto unchanged = feature.updateQuality(0, 0, 0, -1);
        assert(unchanged && !*unchanged);
        assert(current.momentImage() == accepted_image);
        feature.setActiveTechnique(EShadowTechnique::PCF);
        const auto inactive_change = feature.updateQuality(64, 3, 12, -1);
        assert(inactive_change && *inactive_change);
        feature.setActiveTechnique(EShadowTechnique::EVSM);
        assert(&feature.currentTechnique() == technique_identity);
        const auto& inactive_updated = static_cast<const EVSMShadowTechnique&>(feature.currentTechnique()).resources();
        assert(inactive_updated.pageResolution() == 64 && inactive_updated.pageCount() == 3);
        assert(scene.removeFeature(*installed));
        assert(shadow.currentTechnique() == nullptr);
#if defined(LUX_SHADOW_NATIVE_FAULTS)
        const auto retained_atlas = shadow.atlasImage();
        const auto retained_writes = shadow_fault::writes;
        shadow_fault::boundary = shadow_fault::EBoundary::BUFFER;
        shadow_fault::skip = 2; // EVSM is complete; depth replacement must still be allowed to reject.
        const auto rejected_install = scene.addFeature<ShadowMapFeature>(config);
        shadow_fault::boundary = shadow_fault::EBoundary::NONE;
        assert(!rejected_install && isError<err::device::VulkanCallFailed>(rejected_install.error()));
        assert(shadow.atlasImage() == retained_atlas && shadow.atlasPageResolution() == 64);
        assert(shadow.currentTechnique() == nullptr && shadow_fault::writes == retained_writes);
        (*context)->deferredDestroyQueue().flushAll();
#endif
        auto replacement = scene.addFeature<ShadowMapFeature>(config);
        assert(replacement);
        auto& reinstalled = *scene.getFeatureAs<ShadowMapFeature>(*replacement);
        const auto& restored = static_cast<const EVSMShadowTechnique&>(reinstalled.currentTechnique()).resources();
        assert(shadow.atlasPageResolution() == 16 && shadow.atlasPageCount() == 1);
        assert(restored.pageResolution() == 16 && restored.pageCount() == 1);
        assert(scene.removeFeature(*replacement));
        assert(shadow.currentTechnique() == nullptr);
        std::puts("Inactive EVSM update, no-op identity, feature removal and configured reinstallation PASS");
    }
    assert(device.waitIdle() == VK_SUCCESS);
}
