#include <vulkan/vulkan.h>

#include <cassert>
#include <cstdio>
#include <map>

namespace
{
    std::map<VkSampler, VkDevice> samplers;
    bool reject_sampler{};
    unsigned attempts{};

    VkResult createSampler(
        VkDevice device,
        const VkSamplerCreateInfo* info,
        const VkAllocationCallbacks* callbacks,
        VkSampler* output
    )
    {
        ++attempts;
        if (reject_sampler)
        {
            return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        }
        const auto result = vkCreateSampler(device, info, callbacks, output);
        if (result == VK_SUCCESS)
        {
            assert(samplers.emplace(*output, device).second);
        }
        return result;
    }

    void destroySampler(VkDevice device, VkSampler sampler, const VkAllocationCallbacks* callbacks)
    {
        const auto found = samplers.find(sampler);
        assert(found != samplers.end() && found->second == device);
        samplers.erase(found);
        vkDestroySampler(device, sampler, callbacks);
    }
} // namespace

// Actual UI feature and renderer. Only the feature's Vulkan sampler boundary is
// intercepted; the ImGui backend creates and uploads real GPU font resources.
// clang-format off
#define vkCreateSampler createSampler
#define vkDestroySampler destroySampler
#include "../src/RenderFeature.cpp"
#undef vkDestroySampler
#undef vkCreateSampler
#include "../src/VulkanBackend.cpp"
// clang-format on

#include <lux/engine/render/gpu/pipeline/GeneralDescriptorSetLayout.hpp>
#include <lux/engine/render/gpu/pipeline/PipelineManager.hpp>

int main()
{
    using namespace lux::render;
    using UiFeature = lux::ui::detail::RenderFeature;
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    auto instance = InstanceContext::create({});
    assert(instance);
    auto device = DeviceContext::create(**instance, EPhysicalDeviceSelectionPolicy::DISCRETE_GPU_PREFERRED);
    assert(device);
    auto resources = ResourceContext::create(**device);
    auto layouts = GeneralDescriptorSetLayout::create(**device);
    assert(resources && layouts);
    RenderContext::CreateInfo info{
        std::make_unique<PipelineManager>(**device, true),
        std::move(*layouts),
        std::make_unique<ResourceRegistry>(),
        2
    };
    auto context = RenderContext::create(**resources, std::move(info));
    assert(context);
    auto scene = RenderScene::create(*context);
    assert(scene);
    const auto no_font = (*scene)->addFeature<UiFeature>(lux::ui::FontAtlas{});
    assert(!no_font && isError<err::feature::ResourceInitFailed>(no_font.error()));
    assert(attempts == 0 && samplers.empty());
    const lux::ui::FontAtlas font{{255, 255, 255, 255}, 1, 1};
    reject_sampler = true;
    const auto rejected = (*scene)->addFeature<UiFeature>(font);
    assert(!rejected && isError<err::device::VulkanCallFailed>(rejected.error()));
    assert(rejected.error().args[0] == encodeVkResult(VK_ERROR_OUT_OF_DEVICE_MEMORY));
    assert(attempts == 1 && samplers.empty());
    reject_sampler = false;
    for (unsigned round = 0; round != 8; ++round)
    {
        const auto accepted = (*scene)->addFeature<UiFeature>(font);
        assert(accepted && samplers.size() == 1);
        assert((*scene)->removeFeature(*accepted));
        assert(!(*scene)->getFeature(*accepted) && samplers.empty());
    }
    const auto final = (*scene)->addFeature<UiFeature>(font);
    assert(final && samplers.size() == 1);
    scene->reset();
    assert(samplers.empty());
    std::puts(
        "actual UI renderer/font: pre-sampler failure, native sampler failure, retry/remove8 and Scene teardown PASS"
    );
}
