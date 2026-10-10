#define VMA_IMPLEMENTATION
#include <lux/engine/render/gpu/VulkanContext.hpp>
#include <vk_mem_alloc.h>

#include <cassert>
#include <cstdio>
#include <cstring>
#include <lux/engine/render/gpu/pipeline/GeneralDescriptorSetLayout.hpp>
#include <map>
#include <type_traits>

namespace
{
    struct Allocation
    {
        VmaAllocator allocator{};
        VmaAllocation allocation{};
        bool operator==(const Allocation&) const = default;
    };

    std::map<VkBuffer, Allocation> live;
    unsigned attempts{};
    unsigned reject_at{};
    unsigned mapping_attempts{}, reject_mapping_at{};
    bool reject_flush{};

    VkResult trackedCreate(
        VmaAllocator allocator,
        const VkBufferCreateInfo* info,
        const VmaAllocationCreateInfo* allocation_info,
        VkBuffer* buffer,
        VmaAllocation* allocation,
        VmaAllocationInfo* mapped
    )
    {
        if (++attempts == reject_at)
        {
            return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        }
        const auto result = vmaCreateBuffer(allocator, info, allocation_info, buffer, allocation, mapped);
        if (result == VK_SUCCESS)
        {
            assert(live.emplace(*buffer, Allocation{allocator, *allocation}).second);
            if (mapped && ++mapping_attempts == reject_mapping_at)
            {
                mapped->pMappedData = nullptr;
            }
        }
        return result;
    }

    VkResult trackedFlush(VmaAllocator allocator, VmaAllocation allocation, VkDeviceSize offset, VkDeviceSize size)
    {
        return reject_flush ? VK_ERROR_MEMORY_MAP_FAILED : vmaFlushAllocation(allocator, allocation, offset, size);
    }

    void trackedDestroy(VmaAllocator allocator, VkBuffer buffer, VmaAllocation allocation)
    {
        const auto found = live.find(buffer);
        assert(found != live.end());
        assert(found->second.allocator == allocator && found->second.allocation == allocation);
        live.erase(found);
        vmaDestroyBuffer(allocator, buffer, allocation);
    }
} // namespace

// Compile the actual owners and retirement algorithm; only their native boundary
// is instrumented. Every successful allocation is a real Vulkan/VMA buffer.
// clang-format off
#define vmaCreateBuffer trackedCreate
#define vmaDestroyBuffer trackedDestroy
#define vmaFlushAllocation trackedFlush
#include "../../vulkan/src/gpu/memory/VmaTypes.cpp"
#include "../../vulkan/src/gpu/lifecycle/DeferredDestroyQueue.cpp"
#include "../src/renderer/features/shadow/MeshShadowFeature.cpp"
#undef vmaFlushAllocation
#undef vmaDestroyBuffer
#undef vmaCreateBuffer
#include "../../vulkan/src/gpu/VulkanContext.cpp"
#include "../src/renderer/features/light/LightFeature.cpp"
// clang-format on

#include <lux/engine/render/gpu/descriptor/SceneDomainDescriptorSets.hpp>
#include <lux/engine/render/gpu/pipeline/EngineSetShapes.hpp>
#include <lux/engine/render/renderer/features/light/LightFeature.hpp>
#include <lux/engine/render/renderer/features/shadow/ShadowMapFeature.hpp>

