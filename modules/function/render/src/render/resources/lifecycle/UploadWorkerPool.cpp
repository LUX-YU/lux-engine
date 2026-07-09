#include <lux/engine/render/resources/lifecycle/UploadWorkerPool.hpp>
#include <lux/engine/render/core/VulkanContext.hpp>

#include <vk_mem_alloc.h>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>

namespace lux::render
{
    namespace
    {
        [[nodiscard]] bool isCompressedPixelFormat(EPixelFormat fmt) noexcept
        {
            switch (fmt)
            {
            case EPixelFormat::BC1_SRGB:
            case EPixelFormat::BC3_SRGB:
            case EPixelFormat::BC5_UNORM:
            case EPixelFormat::BC7_SRGB:
                return true;
            default:
                return false;
            }
        }
    }

    // =========================================================================
    //  Construction / Destruction
    // =========================================================================
    UploadWorkerPool::UploadWorkerPool(const Config &cfg)
        : pool_(cfg.worker_count, cfg.queue_capacity), completions_(cfg.completion_ring), pending_submits_(cfg.pending_submit_ring), vma_(cfg.device_ctx->vmaAllocator()), device_(cfg.device_ctx->logicalDevice().handle()), transfer_queue_(cfg.device_ctx->transferQueue().handle()), transfer_family_(cfg.device_ctx->transferQueueFamilyIndex()), graphics_family_(cfg.device_ctx->graphicsQueueFamilyIndex()), needs_ownership_transfer_(transfer_family_ != graphics_family_)
    {
        mode_ = cfg.device_ctx->hasTransferQueue()
                    ? UploadMode::TransferQueue
                    : UploadMode::StagingOnly;

        // Timeline semaphore
        VkSemaphoreTypeCreateInfo timeline_ci{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
        timeline_ci.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        timeline_ci.initialValue = 0;
        VkSemaphoreCreateInfo sem_ci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        sem_ci.pNext = &timeline_ci;
        { VkResult r = vkCreateSemaphore(device_, &sem_ci, nullptr, &timeline_sem_);
          assert(r == VK_SUCCESS && "vkCreateSemaphore failed"); }

        // Command pools (transfer queue family). A lease now lives from the
        // worker recording the CB until the render thread submits + releases it
        // (one tick later), so the free-list must cover every lease that can be
        // outstanding at once = (packets queued in pending_submits_) + (workers
        // mid-record / blocked on a full ring). With pending_submit_ring +
        // worker_count pools the free-list can never starve → acquireCmdPool
        // never blocks and its assert stays a true invariant.
        const uint32_t pool_count = cfg.cmd_pool_count > 0
                                        ? cfg.cmd_pool_count
                                        : (cfg.pending_submit_ring + cfg.worker_count);
        cmd_pools_.resize(pool_count);
        free_pool_indices_.reserve(pool_count);
        for (uint32_t i = 0; i < pool_count; ++i)
        {
            VkCommandPoolCreateInfo pool_ci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
            pool_ci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
            pool_ci.queueFamilyIndex = transfer_family_;
            VkResult pool_r = vkCreateCommandPool(device_, &pool_ci, nullptr, &cmd_pools_[i].pool);
            assert(pool_r == VK_SUCCESS && "vkCreateCommandPool failed");
            free_pool_indices_.push_back(i);
        }
    }

    UploadWorkerPool::~UploadWorkerPool()
    {
        shutdown();
    }

    void UploadWorkerPool::shutdown()
    {
        // Close the recorded-CB hand-off ring FIRST so a worker blocked on a
        // full ring's push() returns immediately — otherwise pool_.close()'s
        // thread join would hang waiting for that worker. push() returns false
        // WITHOUT moving its argument on a closed queue, so the worker still
        // cleans up the dropped packet's GPU resources (see the handlers).
        // (completions_ is not closed: in TransferQueue mode only the render
        // thread pushes to it, and it is idle here; StagingOnly keeps its
        // existing teardown behaviour untouched.)
        pending_submits_.close();

        pool_.close();
        vkDeviceWaitIdle(device_);

        // Free any packets the workers recorded but the render thread never got
        // to submit (CBs are released by the command-pool destroy below; the
        // staging buffer + per-task texture image/view/sampler are not, so free
        // them here to keep teardown leak-free).
        {
            PendingSubmit leftover;
            while (pending_submits_.try_pop(leftover))
                freeUnsubmittedCompletion(leftover.completion);
        }

        for (auto &slot : cmd_pools_)
        {
            if (slot.pool != VK_NULL_HANDLE)
                vkDestroyCommandPool(device_, slot.pool, nullptr);
            slot.pool = VK_NULL_HANDLE;
        }
        cmd_pools_.clear();

        if (timeline_sem_ != VK_NULL_HANDLE)
        {
            vkDestroySemaphore(device_, timeline_sem_, nullptr);
            timeline_sem_ = VK_NULL_HANDLE;
        }
    }

    // =========================================================================
    //  CmdPool lease
    // =========================================================================

    CmdPoolLease UploadWorkerPool::acquireCmdPool()
    {
        uint32_t      idx;
        uint64_t      wait_val;
        VkCommandPool pool;
        {
            std::lock_guard lock(pool_alloc_mutex_);
            assert(!free_pool_indices_.empty() && "All command pools leased — increase cmd_pool_count");
            idx = free_pool_indices_.back();
            free_pool_indices_.pop_back();
            wait_val = cmd_pools_[idx].last_timeline_value;
            pool     = cmd_pools_[idx].pool;
        }

        // The popped slot is exclusively owned by this worker (removed from
        // free_pool_indices_ until releaseCmdPool re-pushes it), so the GPU wait +
        // reset need no lock. Holding pool_alloc_mutex_ across the unbounded
        // vkWaitSemaphores serialized ALL upload workers — one slow transfer
        // collapsed worker parallelism to 1 whenever a pool was reused before its
        // previous submit retired. (P-2)
        if (wait_val > 0)
        {
            VkSemaphoreWaitInfo wi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
            wi.semaphoreCount = 1;
            wi.pSemaphores    = &timeline_sem_;
            wi.pValues        = &wait_val;
            vkWaitSemaphores(device_, &wi, UINT64_MAX);
        }

        vkResetCommandPool(device_, pool, 0);
        return {pool, idx};
    }

    void UploadWorkerPool::releaseCmdPool(CmdPoolLease lease)
    {
        std::lock_guard lock(pool_alloc_mutex_);
        free_pool_indices_.push_back(lease.index);
    }

    // =========================================================================
    //  Internal helpers
    // =========================================================================

    UploadWorkerPool::StagingResult UploadWorkerPool::allocStagingBuffer(VkDeviceSize bytes)
    {
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size = bytes;
        bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;

        VmaAllocationCreateInfo aci{};
        aci.usage = VMA_MEMORY_USAGE_AUTO;
        aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;

        VmaAllocationInfo info{};
        VkBuffer buf = VK_NULL_HANDLE;
        VmaAllocation alloc = nullptr;
        if (vmaCreateBuffer(vma_, &bci, &aci, &buf, &alloc, &info) != VK_SUCCESS)
            return {};

        return {buf, alloc, info.pMappedData};
    }

    // =========================================================================
    //  flushPendingSubmits  (render thread — the SOLE Transfer Queue submitter)
    // =========================================================================
    uint32_t UploadWorkerPool::flushPendingSubmits(TransferCompletion *out, uint32_t max)
    {
        // StagingOnly never enqueues recorded CBs (workers push completions with
        // timeline_value==0 straight to the completion ring).
        if (mode_ != UploadMode::TransferQueue || max == 0)
            return 0;

        uint32_t count = 0;
        PendingSubmit ps;
        while (count < max && pending_submits_.try_pop(ps))
        {
            // Timeline values are assigned here, in submit order on the queue —
            // monotonic by construction (single submitter, no atomic contention).
            const uint64_t my_value =
                timeline_counter_.fetch_add(1, std::memory_order_relaxed) + 1;

            VkSemaphoreSubmitInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
            signal.semaphore  = timeline_sem_;
            signal.value      = my_value;
            signal.stageMask  = VK_PIPELINE_STAGE_2_COPY_BIT;

            VkCommandBufferSubmitInfo cb_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
            cb_info.commandBuffer = ps.cmd;

            VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
            si.commandBufferInfoCount   = 1;
            si.pCommandBufferInfos      = &cb_info;
            si.signalSemaphoreInfoCount = 1;
            si.pSignalSemaphoreInfos    = &signal;
            // No queue mutex: this is the only thread that ever submits to
            // transfer_queue_ (the frame submit is on this same render thread).
            vkQueueSubmit2(transfer_queue_, 1, &si, VK_NULL_HANDLE);

            // Publish last_timeline_value BEFORE releasing the lease: the release
            // (pool_alloc_mutex_ unlock) synchronises-with the next worker's
            // acquireCmdPool (lock), so the value is visible before that worker
            // can pop this slot and wait on it.
            cmd_pools_[ps.lease.index].last_timeline_value = my_value;
            releaseCmdPool(ps.lease);

            // Hand the completion back to the caller (timeline_value now filled).
            // It just submitted, so its transfer hasn't retired — the render
            // thread defers it into pending_completions_ like any drained one,
            // finalizing once the timeline reaches my_value. NOT pushed through
            // the completion ring: that would be a blocking push on the very
            // thread that drains the ring (deadlock if it filled).
            ps.completion.timeline_value = my_value;
            out[count++] = std::move(ps.completion);
        }
        return count;
    }

    void UploadWorkerPool::freeUnsubmittedCompletion(TransferCompletion &c)
    {
        using Kind = TransferCompletion::Kind;
        if (c.kind == Kind::Texture2D || c.kind == Kind::TextureCube)
        {
            if (c.texture.view != VK_NULL_HANDLE)
                vkDestroyImageView(device_, c.texture.view, nullptr);
            if (c.texture.sampler != VK_NULL_HANDLE)
                vkDestroySampler(device_, c.texture.sampler, nullptr);
            if (c.texture.image != VK_NULL_HANDLE)
                vmaDestroyImage(vma_, c.texture.image, c.texture.image_alloc);
        }
        if (c.stg_buf != VK_NULL_HANDLE)
            vmaDestroyBuffer(vma_, c.stg_buf, c.stg_alloc);
    }

    VkFormat UploadWorkerPool::toVkFormat(EPixelFormat fmt)
    {
        switch (fmt)
        {
        case EPixelFormat::RGBA8_SRGB:
            return VK_FORMAT_R8G8B8A8_SRGB;
        case EPixelFormat::RGBA8_UNORM:
            return VK_FORMAT_R8G8B8A8_UNORM;
        case EPixelFormat::RGBA16_SFLOAT:
            return VK_FORMAT_R16G16B16A16_SFLOAT;
        case EPixelFormat::RG8_UNORM:
            return VK_FORMAT_R8G8_UNORM;
        case EPixelFormat::R8_UNORM:
            return VK_FORMAT_R8_UNORM;
        case EPixelFormat::BC1_SRGB:
            return VK_FORMAT_BC1_RGBA_SRGB_BLOCK;
        case EPixelFormat::BC3_SRGB:
            return VK_FORMAT_BC3_SRGB_BLOCK;
        case EPixelFormat::BC5_UNORM:
            return VK_FORMAT_BC5_UNORM_BLOCK;
        case EPixelFormat::BC7_SRGB:
            return VK_FORMAT_BC7_SRGB_BLOCK;
        case EPixelFormat::R16_UINT:
            return VK_FORMAT_R16_UINT;
        default:
            return VK_FORMAT_R8G8B8A8_SRGB;
        }
    }

    bool UploadWorkerPool::hasTransferQueue() const noexcept
    {
        return mode_ == UploadMode::TransferQueue;
    }

    // =========================================================================
    //  Drain completions (render thread)
    // =========================================================================

    uint32_t UploadWorkerPool::drainCompletions(TransferCompletion *out, uint32_t max)
    {
        const auto n = static_cast<uint32_t>(completions_.try_pop_bulk(out, max));

        uint64_t local_max = max_completed_timeline_.load(std::memory_order_relaxed);
        for (uint32_t i = 0; i < n; ++i)
            local_max = std::max(local_max, out[i].timeline_value);
        if (n > 0)
            max_completed_timeline_.store(local_max, std::memory_order_relaxed);

        return n;
    }

    // =========================================================================
    //  Mesh transfer
    // =========================================================================

    void UploadWorkerPool::submitMeshTransfer(MeshTransferTask task)
    {
        if (mode_ == UploadMode::TransferQueue)
        {
            pool_.submit([this, task = std::move(task)](std::stop_token st) mutable
                         {
            if (st.stop_requested()) return;

            // 1. Staging alloc + memcpy
            const VkDeviceSize total = task.vbo_bytes + task.ibo_bytes;
            auto stg = allocStagingBuffer(total);
            if (!stg.mapped) return;

            std::memcpy(stg.mapped, task.vbo_data, task.vbo_bytes);
            if (task.ibo_bytes > 0)
                std::memcpy(static_cast<std::byte*>(stg.mapped) + task.vbo_bytes,
                            task.ibo_data, task.ibo_bytes);

            // 2. Record transfer commands
            auto lease = acquireCmdPool();
            VkCommandBufferAllocateInfo cb_ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            cb_ai.commandPool        = lease.pool;
            cb_ai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            cb_ai.commandBufferCount = 1;
            VkCommandBuffer tcb;
            vkAllocateCommandBuffers(device_, &cb_ai, &tcb);

            VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            vkBeginCommandBuffer(tcb, &begin);

            // VBO copy
            VkBufferCopy vbo_region{};
            vbo_region.srcOffset = 0;
            vbo_region.dstOffset = task.vbo_offset;
            vbo_region.size      = task.vbo_bytes;
            vkCmdCopyBuffer(tcb, stg.buf, task.vbo_buf, 1, &vbo_region);

            // IBO copy
            if (task.ibo_bytes > 0)
            {
                VkBufferCopy ibo_region{};
                ibo_region.srcOffset = task.vbo_bytes;
                ibo_region.dstOffset = task.ibo_offset;
                ibo_region.size      = task.ibo_bytes;
                vkCmdCopyBuffer(tcb, stg.buf, task.ibo_buf, 1, &ibo_region);
            }

            // Queue Ownership Transfer: release barrier
            if (needs_ownership_transfer_)
            {
                std::array<VkBufferMemoryBarrier2, 2> barriers{};
                uint32_t count = 0;

                auto& vbo_rel       = barriers[count++];
                vbo_rel.sType       = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
                vbo_rel.srcStageMask  = VK_PIPELINE_STAGE_2_COPY_BIT;
                vbo_rel.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
                vbo_rel.dstStageMask  = VK_PIPELINE_STAGE_2_NONE;
                vbo_rel.dstAccessMask = VK_ACCESS_2_NONE;
                vbo_rel.srcQueueFamilyIndex = transfer_family_;
                vbo_rel.dstQueueFamilyIndex = graphics_family_;
                vbo_rel.buffer = task.vbo_buf;
                vbo_rel.offset = task.vbo_offset;
                vbo_rel.size   = task.vbo_bytes;

                if (task.ibo_bytes > 0)
                {
                    auto& ibo_rel       = barriers[count++];
                    ibo_rel.sType       = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
                    ibo_rel.srcStageMask  = VK_PIPELINE_STAGE_2_COPY_BIT;
                    ibo_rel.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
                    ibo_rel.dstStageMask  = VK_PIPELINE_STAGE_2_NONE;
                    ibo_rel.dstAccessMask = VK_ACCESS_2_NONE;
                    ibo_rel.srcQueueFamilyIndex = transfer_family_;
                    ibo_rel.dstQueueFamilyIndex = graphics_family_;
                    ibo_rel.buffer = task.ibo_buf;
                    ibo_rel.offset = task.ibo_offset;
                    ibo_rel.size   = task.ibo_bytes;
                }

                VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
                dep.bufferMemoryBarrierCount = count;
                dep.pBufferMemoryBarriers    = barriers.data();
                vkCmdPipelineBarrier2(tcb, &dep);
            }

            // 4. Finish recording — the render thread submits it (no worker submit).
            vkEndCommandBuffer(tcb);

            // 5. Hand off the recorded CB + completion (timeline_value assigned
            //    by flushPendingSubmits at submit time).
            TransferCompletion tc{};
            tc.kind            = TransferCompletion::Kind::MeshBuffer;
            tc.request_id      = task.request_id;
            tc.resource_handle = task.mesh_index;
            tc.resource_gen    = task.resource_gen;
            tc.stg_buf         = stg.buf;
            tc.stg_alloc       = stg.alloc;
            tc.mesh.vbo_buf    = task.vbo_buf;
            tc.mesh.vbo_offset = task.vbo_offset;
            tc.mesh.vbo_size   = task.vbo_bytes;
            tc.mesh.ibo_buf    = task.ibo_buf;
            tc.mesh.ibo_offset = task.ibo_offset;
            tc.mesh.ibo_size   = task.ibo_bytes;
            tc.mesh.mesh_index = task.mesh_index;

            PendingSubmit ps{};
            ps.cmd        = tcb;
            ps.lease      = lease;
            ps.completion = std::move(tc);
            if (!pending_submits_.push(std::move(ps)))
                freeUnsubmittedCompletion(ps.completion); // ring closed at shutdown
            });
        }
        else // StagingOnly fallback
        {
            pool_.submit(
                [this, task = std::move(task)](std::stop_token st) mutable
                {
                    if (st.stop_requested()) return;

                    const VkDeviceSize total = task.vbo_bytes + task.ibo_bytes;
                    auto stg = allocStagingBuffer(total);
                    if (!stg.mapped) return;

                    std::memcpy(stg.mapped, task.vbo_data, task.vbo_bytes);
                    if (task.ibo_bytes > 0)
                    {
                        std::memcpy(
                            static_cast<std::byte*>(stg.mapped) + task.vbo_bytes,
                            task.ibo_data, task.ibo_bytes
                        );
                    }
                
                    TransferCompletion tc{};
                    tc.kind            = TransferCompletion::Kind::MeshBuffer;
                    tc.timeline_value  = 0;  // StagingOnly: render thread records copy
                    tc.request_id      = task.request_id;
                    tc.resource_handle = task.mesh_index;
                    tc.resource_gen    = task.resource_gen;
                    tc.stg_buf         = stg.buf;
                    tc.stg_alloc       = stg.alloc;
                    tc.mesh.vbo_buf    = task.vbo_buf;
                    tc.mesh.vbo_offset = task.vbo_offset;
                    tc.mesh.vbo_size   = task.vbo_bytes;
                    tc.mesh.ibo_buf    = task.ibo_buf;
                    tc.mesh.ibo_offset = task.ibo_offset;
                    tc.mesh.ibo_size   = task.ibo_bytes;
                    tc.mesh.mesh_index = task.mesh_index;
                    (void)completions_.push(std::move(tc)); 
                }
            );
        }
    }

    // =========================================================================
    //  Texture transfer
    // =========================================================================

    void UploadWorkerPool::submitTextureTransfer(TextureTransferTask task)
    {
        pool_.submit([this, task = std::move(task)](std::stop_token st) mutable
                     {
        if (st.stop_requested()) return;

        const uint32_t provided_mips = std::clamp<uint32_t>(
            task.mip_count,
            1u,
            kTextureTransferMaxMipCount);
        if (provided_mips == 0)
            return;

        for (uint32_t i = 0; i < provided_mips; ++i)
        {
            const auto& mip = task.mips[i];
            if (mip.data == nullptr || mip.bytes == 0 || mip.width <= 0 || mip.height <= 0)
                return;
        }

        const int32_t base_width = task.mips[0].width;
        const int32_t base_height = task.mips[0].height;

        const VkFormat vk_fmt = toVkFormat(task.format);
        const bool runtime_mips = task.gen_mips && !isCompressedPixelFormat(task.format) && provided_mips == 1;
        const uint32_t mip_levels = runtime_mips
            ? static_cast<uint32_t>(std::floor(std::log2(
                  std::max(base_width, base_height)))) + 1
            : provided_mips;

        std::array<TextureTransferMipCopy, kTextureTransferMaxMipCount> uploaded_mips{};
        VkDeviceSize total_bytes = 0;
        for (uint32_t i = 0; i < provided_mips; ++i)
        {
            uploaded_mips[i].buffer_offset = total_bytes;
            uploaded_mips[i].byte_size = static_cast<VkDeviceSize>(task.mips[i].bytes);
            uploaded_mips[i].mip_level = i;
            uploaded_mips[i].width = static_cast<uint32_t>(task.mips[i].width);
            uploaded_mips[i].height = static_cast<uint32_t>(task.mips[i].height);
            total_bytes += static_cast<VkDeviceSize>(task.mips[i].bytes);
        }

        // 1. Create GPU image
        VkImageCreateInfo img_ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        img_ci.imageType     = VK_IMAGE_TYPE_2D;
        img_ci.format        = vk_fmt;
        img_ci.extent        = {static_cast<uint32_t>(base_width),
                                static_cast<uint32_t>(base_height), 1};
        img_ci.mipLevels     = mip_levels;
        img_ci.arrayLayers   = 1;
        img_ci.samples       = VK_SAMPLE_COUNT_1_BIT;
        img_ci.tiling        = VK_IMAGE_TILING_OPTIMAL;
        img_ci.usage         = VK_IMAGE_USAGE_TRANSFER_DST_BIT
                             | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
                             | VK_IMAGE_USAGE_SAMPLED_BIT;
        img_ci.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
        img_ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        VkImage image;
        VmaAllocation image_alloc;
        VmaAllocationCreateInfo img_aci{};
        img_aci.usage = VMA_MEMORY_USAGE_GPU_ONLY;
        if (vmaCreateImage(vma_, &img_ci, &img_aci, &image, &image_alloc, nullptr) != VK_SUCCESS)
            return;

        // 2. Image view
        VkImageViewCreateInfo view_ci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view_ci.image    = image;
        view_ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_ci.format   = vk_fmt;
        view_ci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mip_levels, 0, 1};
        VkImageView view;
        if (vkCreateImageView(device_, &view_ci, nullptr, &view) != VK_SUCCESS)
        {
            vmaDestroyImage(vma_, image, image_alloc);
            return;
        }

        // 3. Sampler
        VkSamplerCreateInfo samp_ci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        samp_ci.magFilter    = VK_FILTER_LINEAR;
        samp_ci.minFilter    = VK_FILTER_LINEAR;
        samp_ci.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        samp_ci.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samp_ci.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samp_ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samp_ci.maxLod       = static_cast<float>(mip_levels);
        VkSampler sampler;
        if (vkCreateSampler(device_, &samp_ci, nullptr, &sampler) != VK_SUCCESS)
        {
            vkDestroyImageView(device_, view, nullptr);
            vmaDestroyImage(vma_, image, image_alloc);
            return;
        }

        // 4. Staging buffer + memcpy
        auto stg = allocStagingBuffer(total_bytes);
        if (!stg.mapped)
        {
            vkDestroySampler(device_, sampler, nullptr);
            vkDestroyImageView(device_, view, nullptr);
            vmaDestroyImage(vma_, image, image_alloc);
            return;
        }
        for (uint32_t i = 0; i < provided_mips; ++i)
        {
            std::memcpy(
                static_cast<std::byte*>(stg.mapped) + uploaded_mips[i].buffer_offset,
                task.mips[i].data,
                task.mips[i].bytes);
        }

        if (mode_ == UploadMode::TransferQueue)
        {
            // 5. Record commands
            auto lease = acquireCmdPool();
            VkCommandBufferAllocateInfo cb_ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            cb_ai.commandPool        = lease.pool;
            cb_ai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            cb_ai.commandBufferCount = 1;
            VkCommandBuffer tcb;
            vkAllocateCommandBuffers(device_, &cb_ai, &tcb);

            VkCommandBufferBeginInfo begin_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            vkBeginCommandBuffer(tcb, &begin_info);

            // UNDEFINED → TRANSFER_DST_OPTIMAL
            VkImageMemoryBarrier2 to_dst{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
            to_dst.srcStageMask  = VK_PIPELINE_STAGE_2_NONE;
            to_dst.srcAccessMask = VK_ACCESS_2_NONE;
            to_dst.dstStageMask  = VK_PIPELINE_STAGE_2_COPY_BIT;
            to_dst.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            to_dst.oldLayout     = VK_IMAGE_LAYOUT_UNDEFINED;
            to_dst.newLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            to_dst.image         = image;
            to_dst.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mip_levels, 0, 1};
            {
                VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
                dep.imageMemoryBarrierCount = 1;
                dep.pImageMemoryBarriers    = &to_dst;
                vkCmdPipelineBarrier2(tcb, &dep);
            }

            // Copy staged mip chain
            for (uint32_t i = 0; i < provided_mips; ++i)
            {
                VkBufferImageCopy2 copy{VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2};
                copy.bufferOffset     = uploaded_mips[i].buffer_offset;
                copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, uploaded_mips[i].mip_level, 0, 1};
                copy.imageExtent      = {uploaded_mips[i].width,
                                         uploaded_mips[i].height, 1};
                VkCopyBufferToImageInfo2 copy_info{VK_STRUCTURE_TYPE_COPY_BUFFER_TO_IMAGE_INFO_2};
                copy_info.srcBuffer      = stg.buf;
                copy_info.dstImage       = image;
                copy_info.dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                copy_info.regionCount    = 1;
                copy_info.pRegions       = &copy;
                vkCmdCopyBufferToImage2(tcb, &copy_info);
            }

            // Mips: same-family → blit here; cross-family → defer to render thread
            bool needs_mip_gen = false;
            if (runtime_mips && mip_levels > 1)
            {
                if (!needs_ownership_transfer_)
                {
                    // Same queue family — generate mips via blit chain
                    int32_t w = base_width, h = base_height;
                    for (uint32_t lvl = 1; lvl < mip_levels; ++lvl)
                    {
                        // src level (lvl-1): TRANSFER_DST → TRANSFER_SRC.
                        // Level 0 was produced by vkCmdCopyBufferToImage (COPY stage);
                        // every higher level was produced by the PREVIOUS
                        // vkCmdBlitImage2 (BLIT stage). Using COPY for lvl>=2 left
                        // the layout transition and next blit unsynchronized against
                        // the prior blit's write (RAW) — masked only because many
                        // GPUs serialize transfer work.
                        VkImageMemoryBarrier2 src_bar{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
                        src_bar.srcStageMask  = (lvl == 1u) ? VK_PIPELINE_STAGE_2_COPY_BIT
                                                            : VK_PIPELINE_STAGE_2_BLIT_BIT;
                        src_bar.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
                        src_bar.dstStageMask  = VK_PIPELINE_STAGE_2_BLIT_BIT;
                        src_bar.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
                        src_bar.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                        src_bar.newLayout     = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                        src_bar.image         = image;
                        src_bar.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, lvl - 1, 1, 0, 1};
                        VkDependencyInfo d1{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
                        d1.imageMemoryBarrierCount = 1;
                        d1.pImageMemoryBarriers    = &src_bar;
                        vkCmdPipelineBarrier2(tcb, &d1);

                        int32_t nw = std::max(1, w / 2);
                        int32_t nh = std::max(1, h / 2);

                        VkImageBlit2 blit{VK_STRUCTURE_TYPE_IMAGE_BLIT_2};
                        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, lvl - 1, 0, 1};
                        blit.srcOffsets[0]  = {0, 0, 0};
                        blit.srcOffsets[1]  = {w, h, 1};
                        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, lvl, 0, 1};
                        blit.dstOffsets[0]  = {0, 0, 0};
                        blit.dstOffsets[1]  = {nw, nh, 1};

                        VkBlitImageInfo2 blit_info{VK_STRUCTURE_TYPE_BLIT_IMAGE_INFO_2};
                        blit_info.srcImage       = image;
                        blit_info.srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                        blit_info.dstImage       = image;
                        blit_info.dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                        blit_info.regionCount    = 1;
                        blit_info.pRegions       = &blit;
                        blit_info.filter         = VK_FILTER_LINEAR;
                        vkCmdBlitImage2(tcb, &blit_info);

                        w = nw;
                        h = nh;
                    }

                    // Transition all mip levels to SHADER_READ_ONLY
                    // levels [0..mip_levels-2] are TRANSFER_SRC, last is TRANSFER_DST
                    if (mip_levels > 1)
                    {
                        VkImageMemoryBarrier2 final_bar{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
                        final_bar.srcStageMask  = VK_PIPELINE_STAGE_2_BLIT_BIT;
                        final_bar.srcAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
                        final_bar.dstStageMask  = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
                        final_bar.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
                        final_bar.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                        final_bar.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                        final_bar.image         = image;
                        final_bar.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0,
                                                      mip_levels - 1, 0, 1};

                        VkImageMemoryBarrier2 last_bar{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
                        last_bar.srcStageMask  = VK_PIPELINE_STAGE_2_BLIT_BIT;
                        last_bar.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
                        last_bar.dstStageMask  = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
                        last_bar.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
                        last_bar.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                        last_bar.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                        last_bar.image         = image;
                        last_bar.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT,
                                                      mip_levels - 1, 1, 0, 1};

                        std::array bars = {final_bar, last_bar};
                        VkDependencyInfo d2{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
                        d2.imageMemoryBarrierCount = static_cast<uint32_t>(bars.size());
                        d2.pImageMemoryBarriers    = bars.data();
                        vkCmdPipelineBarrier2(tcb, &d2);
                    }
                }
                else
                {
                    needs_mip_gen = true;  // Render thread generates mips after acquire
                }
            }
            else if (!runtime_mips || mip_levels == 1)
            {
                // No deferred mip generation: move image to SHADER_READ before any
                // optional queue-family ownership release.
                VkImageMemoryBarrier2 to_read{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
                to_read.srcStageMask  = VK_PIPELINE_STAGE_2_COPY_BIT;
                to_read.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
                to_read.dstStageMask  = needs_ownership_transfer_
                    ? VK_PIPELINE_STAGE_2_NONE
                    : VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
                to_read.dstAccessMask = needs_ownership_transfer_
                    ? VK_ACCESS_2_NONE
                    : VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
                to_read.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                to_read.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                to_read.image         = image;
                to_read.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mip_levels, 0, 1};
                VkDependencyInfo d3{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
                d3.imageMemoryBarrierCount = 1;
                d3.pImageMemoryBarriers    = &to_read;
                vkCmdPipelineBarrier2(tcb, &d3);
            }

            // Queue Ownership Transfer: release barrier
            if (needs_ownership_transfer_)
            {
                const VkImageLayout layout = needs_mip_gen
                    ? VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL
                    : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

                VkImageMemoryBarrier2 release{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
                release.srcStageMask       = VK_PIPELINE_STAGE_2_COPY_BIT;
                release.srcAccessMask      = VK_ACCESS_2_TRANSFER_WRITE_BIT;
                release.dstStageMask       = VK_PIPELINE_STAGE_2_NONE;
                release.dstAccessMask      = VK_ACCESS_2_NONE;
                release.oldLayout          = layout;
                release.newLayout          = layout;
                release.srcQueueFamilyIndex = transfer_family_;
                release.dstQueueFamilyIndex = graphics_family_;
                release.image              = image;
                release.subresourceRange   = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mip_levels, 0, 1};

                VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
                dep.imageMemoryBarrierCount = 1;
                dep.pImageMemoryBarriers    = &release;
                vkCmdPipelineBarrier2(tcb, &dep);
            }

            vkEndCommandBuffer(tcb); // render thread submits it (no worker submit)

            // Hand off recorded CB + completion (timeline_value set at submit).
            TransferCompletion tc{};
            tc.kind                  = TransferCompletion::Kind::Texture2D;
            tc.request_id            = task.request_id;
            tc.resource_handle       = task.slot_index;
            tc.resource_gen          = task.resource_gen;
            tc.stg_buf               = stg.buf;
            tc.stg_alloc             = stg.alloc;
            tc.stg_size              = total_bytes;
            tc.texture.image         = image;
            tc.texture.image_alloc   = image_alloc;
            tc.texture.view          = view;
            tc.texture.sampler       = sampler;
            tc.texture.format        = vk_fmt;
            tc.texture.mip_levels    = mip_levels;
            tc.texture.array_layers  = 1;
            tc.texture.width         = base_width;
            tc.texture.height        = base_height;
            tc.texture.slot_index    = task.slot_index;
            tc.texture.needs_mip_gen = needs_mip_gen;
            tc.texture.uploaded_mip_count = provided_mips;
            tc.texture.uploaded_mips = uploaded_mips;

            PendingSubmit ps{};
            ps.cmd        = tcb;
            ps.lease      = lease;
            ps.completion = std::move(tc);
            if (!pending_submits_.push(std::move(ps)))
                freeUnsubmittedCompletion(ps.completion); // ring closed at shutdown
        }
        else // StagingOnly
        {
            // Not recording commands — render thread will handle image creation + copy
            TransferCompletion tc{};
            tc.kind                  = TransferCompletion::Kind::Texture2D;
            tc.timeline_value        = 0;
            tc.request_id            = task.request_id;
            tc.resource_handle       = task.slot_index;
            tc.resource_gen          = task.resource_gen;
            tc.stg_buf               = stg.buf;
            tc.stg_alloc             = stg.alloc;
            tc.stg_size              = total_bytes;
            tc.texture.image         = image;
            tc.texture.image_alloc   = image_alloc;
            tc.texture.view          = view;
            tc.texture.sampler       = sampler;
            tc.texture.format        = vk_fmt;
            tc.texture.mip_levels    = mip_levels;
            tc.texture.array_layers  = 1;
            tc.texture.width         = base_width;
            tc.texture.height        = base_height;
            tc.texture.slot_index    = task.slot_index;
            tc.texture.needs_mip_gen = (runtime_mips && mip_levels > 1);
            tc.texture.uploaded_mip_count = provided_mips;
            tc.texture.uploaded_mips = uploaded_mips;
            (void)completions_.push(std::move(tc));
        } });
    }

