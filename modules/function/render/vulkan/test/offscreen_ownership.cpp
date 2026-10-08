#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>

#include <lux/engine/render/gpu/VulkanContext.hpp>

#include <cassert>
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
#include "../src/gpu/VulkanContext.cpp"
#define vkCreateImageView createView
#define vkDestroyImageView destroyView
#include "../src/targets/OffscreenImagePool.cpp"
#undef vkDestroyImageView
#undef vkCreateImageView
// clang-format on

int main()
{
    using namespace lux::render;
    InstanceContext instance({});
    DeviceContext device(instance);
    assert(device.init(EPhysicalDeviceSelectionPolicy::DISCRETE_GPU_PREFERRED));
    ResourceContext resources(device);
    assert(resources.init());

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

    for (unsigned boundary = 1; boundary <= 8; ++boundary)
    {
        attempts = 0;
        fail_at = boundary;
        {
            OffscreenImagePool rejected(resources, layout, {32, 32}, 2);
            assert(!rejected.valid());
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
        OffscreenImagePool pool(resources, layout, {32, 32}, 2);
        assert(pool.valid() && images.size() == 4 && views.size() == 4);
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
        assert(pool.tryResize({64, 64}));
        pool.collectRetired(10, 0);
        assert(notification->count == 0 && images.size() == 8 && views.size() == 8);
        assert(pool.tryResize({96, 96}));
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
            assert(!pool.tryResize({128, 128}));
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
}