int main()
{
    using namespace lux::render;
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    auto instance = InstanceContext::create({});
    assert(instance);
    auto device = DeviceContext::create(**instance, EPhysicalDeviceSelectionPolicy::DISCRETE_GPU_PREFERRED);
    assert(device);
    auto resources = ResourceContext::create(**device);
    assert(resources);
    auto layouts = GeneralDescriptorSetLayout::create(**device);
    assert(layouts);
    auto registry = std::make_unique<ResourceRegistry>();
    registry->ensure<ShaderResources>((**device).logicalDevice(), false);
    RenderContext::CreateInfo
        info{std::make_unique<PipelineManager>(**device, true), std::move(*layouts), std::move(registry), 2};
    RenderErrorSink errors;
    auto context = RenderContext::create(**resources, std::move(info));
    assert(context);
    (*context)->setErrorSink(&errors);
    auto scene = RenderScene::create(*context);
    assert(scene);
    auto& retirement = (*context)->deferredDestroyQueue();
    auto* domains = (*scene)->domainDescriptorSets();
    assert(domains);
    InstanceResources::CreateInfo instances_info{
        **device,
        retirement,
        domains->setsFor(lux::rdesc::EBindFrequency::FEATURE)
    };
    instances_info.domain_binding_offset = engineSetDomainOffset(static_cast<uint32_t>(EDescriptorSetSlot::INSTANCE));
    instances_info.initial_capacity = 16;
    instances_info.max_capacity = 32;
    auto instances = InstanceResources::create(instances_info);
    assert(instances);
    (*scene)->resources().insert(std::move(*instances));
    {
        MeshShadowFeature detached(MeshShadowFeature::Config{});
        const auto rejected = detached.initAndAttachTo(**scene);
        assert(!rejected && isError<err::feature::AttachmentNotAuthorized>(rejected.error()));
    }
    for (unsigned failure = 1; failure <= 2 * kMaxFramesInFlight; ++failure)
    {
        attempts = 0;
        reject_at = failure;
        const auto rejected = (*scene)->addFeature<MeshShadowFeature>(MeshShadowFeature::Config{});
        assert(!rejected && isError<err::device::VulkanCallFailed>(rejected.error()));
        assert(rejected.error().args[0] == encodeVkResult(VK_ERROR_OUT_OF_DEVICE_MEMORY));
        assert(attempts == failure && live.empty());
    }
    reject_at = 0;
    for (unsigned failure = 1; failure <= kMaxFramesInFlight; ++failure)
    {
        mapping_attempts = 0;
        reject_mapping_at = failure;
        const auto rejected = (*scene)->addFeature<MeshShadowFeature>(MeshShadowFeature::Config{});
        assert(!rejected && isError<err::memory::GpuAllocationFailed>(rejected.error()));
        assert(mapping_attempts == failure && live.empty());
    }
    reject_mapping_at = 0;
    assert((*scene)->addFeature<LightFeature>());
    ShadowMapFeature::Config shadow_config;
    shadow_config.shadow_config.atlas_page_resolution = 16;
    shadow_config.shadow_config.atlas_page_count = 1;
    shadow_config.shadow_config.max_shadow_slices = 4;
    shadow_config.shadow_config.default_technique = EShadowTechnique::PCF;
    const auto shadow_id = (*scene)->addFeature<ShadowMapFeature>(shadow_config);
    assert(shadow_id);
    auto& shadow_map = *(*scene)->getFeatureAs<ShadowMapFeature>(*shadow_id);
    auto& shadow = (*scene)->resources().must<ShadowResources>();
    const auto accepted = (*scene)->addFeature<MeshShadowFeature>(MeshShadowFeature::Config{});
    assert(accepted && live.size() == 2 * kMaxFramesInFlight);
    auto& feature = *(*scene)->getFeatureAs<MeshShadowFeature>(*accepted);
    const auto original = live;
    const auto repeated = feature.initAndAttachTo(**scene);
    assert(!repeated && isError<err::feature::AttachmentNotAuthorized>(repeated.error()));
    assert(live == original);
    assert(shadow_map.updateQuality(32, 1, 8, -1));
    retirement.beginFrame(100);
    const auto pending = retirement.pendingCount();
    for (unsigned failure = 1; failure <= kMaxFramesInFlight; ++failure)
    {
        errors.clear();
        reject_at = attempts + failure;
        feature.onFrameBegin({1});
        assert(live == original && retirement.pendingCount() == pending);
        assert(errors.pending().size() == 1);
        assert(isError<err::device::VulkanCallFailed>(errors.pending()[0].error));
        assert(errors.pending()[0].error.args[0] == encodeVkResult(VK_ERROR_OUT_OF_DEVICE_MEMORY));
    }
    reject_at = 0;
    errors.clear();
    feature.onFrameBegin({1});
    assert(errors.empty() && live.size() == 3 * kMaxFramesInFlight);
    assert(retirement.pendingCount() == pending + kMaxFramesInFlight);
    retirement.collect(99);
    assert(live.size() == 3 * kMaxFramesInFlight);
    retirement.collect(100);
    assert(live.size() == 2 * kMaxFramesInFlight);

    const auto view = (*scene)->addView({{8, 8}, "mesh-shadow-native"});
    assert(view);
    std::array<ShadowSliceGPU, kMaxShadowBiasGroups> slices{};
    for (uint32_t index = 0; index != slices.size(); ++index)
    {
        slices[index].bias = static_cast<float>(index);
    }
    shadow.setCachedData((*scene)->sceneGlobalSlot().index, view->index, slices, {}, {}, {}, 1, 0);
    auto& table = (*scene)->resources().must<InstanceResources>().mdcTable();
    for (uint32_t index = 0; index != 4096; ++index)
    {
        table.registerInstance(0, 0, index, 0, VK_INDEX_TYPE_UINT32);
    }
    table.buildOffsets();
    const auto before_growth = live;
    const auto growth_pending = retirement.pendingCount();
    for (const bool missing_mapping : {false, true})
    {
        errors.clear();
        reject_at = missing_mapping ? 0 : attempts + 1;
        reject_mapping_at = missing_mapping ? mapping_attempts + 1 : 0;
        feature.onFrameBegin({0});
        assert(live == before_growth && retirement.pendingCount() == growth_pending);
        assert(errors.pending().size() == 1);
        assert(feature.shadowFrameData().bias_group_count == 0);
        assert(feature.shadowFrameExtData().group_count == 0);
        assert(feature.shadowFrameExtData().frustum_data == nullptr);
        if (missing_mapping)
        {
            assert(isError<err::memory::GpuAllocationFailed>(errors.pending()[0].error));
        }
        else
        {
            assert(isError<err::device::VulkanCallFailed>(errors.pending()[0].error));
        }
    }
    reject_at = reject_mapping_at = 0;
    retirement.beginFrame(200);
    errors.clear();
    feature.onFrameBegin({0});
    assert(errors.empty() && live.size() == 2 * kMaxFramesInFlight + 1);
    assert(feature.shadowFrameData().bias_group_count == kMaxShadowBiasGroups);
    assert(feature.shadowFrameExtData().group_count == kMaxShadowBiasGroups);
    const auto cull_slot0 = feature.shadowFrameData().shadow_cull_ubo;
    const auto stable_attempts = attempts;
    feature.onFrameBegin({0});
    assert(attempts == stable_attempts && feature.shadowFrameData().shadow_cull_ubo == cull_slot0);
    retirement.collect(199);
    assert(live.size() == 2 * kMaxFramesInFlight + 1);
    retirement.collect(200);
    assert(live.size() == 2 * kMaxFramesInFlight);

    errors.clear();
    reject_flush = true;
    feature.onFrameBegin({0});
    assert(errors.pending().size() == 1);
    assert(errors.pending()[0].error.args[0] == encodeVkResult(VK_ERROR_MEMORY_MAP_FAILED));
    assert(feature.shadowFrameData().bias_group_count == 0 && feature.shadowFrameExtData().frustum_data == nullptr);
    reject_flush = false;
    feature.onFrameBegin({0});
    assert(feature.shadowFrameData().bias_group_count == kMaxShadowBiasGroups);
    feature.onFrameBegin({1});
    assert(feature.shadowFrameData().shadow_cull_ubo != cull_slot0);
    retirement.collect(200);
    const auto complete = live;
    retirement.beginFrame(300);
    assert((*scene)->removeFeature(*accepted));
    assert(!(*scene)->getFeature(*accepted) && live == complete);
    retirement.collect(299);
    assert(live == complete);
    retirement.collect(300);
    assert(live.empty());
    std::puts(
        "MeshShadow actual install6/mapping3 faults, quality3 faults, MDC native/mapping/flush failures, retry, "
        "engine slot and delayed detach PASS"
    );
}
