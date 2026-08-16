#include <lux/engine/render/targets/OffscreenImagePool.hpp>
#include <lux/engine/render/gpu/VulkanContext.hpp>
#include <vk_mem_alloc.h>

#include <cassert>
#include <cstdio>

namespace lux::render
{

// =============================================================================
// Construction / Destruction (RAII)
// =============================================================================

OffscreenImagePool::OffscreenImagePool(
    ResourceContext& res_ctx,
    const RenderTargetLayout& layout,
    VkExtent2D extent,
    uint32_t frames_in_flight)
    : res_ctx_(res_ctx)
    , layout_(layout)
    , frames_in_flight_(frames_in_flight)
{
    [[maybe_unused]] bool ok = allocate(extent);
    assert(ok && "OffscreenImagePool: failed to allocate images");
    // No explicit layout transition — RG inserts correct barriers on first use.
}

OffscreenImagePool::~OffscreenImagePool()
{
    VkDevice dev = res_ctx_.deviceContext().logicalDevice();
    // Clean up any retired images still in the queue
    for (auto& retired : retired_images_)
    {
        for (size_t si = 0; si < kTargetSlotCount; ++si)
            for (VkImageView v : retired.slot_views[si])
                vkDestroyImageView(dev, v, nullptr);
        // VmaImage RAII handles VkImage + VmaAllocation destruction
    }
    retired_images_.clear();
    release();
}

// =============================================================================
// Resize
// =============================================================================

void OffscreenImagePool::resize(VkExtent2D new_extent)
{
    // Retire old images — they may still be referenced by in-flight GPU work.
    RetiredImages retired;
    for (size_t si = 0; si < kTargetSlotCount; ++si)
    {
        retired.slot_images[si] = std::move(slot_images_[si]);
        retired.slot_views[si]  = std::move(slot_views_[si]);
    }
    retired.retire_frame = 0; // sentinel — stamped by collectRetired
    retired_images_.push_back(std::move(retired));

    binding_ = {};
    [[maybe_unused]] bool ok = allocate(new_extent);
    assert(ok && "OffscreenImagePool: failed to allocate images on resize");
}

// =============================================================================
// makeFrameBinding
// =============================================================================

RenderTargetBinding OffscreenImagePool::makeFrameBinding(uint32_t image_index) const
{
    RenderTargetBinding b;
    b.layout        = &layout_;
    b.extent        = binding_.extent;
    b.is_presentable = false;

    for (size_t si = 0; si < kTargetSlotCount; ++si)
    {
        if (!layout_.hasSlot(static_cast<TargetSlot>(si)))
            continue;

        auto& src = binding_.slot_images[si];
        auto& dst = b.slot_images[si];

        if (image_index < src.images.size())
        {
            dst.images = {src.images[image_index]};
            dst.views  = {src.views[image_index]};
        }
    }

    return b;
}

// =============================================================================
// Image allocation / release
// =============================================================================

bool OffscreenImagePool::allocate(VkExtent2D extent)
{
    auto& dev_ctx = res_ctx_.deviceContext();
    VmaAllocator vma = dev_ctx.vmaAllocator();
    VkDevice dev     = dev_ctx.logicalDevice();
    const auto name_object = reinterpret_cast<
        PFN_vkSetDebugUtilsObjectNameEXT>(vkGetInstanceProcAddr(
            dev_ctx.instanceContext().instance().handle(),
            "vkSetDebugUtilsObjectNameEXT"));

    binding_.layout       = &layout_;
    binding_.extent       = extent;
    binding_.is_presentable = false;

    for (size_t si = 0; si < kTargetSlotCount; ++si)
    {
        const auto slot_enum = static_cast<TargetSlot>(si);
        if (!layout_.hasSlot(slot_enum))
            continue;

        const RenderTargetSlotDesc& desc = layout_.slot(slot_enum);

        VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ci.imageType     = VK_IMAGE_TYPE_2D;
        ci.format        = toVkFormat(desc.format);
        ci.extent        = {extent.width, extent.height, 1};
        ci.mipLevels     = 1;
        ci.arrayLayers   = 1;
        ci.samples       = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling        = VK_IMAGE_TILING_OPTIMAL;
        ci.usage         = toVkImageUsage(desc.usage);
        ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        VmaAllocationCreateInfo aci{};
        aci.usage = VMA_MEMORY_USAGE_GPU_ONLY;

        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.viewType                        = VK_IMAGE_VIEW_TYPE_2D;
        vi.format                          = toVkFormat(desc.format);
        vi.subresourceRange.aspectMask     = toVkImageAspect(desc.aspect);
        vi.subresourceRange.baseMipLevel   = 0;
        vi.subresourceRange.levelCount     = 1;
        vi.subresourceRange.baseArrayLayer = 0;
        vi.subresourceRange.layerCount     = 1;

        auto& images = slot_images_[si];
        auto& views  = slot_views_[si];
        images.clear();
        views.clear();
        images.reserve(frames_in_flight_);
        views.reserve(frames_in_flight_);

        for (uint32_t f = 0; f < frames_in_flight_; ++f)
        {
            auto image = VmaImage::create(vma, ci, aci);
            if (!image)
            {
                release();
                return false;
            }
            images.emplace_back(std::move(image.value()));
            vi.image = images.back().image();

            if (name_object != nullptr)
            {
                char name[96]{};
                std::snprintf(
                    name,
                    sizeof(name),
                    "OffscreenTarget.%s.fif%u.Image",
                    targetSlotName(slot_enum),
                    f);
                VkDebugUtilsObjectNameInfoEXT info{
                    VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT};
                info.objectType = VK_OBJECT_TYPE_IMAGE;
                info.objectHandle = reinterpret_cast<std::uint64_t>(
                    images.back().image());
                info.pObjectName = name;
                (void)name_object(dev, &info);
            }

            VkImageView view = VK_NULL_HANDLE;
            if (vkCreateImageView(dev, &vi, nullptr, &view) != VK_SUCCESS)
            {
                release();
                return false;
            }
            views.push_back(view);
            if (name_object != nullptr)
            {
                char name[96]{};
                std::snprintf(
                    name,
                    sizeof(name),
                    "OffscreenTarget.%s.fif%u.View",
                    targetSlotName(slot_enum),
                    f);
                VkDebugUtilsObjectNameInfoEXT info{
                    VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT};
                info.objectType = VK_OBJECT_TYPE_IMAGE_VIEW;
                info.objectHandle = reinterpret_cast<std::uint64_t>(view);
                info.pObjectName = name;
                (void)name_object(dev, &info);
            }
        }

        // Populate binding slot
        auto& bs = binding_.slot_images[si];
        bs.images.clear();
        bs.views.clear();
        bs.images.reserve(frames_in_flight_);
        bs.views.reserve(frames_in_flight_);
        for (uint32_t f = 0; f < frames_in_flight_; ++f)
        {
            bs.images.push_back(images[f].image());
            bs.views.push_back(views[f]);
        }
    }
    return true;
}

void OffscreenImagePool::release()
{
    VkDevice dev = res_ctx_.deviceContext().logicalDevice();
    for (size_t si = 0; si < kTargetSlotCount; ++si)
    {
        for (VkImageView v : slot_views_[si])
            vkDestroyImageView(dev, v, nullptr);
        slot_views_[si].clear();
        slot_images_[si].clear();  // VmaImage RAII releases VkImage + VmaAllocation
    }
    binding_ = {};
}

// =============================================================================
// Retire GC
// =============================================================================

void OffscreenImagePool::collectRetired(uint64_t frame_id, uint64_t completed_serial)
{
    VkDevice dev = res_ctx_.deviceContext().logicalDevice();
    auto it = retired_images_.begin();
    while (it != retired_images_.end())
    {
        // Stamp the retire frame on first encounter (upper bound of last use).
        if (it->retire_frame == 0) {
            it->retire_frame = frame_id;
            ++it;
            continue;
        }
        // Free only once the FENCE-PROVEN completion watermark passes the stamp
        // — "frame_id - retire_frame >= fif" arithmetic over-claims completion
        // on non-submitting ticks (see FrameDriver::gpuCompletedSerial).
        if (it->retire_frame <= completed_serial) {
            for (size_t si = 0; si < kTargetSlotCount; ++si) {
                for (VkImageView v : it->slot_views[si])
                    vkDestroyImageView(dev, v, nullptr);
                // VmaImage RAII releases VkImage + VmaAllocation
            }
            it = retired_images_.erase(it);
        } else {
            ++it;
        }
    }
}

} // namespace lux::render
