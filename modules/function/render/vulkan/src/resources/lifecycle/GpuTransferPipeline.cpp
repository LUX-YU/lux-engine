#include <lux/engine/render/resources/lifecycle/GpuTransferPipeline.hpp>
#include <lux/engine/render/gpu/VulkanContext.hpp>
#include <lux/engine/function/render/client/core/RenderFatal.hpp>

#include <vk_mem_alloc.h>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>
#include <type_traits>

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
    GpuTransferPipeline::GpuTransferPipeline(const Config& cfg)
        : jobs_(cfg.queue_capacity), results_(std::max(cfg.result_capacity, cfg.queue_capacity + 1u)),
          notify_work_(cfg.notify_work), notify_work_state_(cfg.notify_work_state), lifecycle_(cfg.lifecycle),
          lifecycle_state_(cfg.lifecycle_state)
    {
    }

    Expected<std::unique_ptr<GpuTransferPipeline>> GpuTransferPipeline::create(const Config& config)
    {
        if (config.device_ctx == nullptr)
            return renderFailure<err::device::VulkanObjectCreationFailed>();

        auto pipeline = std::unique_ptr<GpuTransferPipeline>(new GpuTransferPipeline(config));
        auto initialized = pipeline->initialize(config);
        if (!initialized)
            return lux::cxx::unexpected<RenderError>(initialized.error());
        return pipeline;
    }

    Expected<void> GpuTransferPipeline::initialize(const Config& cfg)
    {
        device_context_ = cfg.device_ctx;
        transfer_queue_ = cfg.device_ctx->transferQueue().handle();
        graphics_queue_ = cfg.device_ctx->graphicsQueue().handle();
        transfer_queue_mutex_ = &cfg.device_ctx->transferQueueMutex();
        graphics_queue_mutex_ = &cfg.device_ctx->graphicsQueueMutex();
        transfer_family_ = cfg.device_ctx->transferQueueFamilyIndex();
        graphics_family_ = cfg.device_ctx->graphicsQueueFamilyIndex();
        needs_ownership_transfer_ = transfer_family_ != graphics_family_;
        vma_ = cfg.device_ctx->vmaAllocator();
        device_ = cfg.device_ctx->logicalDevice().handle();
        can_record_transfer_ = cfg.device_ctx->hasTransferQueue();
        mode_ = can_record_transfer_ && transfer_queue_ != graphics_queue_ ? EGpuTransferMode::DEDICATED_QUEUE
                                                                           : EGpuTransferMode::RECORD_ONLY;

        // Timeline semaphore
        VkSemaphoreTypeCreateInfo timeline_ci{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
        timeline_ci.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        timeline_ci.initialValue = 0;
        VkSemaphoreCreateInfo sem_ci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        sem_ci.pNext = &timeline_ci;
        const auto semaphore_result = vkCreateSemaphore(device_, &sem_ci, nullptr, &timeline_sem_);
        if (semaphore_result != VK_SUCCESS)
            return renderFailure<err::device::VulkanCallFailed>(encodeVkResult(semaphore_result));

        cmd_pool_count_ = std::max(2u, cfg.batch_slot_count);
        cmd_pools_ = std::make_unique<CmdPoolSlot[]>(cmd_pool_count_);
        for (uint32_t i = 0; i < cmd_pool_count_; ++i)
        {
            VkCommandPoolCreateInfo pool_ci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
            pool_ci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
            pool_ci.queueFamilyIndex = transfer_family_;
            const auto pool_result = vkCreateCommandPool(device_, &pool_ci, nullptr, &cmd_pools_[i].pool);
            if (pool_result != VK_SUCCESS)
                return renderFailure<err::device::VulkanCallFailed>(encodeVkResult(pool_result));
        }

        stop_requested_.store(false, std::memory_order_release);
        transfer_thread_ = std::thread([this] { workerLoop(TransferStopToken{&stop_requested_}); });
        return {};
    }

    GpuTransferPipeline::~GpuTransferPipeline()
    {
        shutdown();

        if (device_context_ != nullptr)
            (void)device_context_->waitIdle();

        for (; shutdown_completion_cursor_ < shutdown_completions_.size(); ++shutdown_completion_cursor_)
            freeUnsubmittedCompletion(shutdown_completions_[shutdown_completion_cursor_]);
        shutdown_completions_.clear();

        GpuTransferResult result{};
        while (results_.tryPop(result) == lux::cxx::EQueuePopResult::VALUE)
        {
            if (auto* batch = std::get_if<RecordedBatch>(&result))
                freeUnsubmittedCompletion(batch->completion);
            else
                freeUnsubmittedCompletion(std::get<TransferCompletion>(result));
        }

        for (uint32_t i = 0; i < cmd_pool_count_; ++i)
            if (cmd_pools_[i].pool != VK_NULL_HANDLE)
                vkDestroyCommandPool(device_, cmd_pools_[i].pool, nullptr);
        cmd_pools_.reset();
        cmd_pool_count_ = 0;

        if (timeline_sem_ != VK_NULL_HANDLE)
        {
            vkDestroySemaphore(device_, timeline_sem_, nullptr);
            timeline_sem_ = VK_NULL_HANDLE;
        }
    }

    void GpuTransferPipeline::shutdown()
    {
        if (shutdown_complete_)
            return;

        accepting_.store(false, std::memory_order_release);
        stop_requested_.store(true, std::memory_order_release);
        job_epoch_.fetch_add(1, std::memory_order_release);
        job_epoch_.notify_one();
        result_space_epoch_.fetch_add(1, std::memory_order_release);
        result_space_epoch_.notify_one();

        std::vector<TransferCompletion> drained;
        TransferCompletion buffer[32]{};
        while (worker_running_.load(std::memory_order_acquire))
        {
            const uint32_t count = drainResults(buffer, 32);
            for (uint32_t i = 0; i < count; ++i)
                drained.push_back(std::move(buffer[i]));
            if (count != 0)
                continue;

            const auto epoch = worker_epoch_.load(std::memory_order_acquire);
            if (worker_running_.load(std::memory_order_acquire))
                worker_epoch_.wait(epoch, std::memory_order_relaxed);
        }
        if (transfer_thread_.joinable())
            transfer_thread_.join();

        uint32_t count = 0;
        do
        {
            count = drainResults(buffer, 32);
            for (uint32_t i = 0; i < count; ++i)
                drained.push_back(std::move(buffer[i]));
        } while (count != 0);

        shutdown_completions_ = std::move(drained);
        shutdown_completion_cursor_ = 0;
        shutdown_complete_ = true;
    }

    // =========================================================================
    //  Fixed batch slots
    // =========================================================================

    BatchSlotLease GpuTransferPipeline::acquireBatchSlot()
    {
        for (;;)
        {
            for (uint32_t offset = 0; offset < cmd_pool_count_; ++offset)
            {
                const uint32_t idx = (next_cmd_pool_ + offset) % cmd_pool_count_;
                auto& slot = cmd_pools_[idx];
                auto state = slot.state.load(std::memory_order_acquire);
                if (state == EBatchSlotState::SUBMITTED)
                {
                    const uint64_t wait_value = slot.last_timeline_value.load(std::memory_order_acquire);
                    VkSemaphoreWaitInfo wait_info{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
                    wait_info.semaphoreCount = 1;
                    wait_info.pSemaphores = &timeline_sem_;
                    wait_info.pValues = &wait_value;
                    if (vkWaitSemaphores(device_, &wait_info, UINT64_MAX) != VK_SUCCESS)
                        renderFatal("GpuTransferPipeline timeline wait failed");
                    slot.state.store(EBatchSlotState::FREE, std::memory_order_release);
                    if (notify_work_ != nullptr)
                        notify_work_(notify_work_state_);
                    state = EBatchSlotState::FREE;
                }
                if (state != EBatchSlotState::FREE)
                    continue;

                slot.state.store(EBatchSlotState::RECORDING, std::memory_order_release);
                slot.last_timeline_value.store(0, std::memory_order_relaxed);
                if (vkResetCommandPool(device_, slot.pool, 0) != VK_SUCCESS)
                    renderFatal("GpuTransferPipeline command-pool reset failed");
                next_cmd_pool_ = (idx + 1u) % cmd_pool_count_;
                return {slot.pool, idx};
            }

            const auto epoch = worker_epoch_.load(std::memory_order_acquire);
            worker_epoch_.wait(epoch, std::memory_order_relaxed);
        }
    }

    void GpuTransferPipeline::releaseBatchSlot(BatchSlotLease slot) noexcept
    {
        auto& state = cmd_pools_[slot.index].state;
        state.store(EBatchSlotState::FREE, std::memory_order_release);
        state.notify_one();
        worker_epoch_.fetch_add(1, std::memory_order_release);
        worker_epoch_.notify_all();
    }

    void GpuTransferPipeline::releaseAfterGraphicsAcquire(uint32_t batch_slot) noexcept
    {
        if (batch_slot >= cmd_pool_count_)
            renderFatal("GpuTransferPipeline retained batch slot is invalid");
        auto& slot = cmd_pools_[batch_slot];
        if (slot.state.load(std::memory_order_acquire) != EBatchSlotState::RECORDED)
        {
            renderFatal("GpuTransferPipeline retained batch slot changed state before acquire");
        }
        releaseBatchSlot({slot.pool, batch_slot});
    }

    bool GpuTransferPipeline::retireOneSubmittedSlot()
    {
        for (uint32_t i = 0; i < cmd_pool_count_; ++i)
        {
            auto& slot = cmd_pools_[i];
            if (slot.state.load(std::memory_order_acquire) != EBatchSlotState::SUBMITTED)
                continue;

            const uint64_t wait_value = slot.last_timeline_value.load(std::memory_order_acquire);
            VkSemaphoreWaitInfo wait_info{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
            wait_info.semaphoreCount = 1;
            wait_info.pSemaphores = &timeline_sem_;
            wait_info.pValues = &wait_value;
            if (vkWaitSemaphores(device_, &wait_info, UINT64_MAX) != VK_SUCCESS)
                renderFatal("GpuTransferPipeline slot retirement failed");

            slot.state.store(EBatchSlotState::FREE, std::memory_order_release);
            slot.state.notify_one();
            worker_epoch_.fetch_add(1, std::memory_order_release);
            worker_epoch_.notify_all();
            if (notify_work_ != nullptr)
                notify_work_(notify_work_state_);
            return true;
        }
        return false;
    }

    bool GpuTransferPipeline::retireGraphicsFinalize()
    {
        const uint64_t wait_value = graphics_finalize_timeline_.exchange(0, std::memory_order_acq_rel);
        if (wait_value == 0)
            return false;

        VkSemaphoreWaitInfo wait_info{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
        wait_info.semaphoreCount = 1;
        wait_info.pSemaphores = &timeline_sem_;
        wait_info.pValues = &wait_value;
        if (vkWaitSemaphores(device_, &wait_info, UINT64_MAX) != VK_SUCCESS)
            renderFatal("GpuTransferPipeline graphics-finalize wait failed");
        if (notify_work_ != nullptr)
            notify_work_(notify_work_state_);
        return true;
    }

    // =========================================================================
    //  Internal helpers
    // =========================================================================

    GpuTransferPipeline::StagingResult GpuTransferPipeline::allocStagingBuffer(VkDeviceSize bytes)
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

    uint32_t GpuTransferPipeline::drainResults(TransferCompletion* out, uint32_t max)
    {
        if (max == 0)
            return 0;

        uint32_t count = 0;
        while (count < max && shutdown_completion_cursor_ < shutdown_completions_.size())
        {
            out[count++] = std::move(shutdown_completions_[shutdown_completion_cursor_++]);
        }
        if (shutdown_completion_cursor_ == shutdown_completions_.size() && !shutdown_completions_.empty())
        {
            shutdown_completions_.clear();
            shutdown_completion_cursor_ = 0;
        }

        GpuTransferResult result{};
        while (count < max && results_.tryPop(result) == lux::cxx::EQueuePopResult::VALUE)
        {
            result_space_epoch_.fetch_add(1, std::memory_order_release);
            result_space_epoch_.notify_one();

            if (auto* completion = std::get_if<TransferCompletion>(&result))
            {
                out[count++] = std::move(*completion);
                continue;
            }

            auto batch = std::move(std::get<RecordedBatch>(result));
            const uint64_t my_value = ++timeline_counter_;

            // The same timeline is signalled by both the graphics owner and,
            // in dedicated mode, the transfer owner. Allocation order alone
            // does not constrain execution order across Vulkan queues: N+1 may
            // otherwise complete first and make the later signal of N invalid.
            // Chain every submission through N-1 so the semaphore value is
            // globally monotonic, not merely monotonically allocated.
            VkSemaphoreSubmitInfo wait{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
            wait.semaphore = timeline_sem_;
            wait.value = my_value - 1u;
            wait.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

            VkSemaphoreSubmitInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
            signal.semaphore = timeline_sem_;
            signal.value = my_value;
            // The command buffer may end with a queue-family ownership
            // release barrier.  Signalling at COPY would allow the matching
            // graphics acquire to run after the copies but before that
            // release operation, which is both a validation error and an
            // ownership race.  The timeline value is the terminal state of
            // the whole recorded batch, not merely of its copy commands.
            signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

            VkCommandBufferSubmitInfo cb_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
            cb_info.commandBuffer = batch.cmd;

            VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
            if (my_value > 1u)
            {
                si.waitSemaphoreInfoCount = 1;
                si.pWaitSemaphoreInfos = &wait;
            }
            si.commandBufferInfoCount = 1;
            si.pCommandBufferInfos = &cb_info;
            si.signalSemaphoreInfoCount = 1;
            si.pSignalSemaphoreInfos = &signal;
            VkResult sub_res{VK_ERROR_UNKNOWN};
            {
                const std::scoped_lock queue_lock(*graphics_queue_mutex_);
                sub_res = vkQueueSubmit2(graphics_queue_, 1, &si, VK_NULL_HANDLE);
            }
            if (sub_res != VK_SUCCESS)
            {
                const auto kind = batch.completion.kind;
                const auto request_id = batch.completion.request_id;
                const auto resource_handle = batch.completion.resource_handle;
                const auto resource_gen = batch.completion.resource_gen;
                freeUnsubmittedCompletion(batch.completion);
                releaseBatchSlot(batch.slot);
                TransferCompletion fc{};
                fc.kind = kind;
                fc.failed = true;
                fc.request_id = request_id;
                fc.resource_handle = resource_handle;
                fc.resource_gen = resource_gen;
                out[count++] = fc;
                continue;
            }

            auto& slot = cmd_pools_[batch.slot.index];
            slot.last_timeline_value.store(my_value, std::memory_order_release);
            slot.state.store(EBatchSlotState::SUBMITTED, std::memory_order_release);
            slot.state.notify_one();
            worker_epoch_.fetch_add(1, std::memory_order_release);
            worker_epoch_.notify_one();
            job_epoch_.fetch_add(1, std::memory_order_release);
            job_epoch_.notify_one();

            batch.completion.timeline_value = my_value;
            out[count++] = std::move(batch.completion);
        }
        return count;
    }

    std::optional<std::uint64_t> GpuTransferPipeline::submitGraphicsFinalize(VkCommandBuffer command_buffer)
    {
        const uint64_t timeline_value = timeline_counter_.fetch_add(1, std::memory_order_relaxed) + 1u;

        VkSemaphoreSubmitInfo wait{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
        wait.semaphore = timeline_sem_;
        wait.value = timeline_value - 1u;
        wait.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

        VkSemaphoreSubmitInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
        signal.semaphore = timeline_sem_;
        signal.value = timeline_value;
        signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

        VkCommandBufferSubmitInfo command{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
        command.commandBuffer = command_buffer;

        VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
        if (timeline_value > 1u)
        {
            submit.waitSemaphoreInfoCount = 1;
            submit.pWaitSemaphoreInfos = &wait;
        }
        submit.commandBufferInfoCount = 1;
        submit.pCommandBufferInfos = &command;
        submit.signalSemaphoreInfoCount = 1;
        submit.pSignalSemaphoreInfos = &signal;
        VkResult submit_result{VK_ERROR_UNKNOWN};
        {
            const std::scoped_lock queue_lock(*graphics_queue_mutex_);
            submit_result = vkQueueSubmit2(graphics_queue_, 1, &submit, VK_NULL_HANDLE);
        }
        if (submit_result != VK_SUCCESS)
            return std::nullopt;

        uint64_t observed = graphics_finalize_timeline_.load(std::memory_order_relaxed);
        while (observed < timeline_value && !graphics_finalize_timeline_.compare_exchange_weak(
                                                observed,
                                                timeline_value,
                                                std::memory_order_release,
                                                std::memory_order_relaxed))
        {
        }
        job_epoch_.fetch_add(1, std::memory_order_release);
        job_epoch_.notify_one();
        return timeline_value;
    }

    void GpuTransferPipeline::freeUnsubmittedCompletion(TransferCompletion& c)
    {
        using Kind = TransferCompletion::Kind;
        if (c.kind == Kind::Texture2D || c.kind == Kind::TextureCube || c.kind == Kind::Texture2DReplacement)
        {
            vkDestroyImageView(device_, c.texture.view, nullptr);
            vkDestroySampler(device_, c.texture.sampler, nullptr);
            if (c.texture.image != VK_NULL_HANDLE)
                vmaDestroyImage(vma_, c.texture.image, c.texture.image_alloc);
        }
        if (c.stg_buf != VK_NULL_HANDLE)
            vmaDestroyBuffer(vma_, c.stg_buf, c.stg_alloc);
    }

    // Compile-time verification of the format-size contract the cube-stride bug
    // violated: bytes must follow the format, and BC formats block-align
    // each axis up to 4. Permanent, GPU-free, and impossible to let rot. (audit §8)
    static_assert(pixelFormatMipBytes(EPixelFormat::RGBA8_UNORM, 4, 4) == 64);       // 4bpp
    static_assert(pixelFormatMipBytes(EPixelFormat::R8_UNORM, 4, 4) == 16);          // 1bpp
    static_assert(pixelFormatMipBytes(EPixelFormat::RG8_UNORM, 2, 2) == 8);          // 2bpp
    static_assert(pixelFormatMipBytes(EPixelFormat::RGBA16_SFLOAT, 1, 1) == 8);      // 8bpp
    static_assert(pixelFormatMipBytes(EPixelFormat::BC1_SRGB, 4, 4) == 8);           // 1 block × 8
    static_assert(pixelFormatMipBytes(EPixelFormat::BC1_SRGB, 8, 8) == 32);          // 2×2 blocks × 8
    static_assert(pixelFormatMipBytes(EPixelFormat::BC3_SRGB, 4, 4) == 16);          // 1 block × 16
    static_assert(pixelFormatMipBytes(EPixelFormat::BC1_SRGB, 5, 5) == 32);          // block-align 5→2 blocks/axis
    static_assert(pixelFormatMipBytes(EPixelFormat::RGBA8_UNORM, 0, 4) == 0);        // zero extent → invalid
    static_assert(!pixelFormatBlockInfo(static_cast<EPixelFormat>(0xFF)).supported); // unknown → reject

    std::optional<VkFormat> GpuTransferPipeline::toVkFormat(EPixelFormat fmt)
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
        case EPixelFormat::R16_UNORM:
            return VK_FORMAT_R16_UNORM;
        }
        // Unknown/unsupported enumerator: fail loudly (nullopt) instead of the old
        // silent RGBA8_SRGB fallback, which masked protocol/version mismatch and
        // produced a mis-sized, mis-typed upload.
        return std::nullopt;
    }

    void GpuTransferPipeline::pushFailure(
        TransferCompletion::Kind kind,
        uint32_t request_id,
        uint32_t slot_index,
        uint32_t resource_gen,
        uint32_t logical_base_mip
    )
    {
        // A task with no deferred reply (UINT32_MAX) has nothing to settle.
        if (request_id == UINT32_MAX)
            return;
        TransferCompletion tc{};
        tc.kind = kind;
        tc.failed = true;
        tc.request_id = request_id;
        tc.resource_handle = slot_index;
        tc.resource_gen = resource_gen;
        tc.logical_base_mip = logical_base_mip;
        (void)publishResult(std::move(tc));
    }

    void GpuTransferPipeline::notifyLifecycle(
        std::uint32_t request_id,
        TransferCompletion::Kind kind,
        std::uint32_t resource_handle,
        std::uint32_t resource_gen,
        EUploadLifecycleState state
    ) noexcept
    {
        if (request_id != UINT32_MAX && lifecycle_ != nullptr)
            lifecycle_(lifecycle_state_, request_id, kind, resource_handle, resource_gen, state);
    }

    bool GpuTransferPipeline::needsQueueFamilyOwnershipTransfer() const noexcept
    {
        return needs_ownership_transfer_;
    }

    bool GpuTransferPipeline::publishResult(GpuTransferResult result)
    {
        while (results_.tryPush(std::move(result)) != lux::cxx::EQueuePushResult::ACCEPTED)
        {
            if (stop_requested_.load(std::memory_order_acquire))
            {
                if (auto* batch = std::get_if<RecordedBatch>(&result))
                {
                    freeUnsubmittedCompletion(batch->completion);
                    releaseBatchSlot(batch->slot);
                }
                else
                {
                    auto& completion = std::get<TransferCompletion>(result);
                    freeUnsubmittedCompletion(completion);
                    if (completion.retained_batch_slot != UINT32_MAX)
                    {
                        releaseAfterGraphicsAcquire(completion.retained_batch_slot);
                    }
                }
                return false;
            }
            const auto epoch = result_space_epoch_.load(std::memory_order_acquire);
            result_space_epoch_.wait(epoch, std::memory_order_relaxed);
        }
        worker_epoch_.fetch_add(1, std::memory_order_release);
        worker_epoch_.notify_one();
        if (notify_work_ != nullptr)
            notify_work_(notify_work_state_);
        return true;
    }

    void GpuTransferPipeline::publishRecorded(RecordedBatch batch)
    {
        batch.completion.gpu_copy_recorded = true;
        auto& slot = cmd_pools_[batch.slot.index];
        if (mode_ == EGpuTransferMode::RECORD_ONLY)
        {
            slot.state.store(EBatchSlotState::RECORDED, std::memory_order_release);
            slot.state.notify_one();
            (void)publishResult(std::move(batch));
            return;
        }

        const uint64_t timeline_value = ++timeline_counter_;
        VkSemaphoreSubmitInfo wait{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
        wait.semaphore = timeline_sem_;
        wait.value = timeline_value - 1u;
        wait.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

        VkSemaphoreSubmitInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
        signal.semaphore = timeline_sem_;
        signal.value = timeline_value;
        // Include the terminal queue-family release barrier in the signal's
        // synchronization scope.  The graphics finalize submission waits on
        // this value before recording the matching acquire operation.
        signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

        VkCommandBufferSubmitInfo command{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
        command.commandBuffer = batch.cmd;

        VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
        if (timeline_value > 1u)
        {
            submit.waitSemaphoreInfoCount = 1;
            submit.pWaitSemaphoreInfos = &wait;
        }
        submit.commandBufferInfoCount = 1;
        submit.pCommandBufferInfos = &command;
        submit.signalSemaphoreInfoCount = 1;
        submit.pSignalSemaphoreInfos = &signal;

        VkResult submit_result{VK_ERROR_UNKNOWN};
        {
            const std::scoped_lock queue_lock(*transfer_queue_mutex_);
            submit_result = vkQueueSubmit2(transfer_queue_, 1, &submit, VK_NULL_HANDLE);
        }
        if (submit_result != VK_SUCCESS)
        {
            const auto kind = batch.completion.kind;
            const auto request_id = batch.completion.request_id;
            const auto resource_handle = batch.completion.resource_handle;
            const auto resource_gen = batch.completion.resource_gen;
            freeUnsubmittedCompletion(batch.completion);
            releaseBatchSlot(batch.slot);

            TransferCompletion failure{};
            failure.kind = kind;
            failure.failed = true;
            failure.request_id = request_id;
            failure.resource_handle = resource_handle;
            failure.resource_gen = resource_gen;
            (void)publishResult(std::move(failure));
            return;
        }

        VkSemaphoreWaitInfo wait_info{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
        wait_info.semaphoreCount = 1;
        wait_info.pSemaphores = &timeline_sem_;
        wait_info.pValues = &timeline_value;
        if (vkWaitSemaphores(device_, &wait_info, UINT64_MAX) != VK_SUCCESS)
            renderFatal("GpuTransferPipeline dedicated-queue wait failed");

        batch.completion.timeline_value = 0;
        if (batch.completion.requires_queue_family_ownership_transfer)
        {
            // Keep exclusive-resource release command buffers alive until the
            // render owner has queued their matching graphics acquire.
            batch.completion.retained_batch_slot = batch.slot.index;
            slot.state.store(EBatchSlotState::RECORDED, std::memory_order_release);
            slot.state.notify_one();
        }
        else
        {
            releaseBatchSlot(batch.slot);
        }
        (void)publishResult(std::move(batch.completion));
    }

    void GpuTransferPipeline::workerLoop(TransferStopToken stop_token)
    {
        worker_running_.store(true, std::memory_order_release);
        for (;;)
        {
            UploadJob job{};
            if (jobs_.tryPop(job) == lux::cxx::EQueuePopResult::VALUE)
            {
                std::visit(
                    [this, stop_token](auto&& task) {
                        using Task = std::remove_cvref_t<decltype(task)>;
                        if constexpr (std::is_same_v<Task, MeshTransferTask>)
                            processMeshTransfer(std::move(task), stop_token);
                        else if constexpr (std::is_same_v<Task, TextureTransferTask>)
                            processTextureTransfer(std::move(task), stop_token);
                        else
                            processCubeTransfer(std::move(task), stop_token);
                    },
                    std::move(job)
                );
                continue;
            }

            if (stop_token.stopRequested() || !accepting_.load(std::memory_order_acquire))
                break;

            if (retireOneSubmittedSlot())
                continue;
            if (retireGraphicsFinalize())
                continue;

            const auto epoch = job_epoch_.load(std::memory_order_acquire);
            if (!jobs_.empty())
                continue;
            job_epoch_.wait(epoch, std::memory_order_relaxed);
        }
        worker_running_.store(false, std::memory_order_release);
        worker_epoch_.fetch_add(1, std::memory_order_release);
        worker_epoch_.notify_all();
    }

    bool GpuTransferPipeline::submitMeshTransfer(MeshTransferTask task)
    {
        const auto request_id = task.request_id;
        const auto resource_handle = task.mesh_index;
        const auto resource_gen = task.resource_gen;
        notifyLifecycle(
            request_id,
            TransferCompletion::Kind::MeshBuffer,
            resource_handle,
            resource_gen,
            EUploadLifecycleState::Accepted
        );
        notifyLifecycle(
            request_id,
            TransferCompletion::Kind::MeshBuffer,
            resource_handle,
            resource_gen,
            EUploadLifecycleState::ValidatedAndReserved
        );
        if (!accepting_.load(std::memory_order_acquire) ||
            jobs_.tryPush(UploadJob{std::move(task)}) != lux::cxx::EQueuePushResult::ACCEPTED)
        {
            notifyLifecycle(
                request_id,
                TransferCompletion::Kind::MeshBuffer,
                resource_handle,
                resource_gen,
                EUploadLifecycleState::Failed
            );
            return false;
        }
        notifyLifecycle(
            request_id,
            TransferCompletion::Kind::MeshBuffer,
            resource_handle,
            resource_gen,
            EUploadLifecycleState::TransferQueued
        );
        job_epoch_.fetch_add(1, std::memory_order_release);
        job_epoch_.notify_one();
        return true;
    }

    bool GpuTransferPipeline::submitTextureTransfer(TextureTransferTask task)
    {
        const auto request_id = task.request_id;
        const auto resource_handle = task.slot_index;
        const auto resource_gen = task.resource_gen;
        const auto kind =
            task.replacement ? TransferCompletion::Kind::Texture2DReplacement : TransferCompletion::Kind::Texture2D;
        notifyLifecycle(request_id, kind, resource_handle, resource_gen, EUploadLifecycleState::Accepted);
        notifyLifecycle(request_id, kind, resource_handle, resource_gen, EUploadLifecycleState::ValidatedAndReserved);
        if (!accepting_.load(std::memory_order_acquire) ||
            jobs_.tryPush(UploadJob{std::move(task)}) != lux::cxx::EQueuePushResult::ACCEPTED)
        {
            notifyLifecycle(request_id, kind, resource_handle, resource_gen, EUploadLifecycleState::Failed);
            return false;
        }
        notifyLifecycle(request_id, kind, resource_handle, resource_gen, EUploadLifecycleState::TransferQueued);
        job_epoch_.fetch_add(1, std::memory_order_release);
        job_epoch_.notify_one();
        return true;
    }

    bool GpuTransferPipeline::submitCubeTransfer(CubeTransferTask task)
    {
        const auto request_id = task.request_id;
        const auto resource_handle = task.slot_index;
        const auto resource_gen = task.resource_gen;
        notifyLifecycle(
            request_id,
            TransferCompletion::Kind::TextureCube,
            resource_handle,
            resource_gen,
            EUploadLifecycleState::Accepted
        );
        notifyLifecycle(
            request_id,
            TransferCompletion::Kind::TextureCube,
            resource_handle,
            resource_gen,
            EUploadLifecycleState::ValidatedAndReserved
        );
        if (!accepting_.load(std::memory_order_acquire) ||
            jobs_.tryPush(UploadJob{std::move(task)}) != lux::cxx::EQueuePushResult::ACCEPTED)
        {
            notifyLifecycle(
                request_id,
                TransferCompletion::Kind::TextureCube,
                resource_handle,
                resource_gen,
                EUploadLifecycleState::Failed
            );
            return false;
        }
        notifyLifecycle(
            request_id,
            TransferCompletion::Kind::TextureCube,
            resource_handle,
            resource_gen,
            EUploadLifecycleState::TransferQueued
        );
        job_epoch_.fetch_add(1, std::memory_order_release);
        job_epoch_.notify_one();
        return true;
    }

    // =========================================================================
    //  Mesh transfer
    // =========================================================================

    void GpuTransferPipeline::processMeshTransfer(MeshTransferTask task, TransferStopToken st)
    {
        if (can_record_transfer_)
        {
            // Every bail-out MUST settle the client request. Meshes
            // carry no bindless slot to reclaim (their reserved resource is the
            // mesh-arena range), so the render thread only sends the status!=0
            // reply here; arena reclamation stays with the mesh resource.
            const auto fail = [&] {
                pushFailure(TransferCompletion::Kind::MeshBuffer, task.request_id, task.mesh_index, task.resource_gen);
            };
            if (st.stopRequested())
            {
                fail();
                return;
            }

            // 1. Staging alloc + memcpy
            const VkDeviceSize total = task.vbo_bytes + task.ibo_bytes;
            auto stg = allocStagingBuffer(total);
            if (!stg.mapped)
            {
                fail();
                return;
            }

            std::memcpy(stg.mapped, task.vbo_data, task.vbo_bytes);
            if (task.ibo_bytes > 0)
                std::memcpy(static_cast<std::byte*>(stg.mapped) + task.vbo_bytes, task.ibo_data, task.ibo_bytes);
            staging_copied_bytes_.fetch_add(static_cast<std::uint64_t>(total), std::memory_order_relaxed);

            // 2. Record transfer commands
            auto batch_slot = acquireBatchSlot();
            // A failed command-buffer alloc/begin/end leaves an unusable (or garbage)
            // handle — recording into it is UB. Free the staging, release the
            // batch slot, and settle the request as failed; no future waiter
            // may depend on a slot that never reaches SUBMITTED.
            const auto fail_cmd = [&] {
                vmaDestroyBuffer(vma_, stg.buf, stg.alloc);
                releaseBatchSlot(batch_slot);
                fail();
            };
            VkCommandBufferAllocateInfo cb_ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            cb_ai.commandPool = batch_slot.pool;
            cb_ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            cb_ai.commandBufferCount = 1;
            VkCommandBuffer tcb;
            if (vkAllocateCommandBuffers(device_, &cb_ai, &tcb) != VK_SUCCESS)
            {
                fail_cmd();
                return;
            }

            VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            if (vkBeginCommandBuffer(tcb, &begin) != VK_SUCCESS)
            {
                fail_cmd();
                return;
            }

            // VBO copy
            VkBufferCopy vbo_region{};
            vbo_region.srcOffset = 0;
            vbo_region.dstOffset = task.vbo_offset;
            vbo_region.size = task.vbo_bytes;
            vkCmdCopyBuffer(tcb, stg.buf, task.vbo_buf, 1, &vbo_region);

            // IBO copy
            if (task.ibo_bytes > 0)
            {
                VkBufferCopy ibo_region{};
                ibo_region.srcOffset = task.vbo_bytes;
                ibo_region.dstOffset = task.ibo_offset;
                ibo_region.size = task.ibo_bytes;
                vkCmdCopyBuffer(tcb, stg.buf, task.ibo_buf, 1, &ibo_region);
            }

            // 4. Finish recording — the render thread submits it (no worker submit).
            if (vkEndCommandBuffer(tcb) != VK_SUCCESS)
            {
                fail_cmd();
                return;
            }

            // 5. Hand off the recorded CB + completion (timeline_value assigned
            //    by flushPendingSubmits at submit time).
            TransferCompletion tc{};
            tc.kind = TransferCompletion::Kind::MeshBuffer;
            tc.requires_queue_family_ownership_transfer = false;
            tc.request_id = task.request_id;
            tc.resource_handle = task.mesh_index;
            tc.resource_gen = task.resource_gen;
            tc.stg_buf = stg.buf;
            tc.stg_alloc = stg.alloc;
            tc.mesh.vbo_buf = task.vbo_buf;
            tc.mesh.vbo_offset = task.vbo_offset;
            tc.mesh.vbo_size = task.vbo_bytes;
            tc.mesh.ibo_buf = task.ibo_buf;
            tc.mesh.ibo_offset = task.ibo_offset;
            tc.mesh.ibo_size = task.ibo_bytes;
            tc.mesh.mesh_index = task.mesh_index;

            RecordedBatch ps{};
            ps.cmd = tcb;
            ps.slot = batch_slot;
            ps.completion = std::move(tc);
            publishRecorded(std::move(ps));
        }
        else
        {
            const auto fail = [&] {
                pushFailure(TransferCompletion::Kind::MeshBuffer, task.request_id, task.mesh_index, task.resource_gen);
            };
            if (st.stopRequested())
            {
                fail();
                return;
            }

            const VkDeviceSize total = task.vbo_bytes + task.ibo_bytes;
            auto stg = allocStagingBuffer(total);
            if (!stg.mapped)
            {
                fail();
                return;
            }

            std::memcpy(stg.mapped, task.vbo_data, task.vbo_bytes);
            if (task.ibo_bytes > 0)
            {
                std::memcpy(static_cast<std::byte*>(stg.mapped) + task.vbo_bytes, task.ibo_data, task.ibo_bytes);
            }
            staging_copied_bytes_.fetch_add(static_cast<std::uint64_t>(total), std::memory_order_relaxed);

            TransferCompletion tc{};
            tc.kind = TransferCompletion::Kind::MeshBuffer;
            tc.requires_queue_family_ownership_transfer = false;
            tc.timeline_value = 0; // StagingOnly: render thread records copy
            tc.request_id = task.request_id;
            tc.resource_handle = task.mesh_index;
            tc.resource_gen = task.resource_gen;
            tc.stg_buf = stg.buf;
            tc.stg_alloc = stg.alloc;
            tc.mesh.vbo_buf = task.vbo_buf;
            tc.mesh.vbo_offset = task.vbo_offset;
            tc.mesh.vbo_size = task.vbo_bytes;
            tc.mesh.ibo_buf = task.ibo_buf;
            tc.mesh.ibo_offset = task.ibo_offset;
            tc.mesh.ibo_size = task.ibo_bytes;
            tc.mesh.mesh_index = task.mesh_index;
            // On a closed ring (shutdown) push() returns false without moving tc —
            // free its GPU objects here instead of leaking them.
            (void)publishResult(std::move(tc));
        }
    }

    // =========================================================================
    //  Texture transfer
    // =========================================================================

    void GpuTransferPipeline::processTextureTransfer(TextureTransferTask task, TransferStopToken st)
    {
        const auto completion_kind =
            task.replacement ? TransferCompletion::Kind::Texture2DReplacement : TransferCompletion::Kind::Texture2D;
        // Every bail-out below MUST settle the client request (else it hangs
        // forever). Creation releases its reserved slot; replacement failure
        // leaves the existing slot and its current image untouched.
        const auto fail = [&] {
            pushFailure(completion_kind, task.request_id, task.slot_index, task.resource_gen, task.logical_base_mip);
        };

        if (st.stopRequested())
        {
            fail();
            return;
        }

        // ── Consume the server's validated Texture2DUploadPlan ──
        // The create handler already ran validateTexture2DUpload() and threaded the
        // authoritative per-mip {buffer_offset, bytes, extent} + total_bytes into the
        // task. The worker MUST NOT re-derive that layout — a second algorithm that
        // could drift from the validator and silently OOB the buffer→image copy. It
        // copies the plan's offsets/sizes verbatim. toVkFormat still runs (the worker
        // needs the VkFormat to create the image; it also rejects unknown formats).
        // Only a null source pointer is re-checked — a cheap guard against a
        // mis-populated task; the plan already guarantees non-null, exact-size data.
        const uint32_t provided_mips = std::clamp<uint32_t>(task.mip_count, 1u, kTextureTransferMaxMipCount);

        for (uint32_t i = 0; i < provided_mips; ++i)
            if (task.mips[i].data == nullptr)
            {
                fail();
                return;
            }

        const int32_t base_width = task.mips[0].width;
        const int32_t base_height = task.mips[0].height;

        const auto vk_fmt_opt = toVkFormat(task.format);
        if (!vk_fmt_opt)
        {
            fail();
            return;
        }
        const VkFormat vk_fmt = *vk_fmt_opt;
        const bool runtime_mips = task.gen_mips && !isCompressedPixelFormat(task.format) && provided_mips == 1;
        const uint32_t mip_levels =
            runtime_mips ? static_cast<uint32_t>(std::floor(std::log2(std::max(base_width, base_height)))) + 1
                         : provided_mips;

        std::array<TextureTransferMipCopy, kTextureTransferMaxMipCount> uploaded_mips{};
        for (uint32_t i = 0; i < provided_mips; ++i)
        {
            uploaded_mips[i].buffer_offset = task.mips[i].buffer_offset; // from validated plan
            uploaded_mips[i].byte_size = static_cast<VkDeviceSize>(task.mips[i].bytes);
            uploaded_mips[i].mip_level = i;
            uploaded_mips[i].width = static_cast<uint32_t>(task.mips[i].width);
            uploaded_mips[i].height = static_cast<uint32_t>(task.mips[i].height);
        }
        const VkDeviceSize total_bytes = task.total_bytes; // from validated plan

        // 1. Create GPU image
        VkImageCreateInfo img_ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        img_ci.imageType = VK_IMAGE_TYPE_2D;
        img_ci.format = vk_fmt;
        img_ci.extent = {static_cast<uint32_t>(base_width), static_cast<uint32_t>(base_height), 1};
        img_ci.mipLevels = mip_levels;
        img_ci.arrayLayers = 1;
        img_ci.samples = VK_SAMPLE_COUNT_1_BIT;
        img_ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        img_ci.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        img_ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        img_ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        VkImage image;
        VmaAllocation image_alloc;
        VmaAllocationCreateInfo img_aci{};
        img_aci.usage = VMA_MEMORY_USAGE_GPU_ONLY;
        if (vmaCreateImage(vma_, &img_ci, &img_aci, &image, &image_alloc, nullptr) != VK_SUCCESS)
        {
            fail();
            return;
        }

        // 2. Image view
        VkImageViewCreateInfo view_ci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view_ci.image = image;
        view_ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_ci.format = vk_fmt;
        view_ci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mip_levels, 0, 1};
        VkImageView view;
        if (vkCreateImageView(device_, &view_ci, nullptr, &view) != VK_SUCCESS)
        {
            vmaDestroyImage(vma_, image, image_alloc);
            fail();
            return;
        }

        // 3. Sampler
        VkSamplerCreateInfo samp_ci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        samp_ci.magFilter = VK_FILTER_LINEAR;
        samp_ci.minFilter = VK_FILTER_LINEAR;
        samp_ci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        samp_ci.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samp_ci.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samp_ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samp_ci.maxLod = static_cast<float>(mip_levels);
        VkSampler sampler;
        if (vkCreateSampler(device_, &samp_ci, nullptr, &sampler) != VK_SUCCESS)
        {
            vkDestroyImageView(device_, view, nullptr);
            vmaDestroyImage(vma_, image, image_alloc);
            fail();
            return;
        }

        // 4. Staging buffer + memcpy
        auto stg = allocStagingBuffer(total_bytes);
        if (!stg.mapped)
        {
            vkDestroySampler(device_, sampler, nullptr);
            vkDestroyImageView(device_, view, nullptr);
            vmaDestroyImage(vma_, image, image_alloc);
            fail();
            return;
        }
        for (uint32_t i = 0; i < provided_mips; ++i)
        {
            std::memcpy(
                static_cast<std::byte*>(stg.mapped) + uploaded_mips[i].buffer_offset,
                task.mips[i].data,
                task.mips[i].bytes
            );
        }
        staging_copied_bytes_.fetch_add(static_cast<std::uint64_t>(total_bytes), std::memory_order_relaxed);

        if (can_record_transfer_)
        {
            // 5. Record commands
            auto batch_slot = acquireBatchSlot();
            // A failed command-buffer alloc/begin/end leaves an unusable (or garbage)
            // handle — recording into it is UB. Free the worker's GPU objects + staging,
            // release the batch slot (its gate is a prior, already-reached value, so no
            // future waiter hangs), and settle the request as failed.
            const auto fail_cmd = [&] {
                vkDestroySampler(device_, sampler, nullptr);
                vkDestroyImageView(device_, view, nullptr);
                vmaDestroyImage(vma_, image, image_alloc);
                vmaDestroyBuffer(vma_, stg.buf, stg.alloc);
                releaseBatchSlot(batch_slot);
                fail();
            };
            VkCommandBufferAllocateInfo cb_ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            cb_ai.commandPool = batch_slot.pool;
            cb_ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            cb_ai.commandBufferCount = 1;
            VkCommandBuffer tcb;
            if (vkAllocateCommandBuffers(device_, &cb_ai, &tcb) != VK_SUCCESS)
            {
                fail_cmd();
                return;
            }

            VkCommandBufferBeginInfo begin_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            if (vkBeginCommandBuffer(tcb, &begin_info) != VK_SUCCESS)
            {
                fail_cmd();
                return;
            }

            // UNDEFINED → TRANSFER_DST_OPTIMAL
            VkImageMemoryBarrier2 to_dst{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
            to_dst.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
            to_dst.srcAccessMask = VK_ACCESS_2_NONE;
            to_dst.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
            to_dst.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            to_dst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            to_dst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            to_dst.image = image;
            to_dst.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mip_levels, 0, 1};
            {
                VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
                dep.imageMemoryBarrierCount = 1;
                dep.pImageMemoryBarriers = &to_dst;
                vkCmdPipelineBarrier2(tcb, &dep);
            }

            // Copy staged mip chain
            for (uint32_t i = 0; i < provided_mips; ++i)
            {
                VkBufferImageCopy2 copy{VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2};
                copy.bufferOffset = uploaded_mips[i].buffer_offset;
                copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, uploaded_mips[i].mip_level, 0, 1};
                copy.imageExtent = {uploaded_mips[i].width, uploaded_mips[i].height, 1};
                VkCopyBufferToImageInfo2 copy_info{VK_STRUCTURE_TYPE_COPY_BUFFER_TO_IMAGE_INFO_2};
                copy_info.srcBuffer = stg.buf;
                copy_info.dstImage = image;
                copy_info.dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                copy_info.regionCount = 1;
                copy_info.pRegions = &copy;
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
                        src_bar.srcStageMask =
                            (lvl == 1u) ? VK_PIPELINE_STAGE_2_COPY_BIT : VK_PIPELINE_STAGE_2_BLIT_BIT;
                        src_bar.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
                        src_bar.dstStageMask = VK_PIPELINE_STAGE_2_BLIT_BIT;
                        src_bar.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
                        src_bar.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                        src_bar.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                        src_bar.image = image;
                        src_bar.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, lvl - 1, 1, 0, 1};
                        VkDependencyInfo d1{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
                        d1.imageMemoryBarrierCount = 1;
                        d1.pImageMemoryBarriers = &src_bar;
                        vkCmdPipelineBarrier2(tcb, &d1);

                        int32_t nw = std::max(1, w / 2);
                        int32_t nh = std::max(1, h / 2);

                        VkImageBlit2 blit{VK_STRUCTURE_TYPE_IMAGE_BLIT_2};
                        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, lvl - 1, 0, 1};
                        blit.srcOffsets[0] = {0, 0, 0};
                        blit.srcOffsets[1] = {w, h, 1};
                        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, lvl, 0, 1};
                        blit.dstOffsets[0] = {0, 0, 0};
                        blit.dstOffsets[1] = {nw, nh, 1};

                        VkBlitImageInfo2 blit_info{VK_STRUCTURE_TYPE_BLIT_IMAGE_INFO_2};
                        blit_info.srcImage = image;
                        blit_info.srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                        blit_info.dstImage = image;
                        blit_info.dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                        blit_info.regionCount = 1;
                        blit_info.pRegions = &blit;
                        blit_info.filter = VK_FILTER_LINEAR;
                        vkCmdBlitImage2(tcb, &blit_info);

                        w = nw;
                        h = nh;
                    }

                    // Transition all mip levels to SHADER_READ_ONLY
                    // levels [0..mip_levels-2] are TRANSFER_SRC, last is TRANSFER_DST
                    if (mip_levels > 1)
                    {
                        VkImageMemoryBarrier2 final_bar{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
                        final_bar.srcStageMask = VK_PIPELINE_STAGE_2_BLIT_BIT;
                        final_bar.srcAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
                        final_bar.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
                        final_bar.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
                        final_bar.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                        final_bar.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                        final_bar.image = image;
                        final_bar.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mip_levels - 1, 0, 1};

                        VkImageMemoryBarrier2 last_bar{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
                        last_bar.srcStageMask = VK_PIPELINE_STAGE_2_BLIT_BIT;
                        last_bar.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
                        last_bar.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
                        last_bar.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
                        last_bar.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                        last_bar.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                        last_bar.image = image;
                        last_bar.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, mip_levels - 1, 1, 0, 1};

                        std::array bars = {final_bar, last_bar};
                        VkDependencyInfo d2{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
                        d2.imageMemoryBarrierCount = static_cast<uint32_t>(bars.size());
                        d2.pImageMemoryBarriers = bars.data();
                        vkCmdPipelineBarrier2(tcb, &d2);
                    }
                }
                else
                {
                    needs_mip_gen = true; // Render thread generates mips after acquire
                }
            }
            else if (!runtime_mips || mip_levels == 1)
            {
                // No deferred mip generation: move image to SHADER_READ before any
                // optional queue-family ownership release.
                VkImageMemoryBarrier2 to_read{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
                to_read.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
                to_read.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
                to_read.dstStageMask =
                    needs_ownership_transfer_ ? VK_PIPELINE_STAGE_2_NONE : VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
                to_read.dstAccessMask =
                    needs_ownership_transfer_ ? VK_ACCESS_2_NONE : VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
                to_read.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                to_read.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                to_read.image = image;
                to_read.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mip_levels, 0, 1};
                VkDependencyInfo d3{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
                d3.imageMemoryBarrierCount = 1;
                d3.pImageMemoryBarriers = &to_read;
                vkCmdPipelineBarrier2(tcb, &d3);
            }

            // Queue Ownership Transfer: release barrier
            if (needs_ownership_transfer_)
            {
                const VkImageLayout layout =
                    needs_mip_gen ? VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

                VkImageMemoryBarrier2 release{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
                release.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
                release.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
                release.dstStageMask = VK_PIPELINE_STAGE_2_NONE;
                release.dstAccessMask = VK_ACCESS_2_NONE;
                release.oldLayout = layout;
                release.newLayout = layout;
                release.srcQueueFamilyIndex = transfer_family_;
                release.dstQueueFamilyIndex = graphics_family_;
                release.image = image;
                release.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mip_levels, 0, 1};

                VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
                dep.imageMemoryBarrierCount = 1;
                dep.pImageMemoryBarriers = &release;
                vkCmdPipelineBarrier2(tcb, &dep);
            }

            if (vkEndCommandBuffer(tcb) != VK_SUCCESS) // render thread submits it (no worker submit)
            {
                fail_cmd();
                return;
            }

            // Hand off recorded CB + completion (timeline_value set at submit).
            TransferCompletion tc{};
            tc.kind = completion_kind;
            tc.request_id = task.request_id;
            tc.resource_handle = task.slot_index;
            tc.resource_gen = task.resource_gen;
            tc.logical_base_mip = task.logical_base_mip;
            tc.stg_buf = stg.buf;
            tc.stg_alloc = stg.alloc;
            tc.stg_size = total_bytes;
            tc.texture.image = image;
            tc.texture.image_alloc = image_alloc;
            tc.texture.view = view;
            tc.texture.sampler = sampler;
            tc.texture.format = vk_fmt;
            tc.texture.mip_levels = mip_levels;
            tc.texture.array_layers = 1;
            tc.texture.width = base_width;
            tc.texture.height = base_height;
            tc.texture.slot_index = task.slot_index;
            tc.texture.needs_mip_gen = needs_mip_gen;
            tc.texture.uploaded_mip_count = provided_mips;
            tc.texture.uploaded_mips = uploaded_mips;

            RecordedBatch ps{};
            ps.cmd = tcb;
            ps.slot = batch_slot;
            ps.completion = std::move(tc);
            publishRecorded(std::move(ps));
        }
        else // StagingOnly
        {
            // Not recording commands — render thread will handle image creation + copy
            TransferCompletion tc{};
            tc.kind = completion_kind;
            tc.timeline_value = 0;
            tc.request_id = task.request_id;
            tc.resource_handle = task.slot_index;
            tc.resource_gen = task.resource_gen;
            tc.logical_base_mip = task.logical_base_mip;
            tc.stg_buf = stg.buf;
            tc.stg_alloc = stg.alloc;
            tc.stg_size = total_bytes;
            tc.texture.image = image;
            tc.texture.image_alloc = image_alloc;
            tc.texture.view = view;
            tc.texture.sampler = sampler;
            tc.texture.format = vk_fmt;
            tc.texture.mip_levels = mip_levels;
            tc.texture.array_layers = 1;
            tc.texture.width = base_width;
            tc.texture.height = base_height;
            tc.texture.slot_index = task.slot_index;
            tc.texture.needs_mip_gen = (runtime_mips && mip_levels > 1);
            tc.texture.uploaded_mip_count = provided_mips;
            tc.texture.uploaded_mips = uploaded_mips;
            // On a closed ring (shutdown) push() returns false without moving tc —
            // free its GPU objects here instead of leaking them.
            (void)publishResult(std::move(tc));
        }
    }

    // =========================================================================
    //  Cube texture transfer
    // =========================================================================

    void GpuTransferPipeline::processCubeTransfer(CubeTransferTask task, TransferStopToken st)
    {
        constexpr uint32_t kFaceCount = 6;

        // Every bail-out below MUST settle the client request (else it hangs
        // forever) and release the reserved bindless slot.
        const auto fail = [&] {
            pushFailure(TransferCompletion::Kind::TextureCube, task.request_id, task.slot_index, task.resource_gen);
        };

        if (st.stopRequested())
        {
            fail();
            return;
        }

        // ── Consume the server's validated CubeUploadPlan ───────
        // The create handler ran validateCubeUpload() and threaded the authoritative
        // per-face byte size into task.face_bytes. The worker uses it verbatim as the
        // staging stride instead of recomputing pixelFormatMipBytes() — one source of
        // truth, no second algorithm to drift and OOB the per-face copy. toVkFormat
        // still runs (needed to create the image; also rejects unknown formats). Only
        // null face pointers are re-checked (the plan guarantees non-null faces).
        for (uint32_t f = 0; f < kFaceCount; ++f)
            if (task.faces[f].data == nullptr)
            {
                fail();
                return;
            }

        const VkDeviceSize face_stride = task.face_bytes;

        const auto vk_fmt_opt = toVkFormat(task.format);
        if (!vk_fmt_opt)
        {
            fail();
            return;
        }
        const VkFormat vk_fmt = *vk_fmt_opt;

        // Total staging bytes = 6 equal, format-exact faces (from the validated plan).
        const VkDeviceSize total_bytes = face_stride * kFaceCount;

        // 1. Create GPU cube image
        VkImageCreateInfo img_ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        img_ci.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
        img_ci.imageType = VK_IMAGE_TYPE_2D;
        img_ci.format = vk_fmt;
        img_ci.extent = {static_cast<uint32_t>(task.face_size), static_cast<uint32_t>(task.face_size), 1};
        img_ci.mipLevels = 1;
        img_ci.arrayLayers = kFaceCount;
        img_ci.samples = VK_SAMPLE_COUNT_1_BIT;
        img_ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        img_ci.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        img_ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        img_ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        VkImage image;
        VmaAllocation image_alloc;
        VmaAllocationCreateInfo img_aci{};
        img_aci.usage = VMA_MEMORY_USAGE_GPU_ONLY;
        if (vmaCreateImage(vma_, &img_ci, &img_aci, &image, &image_alloc, nullptr) != VK_SUCCESS)
        {
            fail();
            return;
        }

        // 2. Image view
        VkImageViewCreateInfo view_ci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view_ci.image = image;
        view_ci.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
        view_ci.format = vk_fmt;
        view_ci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, kFaceCount};
        VkImageView view;
        if (vkCreateImageView(device_, &view_ci, nullptr, &view) != VK_SUCCESS)
        {
            vmaDestroyImage(vma_, image, image_alloc);
            fail();
            return;
        }

        // 3. Sampler
        VkSamplerCreateInfo samp_ci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        samp_ci.magFilter = VK_FILTER_LINEAR;
        samp_ci.minFilter = VK_FILTER_LINEAR;
        samp_ci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        samp_ci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samp_ci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samp_ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samp_ci.maxLod = 1.0f;
        VkSampler sampler;
        if (vkCreateSampler(device_, &samp_ci, nullptr, &sampler) != VK_SUCCESS)
        {
            vkDestroyImageView(device_, view, nullptr);
            vmaDestroyImage(vma_, image, image_alloc);
            fail();
            return;
        }

        // 4. Staging buffer
        auto stg = allocStagingBuffer(total_bytes);
        if (!stg.mapped)
        {
            vkDestroySampler(device_, sampler, nullptr);
            vkDestroyImageView(device_, view, nullptr);
            vmaDestroyImage(vma_, image, image_alloc);
            fail();
            return;
        }

        // Pack all 6 faces into staging buffer (uniform stride from the validated plan)
        VkDeviceSize offset = 0;
        for (uint32_t f = 0; f < kFaceCount; ++f)
        {
            std::memcpy(
                static_cast<std::byte*>(stg.mapped) + offset,
                task.faces[f].data,
                static_cast<std::size_t>(face_stride)
            );
            offset += face_stride;
        }
        staging_copied_bytes_.fetch_add(static_cast<std::uint64_t>(total_bytes), std::memory_order_relaxed);
        if (can_record_transfer_)
        {
            auto batch_slot = acquireBatchSlot();
            // A failed command-buffer alloc/begin/end leaves an unusable (or garbage)
            // handle — recording into it is UB. Free the worker's GPU objects + staging,
            // release the batch slot (its gate is a prior, already-reached value, so no
            // future waiter hangs), and settle the request as failed.
            const auto fail_cmd = [&] {
                vkDestroySampler(device_, sampler, nullptr);
                vkDestroyImageView(device_, view, nullptr);
                vmaDestroyImage(vma_, image, image_alloc);
                vmaDestroyBuffer(vma_, stg.buf, stg.alloc);
                releaseBatchSlot(batch_slot);
                fail();
            };
            VkCommandBufferAllocateInfo cb_ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            cb_ai.commandPool = batch_slot.pool;
            cb_ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            cb_ai.commandBufferCount = 1;
            VkCommandBuffer tcb;
            if (vkAllocateCommandBuffers(device_, &cb_ai, &tcb) != VK_SUCCESS)
            {
                fail_cmd();
                return;
            }

            VkCommandBufferBeginInfo begin_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            if (vkBeginCommandBuffer(tcb, &begin_info) != VK_SUCCESS)
            {
                fail_cmd();
                return;
            }

            // UNDEFINED → TRANSFER_DST
            VkImageMemoryBarrier2 to_dst{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
            to_dst.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
            to_dst.srcAccessMask = VK_ACCESS_2_NONE;
            to_dst.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
            to_dst.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            to_dst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            to_dst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            to_dst.image = image;
            to_dst.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, kFaceCount};
            {
                VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
                dep.imageMemoryBarrierCount = 1;
                dep.pImageMemoryBarriers = &to_dst;
                vkCmdPipelineBarrier2(tcb, &dep);
            }

            // Copy each face from staging
            VkDeviceSize buf_offset = 0;
            for (uint32_t f = 0; f < kFaceCount; ++f)
            {
                VkBufferImageCopy2 region{VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2};
                region.bufferOffset = buf_offset;
                region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, f, 1};
                region.imageExtent = {static_cast<uint32_t>(task.face_size), static_cast<uint32_t>(task.face_size), 1};
                VkCopyBufferToImageInfo2 ci{VK_STRUCTURE_TYPE_COPY_BUFFER_TO_IMAGE_INFO_2};
                ci.srcBuffer = stg.buf;
                ci.dstImage = image;
                ci.dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                ci.regionCount = 1;
                ci.pRegions = &region;
                vkCmdCopyBufferToImage2(tcb, &ci);

                buf_offset += face_stride;
            }

            // Transition cube to SHADER_READ before any optional ownership release.
            VkImageMemoryBarrier2 to_read{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
            to_read.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
            to_read.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            to_read.dstStageMask =
                needs_ownership_transfer_ ? VK_PIPELINE_STAGE_2_NONE : VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
            to_read.dstAccessMask = needs_ownership_transfer_ ? VK_ACCESS_2_NONE : VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
            to_read.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            to_read.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            to_read.image = image;
            to_read.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, kFaceCount};

            {
                VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
                dep.imageMemoryBarrierCount = 1;
                dep.pImageMemoryBarriers = &to_read;
                vkCmdPipelineBarrier2(tcb, &dep);
            }

            if (needs_ownership_transfer_)
            {
                VkImageMemoryBarrier2 release{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
                release.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
                release.srcAccessMask = VK_ACCESS_2_NONE;
                release.dstStageMask = VK_PIPELINE_STAGE_2_NONE;
                release.dstAccessMask = VK_ACCESS_2_NONE;
                release.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                release.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                release.srcQueueFamilyIndex = transfer_family_;
                release.dstQueueFamilyIndex = graphics_family_;
                release.image = image;
                release.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, kFaceCount};

                VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
                dep.imageMemoryBarrierCount = 1;
                dep.pImageMemoryBarriers = &release;
                vkCmdPipelineBarrier2(tcb, &dep);
            }

            if (vkEndCommandBuffer(tcb) != VK_SUCCESS) // render thread submits it (no worker submit)
            {
                fail_cmd();
                return;
            }

            TransferCompletion tc{};
            tc.kind = TransferCompletion::Kind::TextureCube;
            tc.request_id = task.request_id;
            tc.resource_handle = task.slot_index;
            tc.resource_gen = task.resource_gen;
            tc.stg_buf = stg.buf;
            tc.stg_alloc = stg.alloc;
            tc.texture.image = image;
            tc.texture.image_alloc = image_alloc;
            tc.texture.view = view;
            tc.texture.sampler = sampler;
            tc.texture.format = vk_fmt;
            tc.texture.mip_levels = 1;
            tc.texture.array_layers = kFaceCount;
            tc.texture.width = task.face_size;
            tc.texture.height = task.face_size;
            tc.texture.slot_index = task.slot_index;
            tc.texture.needs_mip_gen = false;
            tc.texture.face_stride = face_stride; // validated per-face size
            tc.stg_size = total_bytes;

            RecordedBatch ps{};
            ps.cmd = tcb;
            ps.slot = batch_slot;
            ps.completion = std::move(tc);
            publishRecorded(std::move(ps));
        }
        else // StagingOnly
        {
            TransferCompletion tc{};
            tc.kind = TransferCompletion::Kind::TextureCube;
            tc.timeline_value = 0;
            tc.request_id = task.request_id;
            tc.resource_handle = task.slot_index;
            tc.resource_gen = task.resource_gen;
            tc.stg_buf = stg.buf;
            tc.stg_alloc = stg.alloc;
            tc.texture.image = image;
            tc.texture.image_alloc = image_alloc;
            tc.texture.view = view;
            tc.texture.sampler = sampler;
            tc.texture.format = vk_fmt;
            tc.texture.mip_levels = 1;
            tc.texture.array_layers = kFaceCount;
            tc.texture.width = task.face_size;
            tc.texture.height = task.face_size;
            tc.texture.slot_index = task.slot_index;
            tc.texture.needs_mip_gen = false;
            tc.texture.face_stride = face_stride; // validated per-face size
            tc.stg_size = total_bytes;
            // On a closed ring (shutdown) push() returns false without moving tc —
            // free its GPU objects here instead of leaking them.
            (void)publishResult(std::move(tc));
        }
    }

} // namespace lux::render
