#define LUX_SCENE_NATIVE_FAULTS
#define LUX_HZB_NATIVE_FAULTS
#include "shadow_native.hpp"

#include <lux/engine/render/gpu/pipeline/GeneralDescriptorSetLayout.hpp>
#include <lux/engine/render/gpu/pipeline/PipelineManager.hpp>
#include <lux/engine/render/resources/ShaderResources.hpp>
#include <lux/engine/render/scene/View.hpp>

#include <cstdio>

int main()
{
    using namespace lux::render;
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    InstanceContext instance({});
    DeviceContext device(instance);
    assert(device.init(EPhysicalDeviceSelectionPolicy::DISCRETE_GPU_PREFERRED));
    auto resources = ResourceContext::create(device);
    auto layouts = GeneralDescriptorSetLayout::create(device);
    assert(resources && layouts);
    auto registry = std::make_unique<ResourceRegistry>();
    registry->ensure<ShaderResources>(device.logicalDevice(), false);
    RenderContext::CreateInfo
        info{std::make_unique<PipelineManager>(device, true), std::move(*layouts), std::move(registry), 2};
    RenderErrorSink errors;
    auto context = RenderContext::create(**resources, std::move(info));
    assert(context);
    (*context)->setErrorSink(&errors);
    auto made_scene = RenderScene::create(*context);
    assert(made_scene);
    auto& scene = **made_scene;
    const auto view = scene.addView({{8, 8}, "hzb-view"});
    auto installed = scene.addFeature<HzbFeature>();
    assert(installed);
    auto& feature = *scene.getFeatureAs<HzbFeature>(*installed);
    feature.onFrameBegin({});
    const auto* hzb = feature.resources();
    assert(hzb && hzb->viewReady(view.index) && hzb->width(view.index) == 8);
    // Resize changes the backend's cached target extent, as the normal renderer does.
    scene.getView(view)->current_extent = {16, 16};
    shadow_fault::boundary = shadow_fault::EBoundary::SET;
    shadow_fault::skip = 2; // Both read sets succeed; first build descriptor fails.
    feature.onFrameBegin({});
    shadow_fault::boundary = shadow_fault::EBoundary::NONE;
    std::printf(
        "Actual HzbFeature rejected resize: ready=%d extent=%u expected=8\n",
        hzb->viewReady(view.index),
        hzb->width(view.index)
    );
    assert(hzb->viewReady(view.index) && hzb->width(view.index) == 8);
    errors.clear();
    const auto old_descriptor = HzbResources::resolveHzbReadDS(hzb, 0, view.index);
    const auto old_parity = hzb->curIndex(view.index);
    (*context)->deferredDestroyQueue().flushAll();
    const auto retained_buffers = shadow_fault::live_buffers;
    unsigned checked = 0;
    for (const auto [boundary, count] :
         {std::pair{shadow_fault::EBoundary::IMAGE, 2u},
          std::pair{shadow_fault::EBoundary::VIEW, 12u},
          std::pair{shadow_fault::EBoundary::BUFFER, 2u},
          std::pair{shadow_fault::EBoundary::MAPPING, 2u},
          std::pair{shadow_fault::EBoundary::SET, 12u},
          std::pair{shadow_fault::EBoundary::IDLE, 1u},
          std::pair{shadow_fault::EBoundary::ALLOCATE_COMMAND, 1u},
          std::pair{shadow_fault::EBoundary::BEGIN_COMMAND, 1u},
          std::pair{shadow_fault::EBoundary::END_COMMAND, 1u},
          std::pair{shadow_fault::EBoundary::FENCE, 1u},
          std::pair{shadow_fault::EBoundary::SUBMIT, 1u},
          std::pair{shadow_fault::EBoundary::WAIT, 1u}})
    {
        for (unsigned occurrence = 0; occurrence < count; ++occurrence)
        {
            shadow_fault::boundary = boundary;
            shadow_fault::skip = occurrence;
            const auto rejected = shadow_fault::rejected;
            feature.onFrameBegin({});
            assert(shadow_fault::rejected == rejected + 1 && shadow_fault::skip == 0);
            assert(
                !shadow_fault::in_flight && shadow_fault::live_commands.empty() && shadow_fault::live_fences.empty()
            );
            assert(errors.pending().size() == 1 && errors.pending()[0].occurrences == 1);
            const auto& error = errors.pending()[0].error;
            assert(isError<err::device::VulkanCallFailed>(error));
            const auto expected = boundary == shadow_fault::EBoundary::IDLE      ? VK_ERROR_DEVICE_LOST
                                  : boundary == shadow_fault::EBoundary::MAPPING ? VK_ERROR_MEMORY_MAP_FAILED
                                                                                 : VK_ERROR_OUT_OF_DEVICE_MEMORY;
            assert(error.args[0] == encodeVkResult(expected));
            assert(hzb->viewReady(view.index) && hzb->width(view.index) == 8 && hzb->mipCount(view.index) == 4);
            assert(hzb->curIndex(view.index) == old_parity);
            assert(HzbResources::resolveHzbReadDS(hzb, 0, view.index) == old_descriptor);
            errors.clear();
            shadow_fault::boundary = shadow_fault::EBoundary::NONE;
            (*context)->deferredDestroyQueue().flushAll();
            assert(shadow_fault::live_buffers == retained_buffers);
            ++checked;
        }
    }
    assert(checked == 37);
    feature.onFrameBegin({});
    assert(hzb->viewReady(view.index) && hzb->width(view.index) == 16 && errors.empty());
    const auto second = scene.addView({{4, 2}, "hzb-second"});
    feature.onFrameBegin({});
    assert(hzb->viewReady(second.index) && hzb->width(second.index) == 4);
    assert(scene.removeView(second) && !hzb->viewReady(second.index));
    assert(hzb->viewReady(view.index));
    assert(scene.removeFeature(*installed));
    assert(!hzb->viewReady(view.index));
    // The scene must not retain callbacks borrowing the destroyed feature's mip-set map.
    assert(scene.removeView(view));
    const auto third = scene.addView({{4, 4}, "hzb-reinstalled"});
    const auto reinstalled = scene.addFeature<HzbFeature>();
    assert(reinstalled);
    scene.getFeatureAs<HzbFeature>(*reinstalled)->onFrameBegin({});
    assert(hzb->viewReady(third.index) && hzb->width(third.index) == 4);
    assert(scene.removeFeature(*reinstalled) && !hzb->viewReady(third.index));
    assert(scene.removeView(third));
    assert(device.waitIdle() == VK_SUCCESS);
    std::puts("Actual HzbFeature: 37 native failures, original accepted backing/parity, exact error delivery, retry, "
              "view cleanup and feature remove/reinstall PASS");
}
