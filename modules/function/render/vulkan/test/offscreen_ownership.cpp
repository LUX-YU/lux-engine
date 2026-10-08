#include <lux/engine/gapi/vk/vk.hpp>

#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>

#include <cassert>
#include <cstdio>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>

namespace
{
    struct ImageRecord
    {
        VmaAllocator allocator{};
        VmaAllocation allocation{};
    };

    struct ViewRecord
    {
        VkDevice device{};
        VkImage image{};
    };

    std::unordered_map<VkImage, ImageRecord> images;
    std::unordered_map<VkImageView, ViewRecord> views;
    unsigned attempts{}, fail_at{};

    VkResult createImage(
        VmaAllocator allocator,
        const VkImageCreateInfo* info,
        const VmaAllocationCreateInfo* allocation_info,
        VkImage* image,
        VmaAllocation* allocation,
        VmaAllocationInfo* output_info
    )
    {
        ++attempts;
        *image = VK_NULL_HANDLE;
        *allocation = nullptr;
        if (attempts == fail_at)
        {
            return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        }
        const auto result = vmaCreateImage(allocator, info, allocation_info, image, allocation, output_info);
        if (result == VK_SUCCESS)
        {
            assert(images.emplace(*image, ImageRecord{allocator, *allocation}).second);
        }
        return result;
    }

    void destroyImage(VmaAllocator allocator, VkImage image, VmaAllocation allocation)
    {
        const auto found = images.find(image);
        assert(found != images.end());
        assert(found->second.allocator == allocator && found->second.allocation == allocation);
        for (const auto& [view, record] : views)
        {
            assert(record.image != image);
        }
        images.erase(found);
        vmaDestroyImage(allocator, image, allocation);
    }

    VkResult createView(
        VkDevice device,
        const VkImageViewCreateInfo* info,
        const VkAllocationCallbacks* allocator,
        VkImageView* view
    )
    {
        ++attempts;
        *view = VK_NULL_HANDLE;
        if (attempts == fail_at)
        {
            return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        }
        assert(images.contains(info->image));
        const auto result = vkCreateImageView(device, info, allocator, view);
        if (result == VK_SUCCESS)
        {
            assert(views.emplace(*view, ViewRecord{device, info->image}).second);
        }
        return result;
    }

    void destroyView(VkDevice device, VkImageView view, const VkAllocationCallbacks* allocator)
    {
        const auto found = views.find(view);
        assert(found != views.end() && found->second.device == device);
        assert(images.contains(found->second.image));
        views.erase(found);
        vkDestroyImageView(device, view, allocator);
    }
} // namespace

// Real image/view allocations and the actual pool/candidate/retirement algorithms.
// clang-format off
#define vmaCreateImage createImage
#define vmaDestroyImage destroyImage
#include "../src/gpu/memory/VmaTypes.cpp"
#undef vmaDestroyImage
#undef vmaCreateImage
#define vkCreateImageView createView
#define vkDestroyImageView destroyView
#include "../src/gpu/VulkanContext.cpp"
#include "../src/targets/OffscreenImagePool.cpp"
#include "../src/renderer/RenderTargetRegistry.cpp"
#undef vkDestroyImageView
#undef vkCreateImageView
// clang-format on

