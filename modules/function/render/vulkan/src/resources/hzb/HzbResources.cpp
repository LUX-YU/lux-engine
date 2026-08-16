/**
 * @file HzbResources.cpp
 * @brief Double-buffered Hi-Z (max-Z) depth pyramid — see HzbResources.hpp.
 */

#include <lux/engine/render/resources/hzb/HzbResources.hpp>

#include <vk_mem_alloc.h>

#include <lux/engine/render/gpu/descriptor/SceneDescriptorArena.hpp>
#include <lux/engine/render/gpu/lifecycle/DeferredDestroyQueue.hpp>

#include <algorithm>
#include <array>
#include <cstring>

namespace lux::render
{
    static_assert(sizeof(HzbResources::ViewParams) == 112u,
                  "HzbResources::ViewParams must be 112 bytes (std140 exact-origin layout)");

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

    bool HzbResources::initSlot(Slot& s, const ViewSlots& geom)
    {
        const uint32_t width_     = geom.width;
        const uint32_t height_    = geom.height;
        const uint32_t mip_count_ = geom.mip_count;

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
        // 退役而非就地销毁:evictView 那条路径(RenderScene::removeView 的视图
        // 销毁钩子)不等 GPU 空闲,在飞帧的命令缓冲/每 mip 构建描述符集仍在
        // 引用这些 image 与 image view。见头文件"回收纪律"。
        if (destroy_queue_ != nullptr)
        {
            for (VkImageView v : s.mip_views)
                if (v != VK_NULL_HANDLE)
                    destroy_queue_->retireImageView(v);
            if (s.full_view != VK_NULL_HANDLE)
                destroy_queue_->retireImageView(s.full_view);
            if (s.image != VK_NULL_HANDLE)
                destroy_queue_->retireImage(s.image, s.alloc);
            if (s.ubo != VK_NULL_HANDLE)
                destroy_queue_->retireBuffer(s.ubo, s.ubo_alloc);
        }
        else
        {
            // 无队列 = 独立/无头用法,调用方自行 waitIdle(见 InitInfo)。
            if (device_ != VK_NULL_HANDLE)
            {
                for (VkImageView v : s.mip_views)
                    vkDestroyImageView(device_, v, nullptr);
                vkDestroyImageView(device_, s.full_view, nullptr);
            }
            if (s.image != VK_NULL_HANDLE && allocator_ != VK_NULL_HANDLE)
                vmaDestroyImage(allocator_, s.image, s.alloc);
            if (s.ubo != VK_NULL_HANDLE && allocator_ != VK_NULL_HANDLE)
                vmaDestroyBuffer(allocator_, s.ubo, s.ubo_alloc);
        }

        s.mip_views.clear();
        s.full_view  = VK_NULL_HANDLE;
        s.image      = VK_NULL_HANDLE;
        s.alloc      = VK_NULL_HANDLE;
        s.ubo        = VK_NULL_HANDLE;
        s.ubo_alloc  = VK_NULL_HANDLE;
        s.ubo_mapped = nullptr;
        s.read_ds    = VK_NULL_HANDLE;   // arena owns the set; just drop our handle
    }

    void HzbResources::destroyViewSlots(ViewSlots& vs)
    {
        for (Slot& s : vs.slots)
            destroySlot(s);
        vs.cur = 0u;
        vs.mip_count = vs.width = vs.height = 0u;
    }

    bool HzbResources::init(const InitInfo& info)
    {
        if (info.device == VK_NULL_HANDLE || info.allocator == VK_NULL_HANDLE)
            return false;

        destroy();

        device_        = info.device;
        allocator_     = info.allocator;
        destroy_queue_ = info.deferred_queue;
        arena_         = info.arena;
        read_layout_   = info.read_layout;
        sampler_       = info.sampler;
        // No images here: they are per view and per extent — see ensureView().
        return true;
    }

