/**
 * @file HzbResources.cpp
 * @brief Double-buffered Hi-Z (max-Z) depth pyramid — see HzbResources.hpp.
 */

#include <lux/engine/render/resources/hzb/HzbResources.hpp>

#include <vk_mem_alloc.h>

#include <lux/engine/render/gpu/VulkanContext.hpp>
#include <lux/engine/render/gpu/descriptor/SceneDescriptorArena.hpp>
#include <lux/engine/render/gpu/lifecycle/DeferredDestroyQueue.hpp>

#include <algorithm>
#include <array>
#include <cstring>

namespace lux::render
{
    static_assert(
        sizeof(HzbResources::ViewParams) == 112u,
        "HzbResources::ViewParams must be 112 bytes (std140 exact-origin layout)"
    );

    namespace
    {
        // Full mip chain down to 1×1: floor(log2(max(w,h))) + 1 levels.
        uint32_t mipCountFor(uint32_t w, uint32_t h)
        {
            uint32_t m = std::max(w, h);
            uint32_t levels = 1u;
            while (m > 1u)
            {
                m >>= 1u;
                ++levels;
            }
            return levels;
        }
    } // namespace

    Expected<std::unique_ptr<HzbResources>> HzbResources::create(const CreateInfo& info) noexcept
    {
        const bool has_read_side = info.arena || info.read_layout != VK_NULL_HANDLE || info.sampler != VK_NULL_HANDLE;
        const bool is_complete_read_side =
            info.arena && info.read_layout != VK_NULL_HANDLE && info.sampler != VK_NULL_HANDLE;
        if (has_read_side && !is_complete_read_side)
        {
            return renderFailure<err::internal::InvalidArgument>();
        }
        return std::unique_ptr<HzbResources>(new HzbResources(info));
    }

    HzbResources::HzbResources(const CreateInfo& info) noexcept
        : device_(info.device), retirement_(info.retirement), arena_(info.arena), read_layout_(info.read_layout),
          sampler_(info.sampler)
    {
    }

    Expected<void> HzbResources::prepareSlot(Slot& s, const ViewSlots& geom) noexcept
    {
        const uint32_t width_ = geom.width;
        const uint32_t height_ = geom.height;
        const uint32_t mip_count_ = geom.mip_count;

        // R32_SFLOAT mip chain. STORAGE: the downsample writes each mip. SAMPLED:
        // the cull pass samples the chain. TRANSFER_SRC: readback for the test.
        VkImageCreateInfo img_ci{};
        img_ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        img_ci.imageType = VK_IMAGE_TYPE_2D;
        img_ci.format = format();
        img_ci.extent = {width_, height_, 1u};
        img_ci.mipLevels = mip_count_;
        img_ci.arrayLayers = 1u;
        img_ci.samples = VK_SAMPLE_COUNT_1_BIT;
        img_ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        img_ci.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                       VK_IMAGE_USAGE_TRANSFER_DST_BIT; // clear-to-far on (re)create
        img_ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        img_ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        VmaAllocationCreateInfo alloc_ci{};
        alloc_ci.usage = VMA_MEMORY_USAGE_GPU_ONLY;

        VkImage image{};
        VmaAllocation allocation{};
        const auto status = vmaCreateImage(device_.vmaAllocator(), &img_ci, &alloc_ci, &image, &allocation, nullptr);
        if (status != VK_SUCCESS)
        {
            return renderFailure<err::device::VulkanCallFailed>(encodeVkResult(status));
        }
        s.image = TFifOwnedAllocated<VkImage>(retirement_, image, allocation);

        auto make_view = [this, image](uint32_t base, uint32_t count) noexcept -> Expected<TFifOwned<VkImageView>>
        {
            VkImageViewCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            info.image = image;
            info.viewType = VK_IMAGE_VIEW_TYPE_2D;
            info.format = format();
            info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, base, count, 0, 1};
            VkImageView view{};
            const auto result = vkCreateImageView(device_.logicalDevice(), &info, nullptr, &view);
            if (result != VK_SUCCESS)
            {
                return renderFailure<err::device::VulkanCallFailed>(encodeVkResult(result));
            }
            return TFifOwned<VkImageView>(&retirement_, view);
        };
        auto full_view = make_view(0, mip_count_);
        if (!full_view)
        {
            return lux::cxx::unexpected(full_view.error());
        }
        s.full_view = std::move(*full_view);
        s.mip_views.reserve(mip_count_);
        for (uint32_t i = 0; i < mip_count_; ++i)
        {
            auto view = make_view(i, 1);
            if (!view)
            {
                return lux::cxx::unexpected(view.error());
            }
            s.mip_views.push_back(std::move(*view));
        }

