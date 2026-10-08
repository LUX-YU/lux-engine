#include <lux/cxx/container/SmallVector.hpp>
#include <lux/cxx/core/Format.hpp>
#include <lux/engine/render/gpu/VulkanContext.hpp>
#include <lux/engine/render/targets/OffscreenImagePool.hpp>
#include <vk_mem_alloc.h>

#include <algorithm>
#include <limits>
#include <utility>

namespace lux::render
{

    // =============================================================================
    // Construction / Destruction (RAII)
    // =============================================================================

    Expected<std::unique_ptr<OffscreenImagePool>> OffscreenImagePool::create(
        ResourceContext& res_ctx,
        const RenderTargetLayout& layout,
        VkExtent2D extent,
        uint32_t frames_in_flight
    ) noexcept
    {
        auto backing = prepareBacking(res_ctx, layout, extent, frames_in_flight);
        if (!backing)
        {
            return lux::cxx::unexpected(backing.error());
        }
        return std::unique_ptr<OffscreenImagePool>(
            new OffscreenImagePool(res_ctx, layout, frames_in_flight, std::move(*backing))
        );
    }

    OffscreenImagePool::OffscreenImagePool(
        ResourceContext& res_ctx,
        const RenderTargetLayout& layout,
        uint32_t frames_in_flight,
        PreparedBacking&& backing
    ) noexcept
        : res_ctx_(res_ctx), layout_(layout), frames_in_flight_(frames_in_flight),
          slot_images_(std::move(backing.images.slot_images)), slot_views_(std::move(backing.images.slot_views)),
          binding_(std::move(backing.binding)), recorded_slots_(frames_in_flight)
    {
        binding_.layout = &layout_;
    }

    OffscreenImagePool::~OffscreenImagePool() noexcept
    {
        // Clean up any retired images still in the queue
        for (auto& retired : retired_images_)
        {
            for (size_t si = 0; si < kTargetSlotCount; ++si)
            {
                notifyViewRetirement(retired.slot_views[si]);
            }
            // Member order destroys views before their VmaImage allocations.
        }
        retired_images_.clear();
        release();
    }

    // =============================================================================
    // Resize
    // =============================================================================

    Expected<void> OffscreenImagePool::resize(VkExtent2D new_extent) noexcept
    {
        const bool is_same_extent = new_extent.width == extent().width && new_extent.height == extent().height;
        if (is_same_extent)
        {
            return {};
        }
        return rebuild(layout_, new_extent);
    }

    Expected<void> OffscreenImagePool::applyLayout(const RenderTargetLayout& layout) noexcept
    {
        if (layout.slots == layout_.slots)
        {
            return {};
        }
        return rebuild(layout, extent());
    }

    Expected<void> OffscreenImagePool::rebuild(const RenderTargetLayout& layout, VkExtent2D extent) noexcept
    {
        if (backing_revision_ == std::numeric_limits<uint64_t>::max())
        {
            return renderFailure<err::memory::CapacityExhausted>();
        }
        auto candidate = prepareBacking(res_ctx_, layout, extent, frames_in_flight_);
        if (!candidate)
        {
            return lux::cxx::unexpected(candidate.error());
        }
        // Every allocation, including the retirement slot, precedes commit.
        retired_images_.reserve(retired_images_.size() + 1);
        RetiredImages retired;
        retired.slot_images.swap(slot_images_);
        retired.slot_views.swap(slot_views_);
        slot_images_.swap(candidate->images.slot_images);
        slot_views_.swap(candidate->images.slot_views);
        binding_ = std::move(candidate->binding);
        layout_ = layout;
        binding_.layout = &layout_;
        ++backing_revision_;
        std::fill(recorded_slots_.begin(), recorded_slots_.end(), 0);
        retired_images_.push_back(std::move(retired));
        return {};
    }

    // =============================================================================
    // makeFrameBinding
    // =============================================================================

    RenderTargetBinding OffscreenImagePool::makeFrameBinding(uint32_t image_index) const
    {
        RenderTargetBinding b;
        b.layout = &layout_;
        b.extent = binding_.extent;
        b.is_presentable = false;

        for (size_t si = 0; si < kTargetSlotCount; ++si)
        {
            if (!layout_.hasSlot(static_cast<ETargetSlot>(si)))
            {
                continue;
            }

            auto& src = binding_.slot_images[si];
            auto& dst = b.slot_images[si];

            if (image_index < src.images.size())
            {
                dst.images = {src.images[image_index]};
                dst.views = {src.views[image_index]};
            }
        }

        return b;
    }

    // =============================================================================
    // Image allocation / release
    // =============================================================================