    void HzbResources::destroy()
    {
        // values() is const-only, so go through the keys. Copied because
        // BasicSparseSet::erase is swap-with-last — even though the loop below
        // does not erase, taking a snapshot keeps this correct if it ever does.
        const std::vector<uint32_t> ids = views_.keys();
        for (uint32_t id : ids)
            if (ViewSlots* vs = views_.tryGet(id))
                destroyViewSlots(*vs);
        views_.clear();

        device_        = VK_NULL_HANDLE;
        allocator_     = VK_NULL_HANDLE;
        destroy_queue_ = nullptr;
        arena_         = nullptr;
        read_layout_   = VK_NULL_HANDLE;
        sampler_       = VK_NULL_HANDLE;
    }

    bool HzbResources::ensureView(uint32_t view_id, uint32_t width, uint32_t height)
    {
        if (device_ == VK_NULL_HANDLE || allocator_ == VK_NULL_HANDLE
            || width == 0u || height == 0u)
            return false;

        if (ViewSlots* existing = views_.tryGet(view_id))
        {
            if (existing->width == width && existing->height == height
                && existing->slots[0].image != VK_NULL_HANDLE)
                return true;                     // idempotent: same extent, already built
            destroyViewSlots(*existing);         // 旧句柄退役到队列(见头文件"回收纪律")
        }

        ViewSlots& vs = views_[view_id];
        vs.width     = width;
        vs.height    = height;
        vs.mip_count = mipCountFor(width, height);
        vs.cur       = 0u;

        for (Slot& s : vs.slots)
            if (!initSlot(s, vs)) { destroyViewSlots(vs); return false; }
        return true;
    }

    void HzbResources::evictView(uint32_t view_id)
    {
        if (ViewSlots* vs = views_.tryGet(view_id))
        {
            destroyViewSlots(*vs);
            views_.erase(view_id);
        }
    }

    bool HzbResources::viewReady(uint32_t view_id) const noexcept
    {
        const ViewSlots* vs = views_.tryGet(view_id);
        return vs != nullptr && vs->slots[0].image != VK_NULL_HANDLE;
    }

    void HzbResources::setCurrent(uint32_t view_id, uint32_t parity) noexcept
    {
        if (ViewSlots* vs = views_.tryGet(view_id))
            vs->cur = parity & 1u;
    }

    uint32_t HzbResources::curIndex(uint32_t view_id) const noexcept
    {
        const ViewSlots* vs = views_.tryGet(view_id);
        return vs ? vs->cur : 0u;
    }

    uint32_t HzbResources::prevIndex(uint32_t view_id) const noexcept
    {
        const ViewSlots* vs = views_.tryGet(view_id);
        return vs ? (vs->cur ^ 1u) : 1u;
    }

    uint32_t HzbResources::mipCount(uint32_t view_id) const noexcept
    {
        const ViewSlots* vs = views_.tryGet(view_id);
        return vs ? vs->mip_count : 0u;
    }

    uint32_t HzbResources::width(uint32_t view_id) const noexcept
    {
        const ViewSlots* vs = views_.tryGet(view_id);
        return vs ? vs->width : 0u;
    }

    uint32_t HzbResources::height(uint32_t view_id) const noexcept
    {
        const ViewSlots* vs = views_.tryGet(view_id);
        return vs ? vs->height : 0u;
    }

    void HzbResources::writeBuildDescriptors(VkDevice device,
                                             uint32_t view_id,
                                             uint32_t slot,
                                             const VkDescriptorSet* mip_sets,
                                             uint32_t mip_set_count) const
    {
        const ViewSlots* vs = views_.tryGet(view_id);
        if (vs == nullptr || slot >= 2u) return;
        const Slot& s = vs->slots[slot];
        const uint32_t n = std::min(mip_set_count, vs->mip_count);
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
                                   uint32_t view_id,
                                   uint32_t slot,
                                   const VkDescriptorSet* mip_sets,
                                   uint32_t mip_set_count,
                                   VkPipeline pipeline,
                                   VkDescriptorSet depth_set) const
    {
        const ViewSlots* vs = views_.tryGet(view_id);
        if (vs == nullptr || slot >= 2u) return;
        const VkImage image = vs->slots[slot].image;
        if (image == VK_NULL_HANDLE) return;

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

        const uint32_t n = std::min(mip_set_count, vs->mip_count);
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
            pc.dst_w   = vs->mipWidth(k);
            pc.dst_h   = vs->mipHeight(k);
            pc.src_w   = (k == 0u) ? vs->mipWidth(0u)  : vs->mipWidth(k - 1u);
            pc.src_h   = (k == 0u) ? vs->mipHeight(0u) : vs->mipHeight(k - 1u);
            pc.is_mip0 = (k == 0u) ? 1u : 0u;
            vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT,
                               0u, sizeof(pc), &pc);

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