        // Read side (set 1): a mapped view-param UBO + a combined-sampler/UBO
        // descriptor for the cull pass. Only when the caller wired the read side
        // (build-only users leave the entire read side absent).
        if (arena_)
        {
            VkBufferCreateInfo buf_ci{};
            buf_ci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            buf_ci.size = sizeof(ViewParams);
            buf_ci.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
            buf_ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            VmaAllocationCreateInfo baci{};
            baci.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
            baci.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
            // writeViewParams publishes by memcpy; a complete read side must be coherent.
            baci.requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
            VmaAllocationInfo bainfo{};
            VkBuffer buffer{};
            VmaAllocation buffer_allocation{};
            const auto result =
                vmaCreateBuffer(device_.vmaAllocator(), &buf_ci, &baci, &buffer, &buffer_allocation, &bainfo);
            if (result != VK_SUCCESS)
            {
                return renderFailure<err::device::VulkanCallFailed>(encodeVkResult(result));
            }
            s.ubo = TFifOwnedAllocated<VkBuffer>(retirement_, buffer, buffer_allocation);
            if (!bainfo.pMappedData)
            {
                return renderFailure<err::device::VulkanCallFailed>(encodeVkResult(VK_ERROR_MEMORY_MAP_FAILED));
            }
            s.ubo_mapped = bainfo.pMappedData;
            const ViewParams initial{};
            std::memcpy(s.ubo_mapped, &initial, sizeof(initial));

            auto descriptor = arena_->allocate(read_layout_);
            if (!descriptor)
            {
                return lux::cxx::unexpected(descriptor.error());
            }
            s.read_ds = *descriptor;

            VkDescriptorImageInfo ii{};
            ii.sampler = sampler_;
            ii.imageView = s.full_view.get();
            ii.imageLayout = VK_IMAGE_LAYOUT_GENERAL; // build leaves every mip in GENERAL
            VkDescriptorBufferInfo bi{};
            bi.buffer = s.ubo.get();
            bi.offset = 0u;
            bi.range = sizeof(ViewParams);
            std::array<VkWriteDescriptorSet, 2> w{};
            w[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[0].dstSet = s.read_ds;
            w[0].dstBinding = 0u;
            w[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            w[0].descriptorCount = 1u;
            w[0].pImageInfo = &ii;
            w[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[1].dstSet = s.read_ds;
            w[1].dstBinding = 1u;
            w[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            w[1].descriptorCount = 1u;
            w[1].pBufferInfo = &bi;
            vkUpdateDescriptorSets(device_.logicalDevice(), 2u, w.data(), 0u, nullptr);
        }
        return {};
    }

    Expected<void> HzbResources::ensureView(uint32_t view_id, uint32_t width, uint32_t height) noexcept
    {
        const bool is_invalid_extent = width == 0 || height == 0;
        if (is_invalid_extent)
        {
            return renderFailure<err::internal::InvalidArgument>();
        }
        if (const auto* existing = findView(view_id))
        {
            const bool is_same_extent = existing->width == width && existing->height == height;
            if (is_same_extent)
            {
                return {};
            }
        }
        auto candidate = std::make_unique<ViewSlots>();
        candidate->width = width;
        candidate->height = height;
        candidate->mip_count = mipCountFor(width, height);
        for (auto& slot : candidate->slots)
        {
            if (auto result = prepareSlot(slot, *candidate); !result)
            {
                return result;
            }
        }
        views_[view_id] = std::move(candidate);
        return {};
    }

    void HzbResources::evictView(uint32_t view_id) noexcept
    {
        views_.erase(view_id);
    }

    HzbResources::ViewSlots* HzbResources::findView(uint32_t view_id) noexcept
    {
        const auto* owner = views_.tryGet(view_id);
        return owner ? owner->get() : nullptr;
    }

    const HzbResources::ViewSlots* HzbResources::findView(uint32_t view_id) const noexcept
    {
        const auto* owner = views_.tryGet(view_id);
        return owner ? owner->get() : nullptr;
    }

    bool HzbResources::viewReady(uint32_t view_id) const noexcept
    {
        return findView(view_id) != nullptr;
    }

    void HzbResources::setCurrent(uint32_t view_id, uint32_t parity) noexcept
    {
        if (ViewSlots* vs = findView(view_id))
        {
            vs->cur = parity & 1u;
        }
    }

    uint32_t HzbResources::curIndex(uint32_t view_id) const noexcept
    {
        const ViewSlots* vs = findView(view_id);
        return vs ? vs->cur : 0u;
    }

    uint32_t HzbResources::prevIndex(uint32_t view_id) const noexcept
    {
        const ViewSlots* vs = findView(view_id);
        return vs ? (vs->cur ^ 1u) : 1u;
    }

    uint32_t HzbResources::mipCount(uint32_t view_id) const noexcept
    {
        const ViewSlots* vs = findView(view_id);
        return vs ? vs->mip_count : 0u;
    }

    uint32_t HzbResources::width(uint32_t view_id) const noexcept
    {
        const ViewSlots* vs = findView(view_id);
        return vs ? vs->width : 0u;
    }

    uint32_t HzbResources::height(uint32_t view_id) const noexcept
    {
        const ViewSlots* vs = findView(view_id);
        return vs ? vs->height : 0u;
    }

    void HzbResources::writeBuildDescriptors(
        VkDevice device,
        uint32_t view_id,
        uint32_t slot,
        const VkDescriptorSet* mip_sets,
        uint32_t mip_set_count
    ) const
    {
        const ViewSlots* vs = findView(view_id);
        if (vs == nullptr || slot >= 2u)
        {
            return;
        }
        const Slot& s = vs->slots[slot];
        const uint32_t n = std::min(mip_set_count, vs->mip_count);
        for (uint32_t k = 0u; k < n; ++k)
        {
            const uint32_t src_level = (k == 0u) ? 0u : (k - 1u);
            VkDescriptorImageInfo src_info{};
            src_info.imageView = s.mip_views[src_level].get();
            src_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            VkDescriptorImageInfo dst_info{};
            dst_info.imageView = s.mip_views[k].get();
            dst_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

            std::array<VkWriteDescriptorSet, 2> w{};
            w[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[0].dstSet = mip_sets[k];
            w[0].dstBinding = 0u;
            w[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            w[0].descriptorCount = 1u;
            w[0].pImageInfo = &src_info;
            w[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[1].dstSet = mip_sets[k];
            w[1].dstBinding = 1u;
            w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            w[1].descriptorCount = 1u;
            w[1].pImageInfo = &dst_info;
            vkUpdateDescriptorSets(device, 2u, w.data(), 0u, nullptr);
        }
    }

    void HzbResources::recordBuild(
        VkCommandBuffer cmd,
        VkPipelineLayout layout,
        uint32_t view_id,
        uint32_t slot,
        const VkDescriptorSet* mip_sets,
        uint32_t mip_set_count,
        VkPipeline pipeline,
        VkDescriptorSet depth_set
    ) const
    {
        const ViewSlots* vs = findView(view_id);
        if (vs == nullptr || slot >= 2u)
        {
            return;
        }
        const VkImage image = vs->slots[slot].image.get();

        if (pipeline != VK_NULL_HANDLE)
        {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        }
        if (depth_set != VK_NULL_HANDLE)
        {
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 1u, 1u, &depth_set, 0u, nullptr);
        }

        auto mipBarrier =
            [image](uint32_t level, VkImageLayout old_l, VkImageLayout new_l, VkAccessFlags src_a, VkAccessFlags dst_a)
            -> VkImageMemoryBarrier
        {
            VkImageMemoryBarrier b{};
            b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            b.srcAccessMask = src_a;
            b.dstAccessMask = dst_a;
            b.oldLayout = old_l;
            b.newLayout = new_l;
            b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = image;
            b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, level, 1u, 0u, 1u};
            return b;
        };

        const uint32_t n = std::min(mip_set_count, vs->mip_count);
        for (uint32_t k = 0u; k < n; ++k)
        {
            std::array<VkImageMemoryBarrier, 2> bar{};
            uint32_t nb = 0u;
            VkPipelineStageFlags src_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;

            // This mip: first touch UNDEFINED → GENERAL (compute writes it).
            bar[nb++] =
                mipBarrier(k, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_SHADER_WRITE_BIT);
            if (k > 0u)
            {
                // Previous mip: make its write visible to this read (RAW). Layout
                // stays GENERAL (sampling a GENERAL-layout image is valid).
                bar[nb++] = mipBarrier(
                    k - 1u,
                    VK_IMAGE_LAYOUT_GENERAL,
                    VK_IMAGE_LAYOUT_GENERAL,
                    VK_ACCESS_SHADER_WRITE_BIT,
                    VK_ACCESS_SHADER_READ_BIT
                );
                src_stage |= VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
            }
            vkCmdPipelineBarrier(
                cmd,
                src_stage,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                0,
                0,
                nullptr,
                0,
                nullptr,
                nb,
                bar.data()
            );

            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0u, 1u, &mip_sets[k], 0u, nullptr);

            BuildPushConstants pc{};
            pc.dst_w = vs->mipWidth(k);
            pc.dst_h = vs->mipHeight(k);
            pc.src_w = (k == 0u) ? vs->mipWidth(0u) : vs->mipWidth(k - 1u);
            pc.src_h = (k == 0u) ? vs->mipHeight(0u) : vs->mipHeight(k - 1u);
            pc.is_mip0 = (k == 0u) ? 1u : 0u;
            vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0u, sizeof(pc), &pc);

            vkCmdDispatch(cmd, (vs->mipWidth(k) + 7u) / 8u, (vs->mipHeight(k) + 7u) / 8u, 1u);
        }

        // ── Cross-frame visibility barrier ──
        // HZB lives outside the RenderGraph: next frame's cull pass samples this
        // frame's freshly-written pyramid through the feature's own self-managed
        // descriptor set (not via RG's .read()), so RG never inserts this
        // barrier for us. This manually makes this frame's compute writes across
        // all mips visible to the future compute sampling read (cull and build
        // share the same graphics queue with a single timeline + per-frame
        // fence, so read-after-write across submits is correct).
        if (n > 0u)
        {
            VkImageMemoryBarrier vis{};
            vis.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            vis.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            vis.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            vis.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
            vis.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            vis.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            vis.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            vis.image = image;
            vis.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0u, n, 0u, 1u};
            vkCmdPipelineBarrier(
                cmd,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                0,
                0,
                nullptr,
                0,
                nullptr,
                1,
                &vis
            );
        }
    }

    void HzbResources::recordInitToGeneral(VkCommandBuffer cmd, uint32_t view_id) const
    {
        const ViewSlots* vs = findView(view_id);
        if (vs == nullptr)
        {
            return;
        }

        // forward-Z: far = 1.0 = "nothing in front" → an unbuilt/just-reset slot
        // reads as far everywhere, so the cull's max-Z test degrades to NO cull
        // (objects stay visible) instead of over-culling on garbage memory.
        const VkClearColorValue far_clear{{1.0f, 1.0f, 1.0f, 1.0f}};
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0u, vs->mip_count, 0u, 1u};
        for (uint32_t s = 0u; s < 2u; ++s)
        {
            // 1. UNDEFINED → GENERAL (discard garbage), ready for the clear (TRANSFER write).
            VkImageMemoryBarrier to_general{};
            to_general.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            to_general.srcAccessMask = 0;
            to_general.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            to_general.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            to_general.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            to_general.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            to_general.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            to_general.image = vs->slots[s].image.get();
            to_general.subresourceRange = range;
            vkCmdPipelineBarrier(
                cmd,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT,
                0,
                0,
                nullptr,
                0,
                nullptr,
                1,
                &to_general
            );

            // 2. Clear every mip to far.
            vkCmdClearColorImage(cmd, vs->slots[s].image.get(), VK_IMAGE_LAYOUT_GENERAL, &far_clear, 1u, &range);

            // 3. Make the clear visible to the build's / cull's shader access.
            VkImageMemoryBarrier to_shader{};
            to_shader.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            to_shader.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            to_shader.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
            to_shader.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
            to_shader.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            to_shader.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            to_shader.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            to_shader.image = vs->slots[s].image.get();
            to_shader.subresourceRange = range;
            vkCmdPipelineBarrier(
                cmd,
                VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                0,
                0,
                nullptr,
                0,
                nullptr,
                1,
                &to_shader
            );
        }
    }

    void HzbResources::writeViewParams(uint32_t view_id, uint32_t slot, const ViewParams& vp) noexcept
    {
        ViewSlots* vs = findView(view_id);
        if (vs != nullptr && slot < 2u && vs->slots[slot].ubo_mapped != nullptr)
        {
            std::memcpy(vs->slots[slot].ubo_mapped, &vp, sizeof(ViewParams));
        }
    }

    VkDescriptorSet HzbResources::resolveHzbReadDS(const void* self, uint32_t /*frame_slot*/, uint32_t view_id) noexcept
    {
        // Ignore the framework FIF slot — return THIS VIEW's PREVIOUS ping-pong
        // slot read DS, i.e. that view's last-frame pyramid (correct for any
        // frames-in-flight). A view with no pyramid yet yields VK_NULL_HANDLE;
        // the recorder skips the bind and the cull shader keeps everything.
        const auto* h = static_cast<const HzbResources*>(self);
        const ViewSlots* vs = h->findView(view_id);
        if (vs == nullptr)
        {
            return VK_NULL_HANDLE;
        }
        return vs->slots[vs->cur ^ 1u].read_ds;
    }

} // namespace lux::render