    Expected<OffscreenImagePool::PreparedBacking> OffscreenImagePool::prepareBacking(
        ResourceContext& res_ctx,
        const RenderTargetLayout& layout,
        VkExtent2D extent,
        uint32_t frames_in_flight
    ) noexcept
    {
        const bool is_invalid_extent = extent.width == 0 || extent.height == 0;
        if (is_invalid_extent || frames_in_flight == 0)
        {
            return renderFailure<err::internal::InvalidArgument>();
        }
        for (const auto& slot : layout.slots)
        {
            if (!slot)
            {
                continue;
            }
            const bool is_invalid_format = toVkFormat(slot->format) == VK_FORMAT_UNDEFINED;
            const bool is_invalid_usage = toVkImageUsage(slot->usage) == 0;
            if (is_invalid_format || is_invalid_usage)
            {
                return renderFailure<err::internal::InvalidArgument>();
            }
        }
        PreparedBacking prepared;
        auto& dev_ctx = res_ctx.deviceContext();
        VmaAllocator vma = dev_ctx.vmaAllocator();
        VkDevice dev = dev_ctx.logicalDevice();
        const auto name_object = reinterpret_cast<PFN_vkSetDebugUtilsObjectNameEXT>(
            vkGetInstanceProcAddr(dev_ctx.instanceContext().instance().handle(), "vkSetDebugUtilsObjectNameEXT")
        );

        prepared.binding.layout = nullptr;
        prepared.binding.extent = extent;
        prepared.binding.is_presentable = false;

        for (size_t si = 0; si < kTargetSlotCount; ++si)
        {
            const auto slot_enum = static_cast<ETargetSlot>(si);
            if (!layout.hasSlot(slot_enum))
            {
                continue;
            }

            const RenderTargetSlotDesc& desc = layout.slot(slot_enum);

            VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
            ci.imageType = VK_IMAGE_TYPE_2D;
            ci.format = toVkFormat(desc.format);
            ci.extent = {extent.width, extent.height, 1};
            ci.mipLevels = 1;
            ci.arrayLayers = 1;
            ci.samples = VK_SAMPLE_COUNT_1_BIT;
            ci.tiling = VK_IMAGE_TILING_OPTIMAL;
            ci.usage = toVkImageUsage(desc.usage);
            ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

            VmaAllocationCreateInfo aci{};
            aci.usage = VMA_MEMORY_USAGE_GPU_ONLY;

            VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
            vi.format = toVkFormat(desc.format);
            vi.subresourceRange.aspectMask = toVkImageAspect(desc.aspect);
            vi.subresourceRange.baseMipLevel = 0;
            vi.subresourceRange.levelCount = 1;
            vi.subresourceRange.baseArrayLayer = 0;
            vi.subresourceRange.layerCount = 1;

            auto& images = prepared.images.slot_images[si];
            auto& views = prepared.images.slot_views[si];
            images.reserve(frames_in_flight);
            views.reserve(frames_in_flight);

            for (uint32_t f = 0; f < frames_in_flight; ++f)
            {
                auto image = VmaImage::create(vma, ci, aci);
                if (!image)
                {
                    return lux::cxx::unexpected(image.error());
                }
                images.emplace_back(std::move(image.value()));
                vi.image = images.back().image();

                if (name_object != nullptr)
                {
                    const auto name = lux::format("OffscreenTarget.{}.fif{}.Image", targetSlotName(slot_enum), f);
                    VkDebugUtilsObjectNameInfoEXT info{VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT};
                    info.objectType = VK_OBJECT_TYPE_IMAGE;
                    info.objectHandle = reinterpret_cast<std::uint64_t>(images.back().image());
                    info.pObjectName = name.c_str();
                    (void)name_object(dev, &info);
                }

                auto view = ImageViewOwner::create(dev, vi);
                if (!view)
                {
                    return renderFailure<err::device::VulkanCallFailed>(encodeVkResult(view.error()));
                }
                views.push_back(std::move(*view));
                if (name_object != nullptr)
                {
                    const auto name = lux::format("OffscreenTarget.{}.fif{}.View", targetSlotName(slot_enum), f);
                    VkDebugUtilsObjectNameInfoEXT info{VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT};
                    info.objectType = VK_OBJECT_TYPE_IMAGE_VIEW;
                    info.objectHandle = reinterpret_cast<std::uint64_t>(views.back().get());
                    info.pObjectName = name.c_str();
                    (void)name_object(dev, &info);
                }
            }

            // Populate binding slot
            auto& bs = prepared.binding.slot_images[si];
            bs.images.reserve(frames_in_flight);
            bs.views.reserve(frames_in_flight);
            for (uint32_t f = 0; f < frames_in_flight; ++f)
            {
                bs.images.push_back(images[f].image());
                bs.views.push_back(views[f].get());
            }
        }
        return prepared;
    }

    void OffscreenImagePool::release() noexcept
    {
        for (size_t si = 0; si < kTargetSlotCount; ++si)
        {
            notifyViewRetirement(slot_views_[si]);
            slot_views_[si].clear();
            slot_images_[si].clear(); // VmaImage RAII releases VkImage + VmaAllocation
        }
        binding_ = {};
    }

    void OffscreenImagePool::notifyViewRetirement(std::span<const ImageViewOwner> views) noexcept
    {
        const bool has_notification = retire_views_ != nullptr && !views.empty();
        if (has_notification)
        {
            lux::cxx::SmallVector<VkImageView, 4> handles;
            handles.reserve(views.size());
            for (const auto& view : views)
            {
                handles.push_back(view.get());
            }
            retire_views_(retire_owner_.get(), {handles.data(), handles.size()});
        }
    }

    // =============================================================================
    // Retire GC
    // =============================================================================

    void OffscreenImagePool::collectRetired(uint64_t frame_id, uint64_t completed_serial)
    {
        auto it = retired_images_.begin();
        while (it != retired_images_.end())
        {
            // Stamp the retire frame on first encounter (upper bound of last use).
            if (it->retire_frame == 0)
            {
                it->retire_frame = frame_id;
                ++it;
                continue;
            }
            // Free only once the FENCE-PROVEN completion watermark passes the stamp
            // — "frame_id - retire_frame >= fif" arithmetic over-claims completion
            // on non-submitting ticks (see FrameDriver::gpuCompletedSerial).
            if (it->retire_frame <= completed_serial)
            {
                for (size_t si = 0; si < kTargetSlotCount; ++si)
                {
                    notifyViewRetirement(it->slot_views[si]);
                    // Erase move-assigns later records over this one. Empty the destination
                    // in dependency order before that memberwise move can release images.
                    it->slot_views[si].clear();
                    it->slot_images[si].clear();
                }
                it = retired_images_.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

} // namespace lux::render