    void HzbResources::recordInitToGeneral(VkCommandBuffer cmd, uint32_t view_id) const
    {
        const ViewSlots* vs = views_.tryGet(view_id);
        if (vs == nullptr) return;

        // forward-Z: far = 1.0 = "nothing in front" → an unbuilt/just-reset slot
        // reads as far everywhere, so the cull's max-Z test degrades to NO cull
        // (objects stay visible) instead of over-culling on garbage memory.
        const VkClearColorValue       far_clear{ { 1.0f, 1.0f, 1.0f, 1.0f } };
        const VkImageSubresourceRange range{ VK_IMAGE_ASPECT_COLOR_BIT, 0u, vs->mip_count, 0u, 1u };
        for (uint32_t s = 0u; s < 2u; ++s)
        {
            if (vs->slots[s].image == VK_NULL_HANDLE) continue;

            // 1. UNDEFINED → GENERAL (discard garbage), ready for the clear (TRANSFER write).
            VkImageMemoryBarrier to_general{};
            to_general.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            to_general.srcAccessMask       = 0;
            to_general.dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
            to_general.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
            to_general.newLayout           = VK_IMAGE_LAYOUT_GENERAL;
            to_general.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            to_general.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            to_general.image               = vs->slots[s].image;
            to_general.subresourceRange    = range;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 0, 0, nullptr, 0, nullptr, 1, &to_general);

            // 2. Clear every mip to far.
            vkCmdClearColorImage(cmd, vs->slots[s].image, VK_IMAGE_LAYOUT_GENERAL, &far_clear, 1u, &range);

            // 3. Make the clear visible to the build's / cull's shader access.
            VkImageMemoryBarrier to_shader{};
            to_shader.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            to_shader.srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
            to_shader.dstAccessMask       = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
            to_shader.oldLayout           = VK_IMAGE_LAYOUT_GENERAL;
            to_shader.newLayout           = VK_IMAGE_LAYOUT_GENERAL;
            to_shader.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            to_shader.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            to_shader.image               = vs->slots[s].image;
            to_shader.subresourceRange    = range;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 0, 0, nullptr, 0, nullptr, 1, &to_shader);
        }
    }

    void HzbResources::writeViewParams(uint32_t view_id, uint32_t slot,
                                       const ViewParams& vp) noexcept
    {
        ViewSlots* vs = views_.tryGet(view_id);
        if (vs != nullptr && slot < 2u && vs->slots[slot].ubo_mapped != nullptr)
            std::memcpy(vs->slots[slot].ubo_mapped, &vp, sizeof(ViewParams));
    }

    VkDescriptorSet HzbResources::resolveHzbReadDS(const void* self, uint32_t /*frame_slot*/,
                                                   uint32_t view_id) noexcept
    {
        // Ignore the framework FIF slot — return THIS VIEW's PREVIOUS ping-pong
        // slot read DS, i.e. that view's last-frame pyramid (correct for any
        // frames-in-flight). A view with no pyramid yet yields VK_NULL_HANDLE;
        // the recorder skips the bind and the cull shader keeps everything.
        const auto* h  = static_cast<const HzbResources*>(self);
        const ViewSlots* vs = h->views_.tryGet(view_id);
        if (vs == nullptr) return VK_NULL_HANDLE;
        return vs->slots[vs->cur ^ 1u].read_ds;
    }

} // namespace lux::render
