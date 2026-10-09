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
    };

    std::map<VkBuffer, Allocation> live;
    unsigned attempts{};
    unsigned reject_at{};
    unsigned unmapped_at{};

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
            if (attempts == unmapped_at)
            {
                mapped->pMappedData = nullptr;
            }
        }
        return result;
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
#include <lux/engine/render/renderer/features/TransientVertexRing.hpp>
#include "../../vulkan/src/gpu/memory/VmaTypes.cpp"
#include "../../vulkan/src/gpu/lifecycle/DeferredDestroyQueue.cpp"
#include "../src/renderer/features/gizmo/TriOverlayTransientFeature.cpp"
#include "../src/renderer/features/gizmo/LineListTransientFeature.cpp"
#include "../src/renderer/features/point_cloud/PCFeatureTransient.cpp"
#undef vmaDestroyBuffer
#undef vmaCreateBuffer
#include "../../vulkan/src/gpu/VulkanContext.cpp"
// clang-format on

template <class Feature>
void checkFeature(lux::render::RenderScene& scene, lux::render::DeferredDestroyQueue& retirement)
{
    using namespace lux::render;
    typename Feature::Config config;
    for (unsigned failure = 1; failure <= 2; ++failure)
    {
        attempts = 0;
        reject_at = failure;
        const auto failed = scene.addFeature<Feature>(config);
        assert(!failed && isError<err::memory::GpuAllocationFailed>(failed.error()));
        assert(live.empty());
    }
    reject_at = 0;
    const auto accepted = scene.addFeature<Feature>(config);
    assert(accepted && live.size() == 2);
    const auto pending = retirement.pendingCount();
    retirement.beginFrame(100);
    assert(scene.removeFeature(*accepted));
    assert(!scene.getFeature(*accepted));
    assert(live.size() == 2 && retirement.pendingCount() == pending + 2);
    retirement.collect(99);
    assert(live.size() == 2);
    retirement.collect(100);
    assert(live.empty());
}

int main()
{
    using namespace lux::render;
    static_assert(!std::is_default_constructible_v<TransientVertexRing>);
    static_assert(!std::is_copy_constructible_v<TransientVertexRing>);
    static_assert(!std::is_copy_assignable_v<TransientVertexRing>);
    static_assert(std::is_nothrow_move_constructible_v<TransientVertexRing>);
    static_assert(std::is_nothrow_move_assignable_v<TransientVertexRing>);
    static_assert(std::is_nothrow_destructible_v<TransientVertexRing>);
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    auto instance = InstanceContext::create({});
    assert(instance);
    auto device = DeviceContext::create(**instance, EPhysicalDeviceSelectionPolicy::DISCRETE_GPU_PREFERRED);
    assert(device);
    auto resources = ResourceContext::create(**device);
    assert(resources);
    const auto allocator = (**resources).vmaAllocator();
    {
        auto accepted = TransientVertexRing::create(allocator, 2, 1024);
        assert(accepted && accepted->size() == 2 && live.size() == 2);
        const auto first = accepted->slotAt(0).buffer.buffer();
        auto* const mapping = accepted->slotAt(0).mapped;
        std::memset(mapping, 0x5a, 1024);
        accepted->flush(0, 1024);
        for (const bool missing_mapping : {false, true})
        {
            for (unsigned failure = 1; failure <= 3; ++failure)
            {
                attempts = 0;
                reject_at = missing_mapping ? 0 : failure;
                unmapped_at = missing_mapping ? failure : 0;
                auto candidate = TransientVertexRing::create(allocator, 3, 2048);
                assert(!candidate);
                assert(isError<err::memory::GpuAllocationFailed>(candidate.error()));
                assert(live.size() == 2);
                assert(accepted->slotAt(0).buffer.buffer() == first);
                assert(accepted->slotAt(0).mapped == mapping);
                assert(static_cast<unsigned char*>(mapping)[1023] == 0x5a);
            }
        }
        reject_at = unmapped_at = 0;
        auto candidate = TransientVertexRing::create(allocator, 4, 2048);
        assert(candidate && live.size() == 6);
        *accepted = std::move(*candidate);
        assert(accepted->size() == 4 && live.size() == 4 && !live.contains(first));
        for (unsigned frame = 0; frame != 64; ++frame)
        {
            assert(accepted->slotIndexFor(frame) == frame % 4);
        }
        auto moved = std::move(*accepted);
        assert(moved.size() == 4 && live.size() == 4);
        DeferredDestroyQueue retirement(**device);
        retirement.beginFrame(12);
        std::move(moved).retireInto(
            [&](VkBuffer buffer, VmaAllocation allocation) noexcept
            {
                retirement.retireBuffer(buffer, allocation);
            }
        );
        assert(live.size() == 4 && retirement.pendingCount() == 4);
        retirement.collect(11);
        assert(live.size() == 4 && retirement.pendingCount() == 4);
        retirement.collect(12);
        assert(live.empty() && retirement.pendingCount() == 0);
        retirement.collect(12);
        assert(live.empty());
    }
    assert(live.empty());
    {
        auto fallback = TransientVertexRing::create(allocator, 0, 512);
        assert(fallback && fallback->size() == 1 && live.size() == 1);
        assert(fallback->slotIndexFor(99) == 0);
    }
    assert(live.empty());
    {
        auto layouts = GeneralDescriptorSetLayout::create(**device);
        assert(layouts);
        auto registry = std::make_unique<ResourceRegistry>();
        registry->ensure<ShaderResources>((**device).logicalDevice(), false);
        RenderContext::CreateInfo
            info{std::make_unique<PipelineManager>(**device, true), std::move(*layouts), std::move(registry), 2};
        auto context = RenderContext::create(**resources, std::move(info));
        assert(context);
        auto scene = RenderScene::create(*context);
        assert(scene);
        auto& retirement = (*context)->deferredDestroyQueue();
        checkFeature<TriOverlayTransientFeature>(**scene, retirement);
        checkFeature<LineListTransientFeature>(**scene, retirement);
        checkFeature<PCFeatureTransient>(**scene, retirement);
        std::puts("actual Scene installs: triangle/line/point native prefix faults and delayed removal PASS");
    }
    assert(live.empty());
    std::puts("native prefix faults=6; old backing preserved; move/replace/retire/fallback PASS; live=0");
}