    // =========================================================================
    //  Cube texture transfer
    // =========================================================================

    void UploadWorkerPool::submitCubeTransfer(CubeTransferTask task)
    {
        pool_.submit([this, task = std::move(task)](std::stop_token st) mutable
                     {
        if (st.stop_requested()) return;

        const VkFormat vk_fmt = toVkFormat(task.format);
        constexpr uint32_t kFaceCount = 6;

        // Calculate total staging bytes
        VkDeviceSize total_bytes = 0;
        for (uint32_t f = 0; f < kFaceCount; ++f)
            total_bytes += static_cast<VkDeviceSize>(task.faces[f].bytes);

        // 1. Create GPU cube image
        VkImageCreateInfo img_ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        img_ci.flags         = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
        img_ci.imageType     = VK_IMAGE_TYPE_2D;
        img_ci.format        = vk_fmt;
        img_ci.extent        = {static_cast<uint32_t>(task.face_size),
                                static_cast<uint32_t>(task.face_size), 1};
        img_ci.mipLevels     = 1;
        img_ci.arrayLayers   = kFaceCount;
        img_ci.samples       = VK_SAMPLE_COUNT_1_BIT;
        img_ci.tiling        = VK_IMAGE_TILING_OPTIMAL;
        img_ci.usage         = VK_IMAGE_USAGE_TRANSFER_DST_BIT
                             | VK_IMAGE_USAGE_SAMPLED_BIT;
        img_ci.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
        img_ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        VkImage image;
        VmaAllocation image_alloc;
        VmaAllocationCreateInfo img_aci{};
        img_aci.usage = VMA_MEMORY_USAGE_GPU_ONLY;
        if (vmaCreateImage(vma_, &img_ci, &img_aci, &image, &image_alloc, nullptr) != VK_SUCCESS)
            return;

        // 2. Image view
        VkImageViewCreateInfo view_ci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view_ci.image    = image;
        view_ci.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
        view_ci.format   = vk_fmt;
        view_ci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, kFaceCount};
        VkImageView view;
        if (vkCreateImageView(device_, &view_ci, nullptr, &view) != VK_SUCCESS)
        {
            vmaDestroyImage(vma_, image, image_alloc);
            return;
        }

        // 3. Sampler
        VkSamplerCreateInfo samp_ci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        samp_ci.magFilter    = VK_FILTER_LINEAR;
        samp_ci.minFilter    = VK_FILTER_LINEAR;
        samp_ci.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        samp_ci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samp_ci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samp_ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samp_ci.maxLod       = 1.0f;
        VkSampler sampler;
        if (vkCreateSampler(device_, &samp_ci, nullptr, &sampler) != VK_SUCCESS)
        {
            vkDestroyImageView(device_, view, nullptr);
            vmaDestroyImage(vma_, image, image_alloc);
            return;
        }

        // 4. Staging buffer
        auto stg = allocStagingBuffer(total_bytes);
        if (!stg.mapped)
        {
            vkDestroySampler(device_, sampler, nullptr);
            vkDestroyImageView(device_, view, nullptr);
            vmaDestroyImage(vma_, image, image_alloc);
            return;
        }

        // Pack all 6 faces into staging buffer
        VkDeviceSize offset = 0;
        for (uint32_t f = 0; f < kFaceCount; ++f)
        {
            std::memcpy(static_cast<std::byte*>(stg.mapped) + offset,
                        task.faces[f].data, task.faces[f].bytes);
            offset += static_cast<VkDeviceSize>(task.faces[f].bytes);
        }
        if (mode_ == UploadMode::TransferQueue)
        {
            auto lease = acquireCmdPool();
            VkCommandBufferAllocateInfo cb_ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            cb_ai.commandPool        = lease.pool;
            cb_ai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            cb_ai.commandBufferCount = 1;
            VkCommandBuffer tcb;
            vkAllocateCommandBuffers(device_, &cb_ai, &tcb);

            VkCommandBufferBeginInfo begin_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            vkBeginCommandBuffer(tcb, &begin_info);

            // UNDEFINED → TRANSFER_DST
            VkImageMemoryBarrier2 to_dst{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
            to_dst.srcStageMask  = VK_PIPELINE_STAGE_2_NONE;
            to_dst.srcAccessMask = VK_ACCESS_2_NONE;
            to_dst.dstStageMask  = VK_PIPELINE_STAGE_2_COPY_BIT;
            to_dst.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            to_dst.oldLayout     = VK_IMAGE_LAYOUT_UNDEFINED;
            to_dst.newLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            to_dst.image         = image;
            to_dst.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, kFaceCount};
            {
                VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
                dep.imageMemoryBarrierCount = 1;
                dep.pImageMemoryBarriers    = &to_dst;
                vkCmdPipelineBarrier2(tcb, &dep);
            }

            // Copy each face from staging
            VkDeviceSize buf_offset = 0;
            for (uint32_t f = 0; f < kFaceCount; ++f)
            {
                VkBufferImageCopy2 region{VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2};
                region.bufferOffset     = buf_offset;
                region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, f, 1};
                region.imageExtent      = {static_cast<uint32_t>(task.face_size),
                                           static_cast<uint32_t>(task.face_size), 1};
                VkCopyBufferToImageInfo2 ci{VK_STRUCTURE_TYPE_COPY_BUFFER_TO_IMAGE_INFO_2};
                ci.srcBuffer      = stg.buf;
                ci.dstImage       = image;
                ci.dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                ci.regionCount    = 1;
                ci.pRegions       = &region;
                vkCmdCopyBufferToImage2(tcb, &ci);

                buf_offset += static_cast<VkDeviceSize>(task.faces[f].bytes);
            }

            // Transition cube to SHADER_READ before any optional ownership release.
            VkImageMemoryBarrier2 to_read{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
            to_read.srcStageMask  = VK_PIPELINE_STAGE_2_COPY_BIT;
            to_read.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            to_read.dstStageMask  = needs_ownership_transfer_
                ? VK_PIPELINE_STAGE_2_NONE
                : VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
            to_read.dstAccessMask = needs_ownership_transfer_
                ? VK_ACCESS_2_NONE
                : VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
            to_read.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            to_read.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            to_read.image         = image;
            to_read.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, kFaceCount};

            {
                VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
                dep.imageMemoryBarrierCount = 1;
                dep.pImageMemoryBarriers    = &to_read;
                vkCmdPipelineBarrier2(tcb, &dep);
            }

            if (needs_ownership_transfer_)
            {
                VkImageMemoryBarrier2 release{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
                release.srcStageMask       = VK_PIPELINE_STAGE_2_NONE;
                release.srcAccessMask      = VK_ACCESS_2_NONE;
                release.dstStageMask       = VK_PIPELINE_STAGE_2_NONE;
                release.dstAccessMask      = VK_ACCESS_2_NONE;
                release.oldLayout          = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                release.newLayout          = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                release.srcQueueFamilyIndex = transfer_family_;
                release.dstQueueFamilyIndex = graphics_family_;
                release.image              = image;
                release.subresourceRange   = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, kFaceCount};

                VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
                dep.imageMemoryBarrierCount = 1;
                dep.pImageMemoryBarriers    = &release;
                vkCmdPipelineBarrier2(tcb, &dep);
            }

            vkEndCommandBuffer(tcb); // render thread submits it (no worker submit)

            TransferCompletion tc{};
            tc.kind                  = TransferCompletion::Kind::TextureCube;
            tc.request_id            = task.request_id;
            tc.resource_handle       = task.slot_index;
            tc.resource_gen          = task.resource_gen;
            tc.stg_buf               = stg.buf;
            tc.stg_alloc             = stg.alloc;
            tc.texture.image         = image;
            tc.texture.image_alloc   = image_alloc;
            tc.texture.view          = view;
            tc.texture.sampler       = sampler;
            tc.texture.format        = vk_fmt;
            tc.texture.mip_levels    = 1;
            tc.texture.array_layers  = kFaceCount;
            tc.texture.width         = task.face_size;
            tc.texture.height        = task.face_size;
            tc.texture.slot_index    = task.slot_index;
            tc.texture.needs_mip_gen = false;

            PendingSubmit ps{};
            ps.cmd        = tcb;
            ps.lease      = lease;
            ps.completion = std::move(tc);
            if (!pending_submits_.push(std::move(ps)))
                freeUnsubmittedCompletion(ps.completion); // ring closed at shutdown
        }
        else // StagingOnly
        {
            TransferCompletion tc{};
            tc.kind                  = TransferCompletion::Kind::TextureCube;
            tc.timeline_value        = 0;
            tc.request_id            = task.request_id;
            tc.resource_handle       = task.slot_index;
            tc.resource_gen          = task.resource_gen;
            tc.stg_buf               = stg.buf;
            tc.stg_alloc             = stg.alloc;
            tc.texture.image         = image;
            tc.texture.image_alloc   = image_alloc;
            tc.texture.view          = view;
            tc.texture.sampler       = sampler;
            tc.texture.format        = vk_fmt;
            tc.texture.mip_levels    = 1;
            tc.texture.array_layers  = kFaceCount;
            tc.texture.width         = task.face_size;
            tc.texture.height        = task.face_size;
            tc.texture.slot_index    = task.slot_index;
            tc.texture.needs_mip_gen = false;
            (void)completions_.push(std::move(tc));
        } });
    }

} // namespace lux::render
