/**
 * @file HzbResources.cpp
 * @brief Double-buffered Hi-Z (max-Z) depth pyramid — see HzbResources.hpp.
 */

#include <lux/engine/render/resources/hzb/HzbResources.hpp>

#include <vk_mem_alloc.h>

#include <lux/engine/render/resources/descriptor/SceneDescriptorArena.hpp>

#include <algorithm>
#include <array>
#include <cstring>

namespace lux::render
{
    static_assert(sizeof(HzbResources::ViewParams) == 80u,
                  "HzbResources::ViewParams must be 80 bytes (std140 mat4 + vec4)");

    namespace
    {
        // Full mip chain down to 1×1: floor(log2(max(w,h))) + 1 levels.
        uint32_t mipCountFor(uint32_t w, uint32_t h)
        {
            uint32_t m = std::max(w, h);
            uint32_t levels = 1u;
            while (m > 1u) { m >>= 1u; ++levels; }
            return levels;
        }
    } // namespace

    HzbResources::~HzbResources() { destroy(); }

    bool HzbResources::initSlot(Slot& s)
    {
        // R32_SFLOAT mip chain. STORAGE: the downsample writes each mip. SAMPLED:
        // the cull pass samples the chain. TRANSFER_SRC: readback for the test.
        VkImageCreateInfo img_ci{};
        img_ci.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        img_ci.imageType     = VK_IMAGE_TYPE_2D;
        img_ci.format        = format();
        img_ci.extent        = { width_, height_, 1u };
        img_ci.mipLevels     = mip_count_;
        img_ci.arrayLayers   = 1u;
        img_ci.samples       = VK_SAMPLE_COUNT_1_BIT;
        img_ci.tiling        = VK_IMAGE_TILING_OPTIMAL;
        img_ci.usage         = VK_IMAGE_USAGE_STORAGE_BIT
                             | VK_IMAGE_USAGE_SAMPLED_BIT
                             | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
                             | VK_IMAGE_USAGE_TRANSFER_DST_BIT;   // clear-to-far on (re)create
        img_ci.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
        img_ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        VmaAllocationCreateInfo alloc_ci{};
        alloc_ci.usage = VMA_MEMORY_USAGE_GPU_ONLY;

        if (vmaCreateImage(allocator_, &img_ci, &alloc_ci, &s.image, &s.alloc, nullptr) != VK_SUCCESS)
        {
            s.image = VK_NULL_HANDLE;
            s.alloc = VK_NULL_HANDLE;
            return false;
        }

        auto makeView = [this, &s](uint32_t base, uint32_t count) -> VkImageView
        {
            VkImageViewCreateInfo v{};
            v.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            v.image                           = s.image;
            v.viewType                        = VK_IMAGE_VIEW_TYPE_2D;
            v.format                          = format();
            v.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
            v.subresourceRange.baseMipLevel   = base;
            v.subresourceRange.levelCount     = count;
            v.subresourceRange.baseArrayLayer = 0u;
            v.subresourceRange.layerCount     = 1u;
            VkImageView out = VK_NULL_HANDLE;
            vkCreateImageView(device_, &v, nullptr, &out);
            return out;
        };

        s.full_view = makeView(0u, mip_count_);
        s.mip_views.resize(mip_count_, VK_NULL_HANDLE);
        for (uint32_t i = 0u; i < mip_count_; ++i)
            s.mip_views[i] = makeView(i, 1u);

        if (s.full_view == VK_NULL_HANDLE) return false;
        for (VkImageView v : s.mip_views)
            if (v == VK_NULL_HANDLE) return false;

        // Read side (set 1): a mapped view-param UBO + a combined-sampler/UBO
        // descriptor for the cull pass. Only when the caller wired the read side
        // (build-only users like the headless test leave these null).
        if (arena_ != nullptr && read_layout_ != VK_NULL_HANDLE && sampler_ != VK_NULL_HANDLE)
        {
            VkBufferCreateInfo buf_ci{};
            buf_ci.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            buf_ci.size        = sizeof(ViewParams);
            buf_ci.usage       = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
            buf_ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            VmaAllocationCreateInfo baci{};
            baci.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
            baci.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
            VmaAllocationInfo bainfo{};
            if (vmaCreateBuffer(allocator_, &buf_ci, &baci, &s.ubo, &s.ubo_alloc, &bainfo) != VK_SUCCESS)
                return false;
            s.ubo_mapped = bainfo.pMappedData;

            s.read_ds = arena_->allocate(read_layout_);
            if (s.read_ds == VK_NULL_HANDLE) return false;

            VkDescriptorImageInfo ii{};
            ii.sampler     = sampler_;
            ii.imageView   = s.full_view;
            ii.imageLayout = VK_IMAGE_LAYOUT_GENERAL;   // build leaves every mip in GENERAL
            VkDescriptorBufferInfo bi{};
            bi.buffer = s.ubo; bi.offset = 0u; bi.range = sizeof(ViewParams);
            std::array<VkWriteDescriptorSet, 2> w{};
            w[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[0].dstSet          = s.read_ds;
            w[0].dstBinding      = 0u;
            w[0].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            w[0].descriptorCount = 1u;
            w[0].pImageInfo      = &ii;
            w[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[1].dstSet          = s.read_ds;
            w[1].dstBinding      = 1u;
            w[1].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            w[1].descriptorCount = 1u;
            w[1].pBufferInfo     = &bi;
            vkUpdateDescriptorSets(device_, 2u, w.data(), 0u, nullptr);
        }
        return true;
    }

    void HzbResources::destroySlot(Slot& s)
    {
        if (device_ != VK_NULL_HANDLE)
        {
            for (VkImageView v : s.mip_views)
                if (v != VK_NULL_HANDLE) vkDestroyImageView(device_, v, nullptr);
            if (s.full_view != VK_NULL_HANDLE)
                vkDestroyImageView(device_, s.full_view, nullptr);
        }
        s.mip_views.clear();
        s.full_view = VK_NULL_HANDLE;
        if (s.image != VK_NULL_HANDLE && allocator_ != VK_NULL_HANDLE)
            vmaDestroyImage(allocator_, s.image, s.alloc);
        s.image = VK_NULL_HANDLE;
        s.alloc = VK_NULL_HANDLE;
        if (s.ubo != VK_NULL_HANDLE && allocator_ != VK_NULL_HANDLE)
            vmaDestroyBuffer(allocator_, s.ubo, s.ubo_alloc);
        s.ubo        = VK_NULL_HANDLE;
        s.ubo_alloc  = VK_NULL_HANDLE;
        s.ubo_mapped = nullptr;
        s.read_ds    = VK_NULL_HANDLE;   // arena owns the set; just drop our handle
    }

    bool HzbResources::init(const InitInfo& info)
    {
        if (info.device == VK_NULL_HANDLE || info.allocator == VK_NULL_HANDLE
            || info.width == 0u || info.height == 0u)
            return false;

        destroy();

        device_      = info.device;
        allocator_   = info.allocator;
        width_       = info.width;
        height_      = info.height;
        mip_count_   = mipCountFor(width_, height_);
        arena_       = info.arena;
        read_layout_ = info.read_layout;
        sampler_     = info.sampler;

        for (Slot& s : slots_)
            if (!initSlot(s)) { destroy(); return false; }
        return true;
    }

    void HzbResources::destroy()
    {
        for (Slot& s : slots_)
            destroySlot(s);
        cur_ = 0u;
        mip_count_ = width_ = height_ = 0u;
    }

    void HzbResources::writeBuildDescriptors(VkDevice device,
                                             uint32_t slot,
                                             const VkDescriptorSet* mip_sets,
                                             uint32_t mip_set_count) const
    {
        if (slot >= 2u) return;
        const Slot& s = slots_[slot];
        const uint32_t n = std::min(mip_set_count, mip_count_);
        for (uint32_t k = 0u; k < n; ++k)
        {
            const uint32_t src_level = (k == 0u) ? 0u : (k - 1u);
            VkDescriptorImageInfo src_info{};
            src_info.imageView   = s.mip_views[src_level];
            src_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            VkDescriptorImageInfo dst_info{};
            dst_info.imageView   = s.mip_views[k];
            dst_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

            std::array<VkWriteDescriptorSet, 2> w{};
            w[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[0].dstSet          = mip_sets[k];
            w[0].dstBinding      = 0u;
            w[0].descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            w[0].descriptorCount = 1u;
            w[0].pImageInfo      = &src_info;
            w[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[1].dstSet          = mip_sets[k];
            w[1].dstBinding      = 1u;
            w[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            w[1].descriptorCount = 1u;
            w[1].pImageInfo      = &dst_info;
            vkUpdateDescriptorSets(device, 2u, w.data(), 0u, nullptr);
        }
    }

    void HzbResources::recordBuild(VkCommandBuffer cmd,
                                   VkPipelineLayout layout,
                                   uint32_t slot,
                                   const VkDescriptorSet* mip_sets,
                                   uint32_t mip_set_count,
                                   VkPipeline pipeline,
                                   VkDescriptorSet depth_set) const
    {
        if (slot >= 2u) return;
        const VkImage image = slots_[slot].image;

        if (pipeline != VK_NULL_HANDLE)
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        if (depth_set != VK_NULL_HANDLE)
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout,
                                    1u, 1u, &depth_set, 0u, nullptr);

        auto mipBarrier = [image](uint32_t level,
                                  VkImageLayout old_l, VkImageLayout new_l,
                                  VkAccessFlags src_a, VkAccessFlags dst_a) -> VkImageMemoryBarrier
        {
            VkImageMemoryBarrier b{};
            b.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            b.srcAccessMask       = src_a;
            b.dstAccessMask       = dst_a;
            b.oldLayout           = old_l;
            b.newLayout           = new_l;
            b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image               = image;
            b.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, level, 1u, 0u, 1u };
            return b;
        };

        const uint32_t n = std::min(mip_set_count, mip_count_);
        for (uint32_t k = 0u; k < n; ++k)
        {
            std::array<VkImageMemoryBarrier, 2> bar{};
            uint32_t nb = 0u;
            VkPipelineStageFlags src_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;

            // This mip: first touch UNDEFINED → GENERAL (compute writes it).
            bar[nb++] = mipBarrier(k, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                                   0, VK_ACCESS_SHADER_WRITE_BIT);
            if (k > 0u)
            {
                // Previous mip: make its write visible to this read (RAW). Layout
                // stays GENERAL (sampling a GENERAL-layout image is valid).
                bar[nb++] = mipBarrier(k - 1u, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
                                       VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
                src_stage |= VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
            }
            vkCmdPipelineBarrier(cmd, src_stage, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 0, 0, nullptr, 0, nullptr, nb, bar.data());

            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout,
                                    0u, 1u, &mip_sets[k], 0u, nullptr);

            BuildPushConstants pc{};
            pc.dst_w   = mipWidth(k);
            pc.dst_h   = mipHeight(k);
            pc.src_w   = (k == 0u) ? mipWidth(0u)  : mipWidth(k - 1u);
            pc.src_h   = (k == 0u) ? mipHeight(0u) : mipHeight(k - 1u);
            pc.is_mip0 = (k == 0u) ? 1u : 0u;
            vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT,
                               0u, sizeof(pc), &pc);

            vkCmdDispatch(cmd, (mipWidth(k) + 7u) / 8u, (mipHeight(k) + 7u) / 8u, 1u);
        }

        // ── 跨帧可见性屏障 ──
        // HZB 在 RenderGraph 之外,下一帧 cull 通过 feature 自管描述符集采样本帧
        // 写的金字塔(不是 RG 的 .read),所以 RG 不会替我们插这道屏障。这里手动让
        // 本帧所有 mip 的 compute 写,对将来 compute 的采样读可见(cull 与 build
        // 同 graphics 队列、单时间线 + 每帧 fence → 跨 submit 的 RAW 正确)。
        if (n > 0u)
        {
            VkImageMemoryBarrier vis{};
            vis.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            vis.srcAccessMask       = VK_ACCESS_SHADER_WRITE_BIT;
            vis.dstAccessMask       = VK_ACCESS_SHADER_READ_BIT;
            vis.oldLayout           = VK_IMAGE_LAYOUT_GENERAL;
            vis.newLayout           = VK_IMAGE_LAYOUT_GENERAL;
            vis.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            vis.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            vis.image               = image;
            vis.subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0u, n, 0u, 1u };
            vkCmdPipelineBarrier(cmd,
                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 0, 0, nullptr, 0, nullptr, 1, &vis);
        }
    }

    void HzbResources::recordInitToGeneral(VkCommandBuffer cmd) const
    {
        // forward-Z: far = 1.0 = "nothing in front" → an unbuilt/just-reset slot
        // reads as far everywhere, so the cull's max-Z test degrades to NO cull
        // (objects stay visible) instead of over-culling on garbage memory.
        const VkClearColorValue       far_clear{ { 1.0f, 1.0f, 1.0f, 1.0f } };
        const VkImageSubresourceRange range{ VK_IMAGE_ASPECT_COLOR_BIT, 0u, mip_count_, 0u, 1u };
        for (uint32_t s = 0u; s < 2u; ++s)
        {
            if (slots_[s].image == VK_NULL_HANDLE) continue;

            // 1. UNDEFINED → GENERAL (discard garbage), ready for the clear (TRANSFER write).
            VkImageMemoryBarrier to_general{};
            to_general.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            to_general.srcAccessMask       = 0;
            to_general.dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
            to_general.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
            to_general.newLayout           = VK_IMAGE_LAYOUT_GENERAL;
            to_general.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            to_general.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            to_general.image               = slots_[s].image;
            to_general.subresourceRange    = range;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 0, 0, nullptr, 0, nullptr, 1, &to_general);

            // 2. Clear every mip to far.
            vkCmdClearColorImage(cmd, slots_[s].image, VK_IMAGE_LAYOUT_GENERAL, &far_clear, 1u, &range);

            // 3. Make the clear visible to the build's / cull's shader access.
            VkImageMemoryBarrier to_shader{};
            to_shader.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            to_shader.srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
            to_shader.dstAccessMask       = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
            to_shader.oldLayout           = VK_IMAGE_LAYOUT_GENERAL;
            to_shader.newLayout           = VK_IMAGE_LAYOUT_GENERAL;
            to_shader.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            to_shader.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            to_shader.image               = slots_[s].image;
            to_shader.subresourceRange    = range;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 0, 0, nullptr, 0, nullptr, 1, &to_shader);
        }
    }

    void HzbResources::writeViewParams(uint32_t slot, const ViewParams& vp) noexcept
    {
        if (slot < 2u && slots_[slot].ubo_mapped != nullptr)
            std::memcpy(slots_[slot].ubo_mapped, &vp, sizeof(ViewParams));
    }

    VkDescriptorSet HzbResources::resolveHzbReadDS(const void* self, uint32_t /*frame_slot*/) noexcept
    {
        // Ignore the framework FIF slot — return the PREVIOUS ping-pong slot's
        // read DS, i.e. last frame's pyramid (correct for any frames-in-flight).
        const auto* h = static_cast<const HzbResources*>(self);
        return h->slots_[h->prevIndex()].read_ds;
    }

} // namespace lux::render