int main()
{
    using namespace lux::render;
    InstanceContext instance({});
    DeviceContext device(instance);
    assert(device.init(EPhysicalDeviceSelectionPolicy::DISCRETE_GPU_PREFERRED));
    auto resources_owner = ResourceContext::create(device);
    assert(resources_owner);
    auto& resources = **resources_owner;

    static_assert(!std::is_default_constructible_v<OffscreenImagePool>);
    static_assert(!std::is_copy_constructible_v<OffscreenImagePool>);
    static_assert(!std::is_move_constructible_v<OffscreenImagePool>);

    RenderTargetLayout layout;
    layout.slots[static_cast<std::size_t>(ETargetSlot::SCENE_COLOR)] = RenderTargetSlotDesc{
        .format = lux::rdesc::ETextureFormat::RGBA8_UNORM,
        .usage = ERenderImageUsage::COLOR_ATTACHMENT | ERenderImageUsage::SAMPLED
    };
    layout.slots[static_cast<std::size_t>(ETargetSlot::SCENE_DEPTH)] = RenderTargetSlotDesc{
        .format = lux::rdesc::ETextureFormat::D32_SFLOAT,
        .usage = ERenderImageUsage::DEPTH_STENCIL_ATTACHMENT,
        .aspect = ERenderAspect::DEPTH
    };

    // Reject incomplete configuration before any native acquisition.
    attempts = 0;
    auto no_width = OffscreenImagePool::create(resources, layout, {0, 32}, 2);
    auto no_height = OffscreenImagePool::create(resources, layout, {32, 0}, 2);
    auto no_frames = OffscreenImagePool::create(resources, layout, {32, 32}, 0);
    assert(!no_width && isError<err::internal::InvalidArgument>(no_width.error()));
    assert(!no_height && isError<err::internal::InvalidArgument>(no_height.error()));
    assert(!no_frames && isError<err::internal::InvalidArgument>(no_frames.error()));
    auto invalid_layout = layout;
    invalid_layout.slots[0]->format = lux::rdesc::ETextureFormat::UNDEFINED;
    auto no_format = OffscreenImagePool::create(resources, invalid_layout, {32, 32}, 2);
    invalid_layout = layout;
    invalid_layout.slots[0]->usage = ERenderImageUsage::NONE;
    auto no_usage = OffscreenImagePool::create(resources, invalid_layout, {32, 32}, 2);
    assert(!no_format && isError<err::internal::InvalidArgument>(no_format.error()));
    assert(!no_usage && isError<err::internal::InvalidArgument>(no_usage.error()));
    assert(attempts == 0 && images.empty() && views.empty());

    for (unsigned boundary = 1; boundary <= 8; ++boundary)
    {
        attempts = 0;
        fail_at = boundary;
        {
            auto rejected = OffscreenImagePool::create(resources, layout, {32, 32}, 2);
            assert(!rejected && isError<err::device::VulkanCallFailed>(rejected.error()));
            assert(rejected.error().args[0] == encodeVkResult(VK_ERROR_OUT_OF_DEVICE_MEMORY));
            assert(attempts == boundary && images.empty() && views.empty());
        }
        assert(images.empty() && views.empty());
    }
    fail_at = 0;

    struct Notification
    {
        unsigned count{};
        std::unordered_set<VkImageView> retired;
    };

    const auto notification = std::make_shared<Notification>();
    {
        auto owner = OffscreenImagePool::create(resources, layout, {32, 32}, 2);
        assert(owner);
        auto& pool = **owner;
        assert(images.size() == 4 && views.size() == 4);
        pool.setViewRetirementObserver(
            notification,
            [](void* state, std::span<const VkImageView> retired) noexcept
            {
                auto& output = *static_cast<Notification*>(state);
                for (const auto view : retired)
                {
                    assert(views.contains(view)); // Notification precedes physical destruction.
                    output.retired.insert(view);
                    ++output.count;
                }
            }
        );
        const auto initial = pool.binding();
        assert(pool.resize({64, 64}));
        pool.collectRetired(10, 0);
        assert(notification->count == 0 && images.size() == 8 && views.size() == 8);
        assert(pool.resize({96, 96}));
        assert(images.size() == 12 && views.size() == 12);
        pool.collectRetired(11, 10);
        assert(notification->count == 4 && images.size() == 8 && views.size() == 8);
        for (const auto& slot : initial.slot_images)
        {
            for (const auto view : slot.views)
            {
                assert(notification->retired.contains(view) && !views.contains(view));
            }
        }

        const auto accepted = pool.binding();
        const auto revision = pool.backingRevision();
        for (unsigned boundary = 1; boundary <= 8; ++boundary)
        {
            attempts = 0;
            fail_at = boundary;
            auto rejected = pool.resize({128, 128});
            assert(!rejected && isError<err::device::VulkanCallFailed>(rejected.error()));
            assert(rejected.error().args[0] == encodeVkResult(VK_ERROR_OUT_OF_DEVICE_MEMORY));
            assert(pool.extent().width == 96 && pool.backingRevision() == revision);
            assert(images.size() == 8 && views.size() == 8 && notification->count == 4);
            for (std::size_t slot = 0; slot < kTargetSlotCount; ++slot)
            {
                assert(pool.binding().slot_images[slot].images == accepted.slot_images[slot].images);
                assert(pool.binding().slot_images[slot].views == accepted.slot_images[slot].views);
            }
        }
        fail_at = 0;
        pool.collectRetired(12, 10);
        assert(notification->count == 4 && images.size() == 8);
        pool.collectRetired(13, 11);
        assert(notification->count == 8 && images.size() == 4 && views.size() == 4);
    }
    assert(notification->count == 12 && images.empty() && views.empty());

    // Layout replacement prepares every attachment/FIF before changing the accepted pool.
    {
        auto owner = OffscreenImagePool::create(resources, layout, {8, 8}, 2);
        assert(owner);
        auto& pool = **owner;
        auto expanded = layout;
        expanded.slots[static_cast<std::size_t>(ETargetSlot::NORMAL)] = layout.slots[0];
        const auto accepted = pool.binding();
        assert(accepted.layout && accepted.layout->slots == layout.slots);
        assert(pool.backingRevision() == 1);
        pool.markRecorded(0);
        pool.markRecorded(1);
        attempts = 0;
        assert(pool.resize({8, 8}) && pool.applyLayout(layout));
        assert(attempts == 0 && pool.recorded(0) && pool.recorded(1));
        auto invalid_resize = pool.resize({0, 8});
        auto invalid_change = pool.applyLayout(invalid_layout);
        assert(!invalid_resize && isError<err::internal::InvalidArgument>(invalid_resize.error()));
        assert(!invalid_change && isError<err::internal::InvalidArgument>(invalid_change.error()));
        assert(attempts == 0 && pool.backingRevision() == 1);
        for (unsigned boundary = 1; boundary <= 12; ++boundary)
        {
            attempts = 0;
            fail_at = boundary;
            auto rejected = pool.applyLayout(expanded);
            assert(!rejected && isError<err::device::VulkanCallFailed>(rejected.error()));
            assert(rejected.error().args[0] == encodeVkResult(VK_ERROR_OUT_OF_DEVICE_MEMORY));
            assert(attempts == boundary && images.size() == 4 && views.size() == 4);
            assert(pool.layout().slots == layout.slots && pool.backingRevision() == 1);
            assert(pool.recorded(0) && pool.recorded(1));
            assert(pool.binding().layout == accepted.layout);
            for (std::size_t slot = 0; slot < kTargetSlotCount; ++slot)
            {
                assert(pool.binding().slot_images[slot].images == accepted.slot_images[slot].images);
                assert(pool.binding().slot_images[slot].views == accepted.slot_images[slot].views);
            }
            pool.collectRetired(19, 18);
            assert(images.size() == 4 && views.size() == 4);
        }
        fail_at = 0;
        assert(pool.applyLayout(expanded));
        assert(pool.backingRevision() == 2 && pool.layout().slots == expanded.slots);
        assert(!pool.recorded(0) && !pool.recorded(1));
        assert(images.size() == 10 && views.size() == 10);
        const auto frame = pool.makeFrameBinding(1);
        assert(frame.layout == pool.binding().layout && frame.extent.width == 8);
        for (std::size_t slot = 0; slot < kTargetSlotCount; ++slot)
        {
            if (expanded.slots[slot])
            {
                assert(frame.slot_images[slot].images.size() == 1);
                assert(frame.slot_images[slot].images[0] == pool.binding().slot_images[slot].images[1]);
            }
        }
        pool.collectRetired(20, 19);
        assert(images.size() == 10);
        pool.collectRetired(21, 20);
        assert(images.size() == 6 && views.size() == 6);
        RenderTargetLayout depth_only;
        depth_only.slots[static_cast<std::size_t>(ETargetSlot::SCENE_DEPTH)] =
            layout.slots[static_cast<std::size_t>(ETargetSlot::SCENE_DEPTH)];
        assert(pool.applyLayout(depth_only));
        assert(pool.layout().slots == depth_only.slots && pool.backingRevision() == 3);
        pool.collectRetired(22, 20);
        assert(images.size() == 8);
        pool.collectRetired(23, 22);
        assert(images.size() == 2 && views.size() == 2);
    }
    assert(images.empty() && views.empty());
    // An attachment-free layout remains legal; it owns no native resources.
    {
        auto empty = OffscreenImagePool::create(resources, {}, {8, 8}, 1);
        assert(empty && (*empty)->backingRevision() == 1);
        assert(images.empty() && views.empty());
    }
    // Actual registry producer and original target/serial ownership, without a server shim.
    {
        static_assert(!std::is_default_constructible_v<RenderTargetRegistry>);
        static_assert(!std::is_move_constructible_v<RenderTargetRegistry>);
        attempts = 0;
        auto invalid = RenderTargetRegistry::create(resources, 0);
        assert(!invalid && isError<err::internal::InvalidArgument>(invalid.error()));
        assert(attempts == 0 && images.empty() && views.empty());
        auto owner = RenderTargetRegistry::create(resources, 2);
        assert(owner);
        auto& registry = **owner;
        assert(&registry.resourceContext() == &resources && registry.framesInFlight() == 2);
        attempts = 0;
        fail_at = 3;
        auto failed = registry.makeTargetPool(layout, {8, 8});
        assert(!failed && isError<err::device::VulkanCallFailed>(failed.error()));
        assert(failed.error().args[0] == encodeVkResult(VK_ERROR_OUT_OF_DEVICE_MEMORY));
        assert(registry.all().values().empty() && images.empty() && views.empty());
        fail_at = 0;
        auto pool = registry.makeTargetPool(layout, {8, 8});
        assert(pool);
        RenderTargetEntry entry;
        entry.layout = layout;
        entry.pool = std::move(*pool);
        const auto first = registry.insert(std::move(entry));
        assert(first.isValid() && registry.tryGet(first));
        registry.retireTargetPool(*registry.tryGet(first), 30);
        registry.erase(first);
        assert(!registry.tryGet(first) && images.size() == 4);
        pool = registry.makeTargetPool(layout, {4, 4});
        assert(pool);
        RenderTargetEntry replacement;
        replacement.layout = layout;
        replacement.pool = std::move(*pool);
        const auto second = registry.insert(std::move(replacement));
        assert(second.isValid() && second != first && !registry.tryGet(first));
        assert(registry.tryGet(second)->pool->extent().width == 4);
        registry.collectRetiredPools(29);
        assert(images.size() == 8 && views.size() == 8);
        registry.collectRetiredPools(30);
        assert(images.size() == 4 && views.size() == 4);
        registry.retireTargetPool(*registry.tryGet(second), 40);
        registry.erase(second);
        assert(images.size() == 4 && !registry.tryGet(second));
        // Original owner-safe teardown disposes the remaining retirement prefix.
        owner->reset();
    }
    assert(images.empty() && views.empty());
    std::puts("offscreen: 8 create + 8 resize + 12 layout native failures; exact errors, retry, retention, serial "
              "retirement PASS");
}
