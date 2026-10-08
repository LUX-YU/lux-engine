#define VMA_IMPLEMENTATION
#include <lux/engine/render/gpu/VmaFwd.hpp>
#include <vk_mem_alloc.h>

#include <cassert>
#include <cstdio>

namespace
{
    VkFence watched_fence{};
    VkCommandBuffer watched_command{};
    VkBuffer watched_buffer{};
    bool blocked{};
    unsigned early_release{};
    enum class EFault
    {
        NONE,
        BEGIN,
        END,
        FENCE,
        SUBMIT,
        INVALIDATE
    };
    EFault fault{};
    unsigned submissions{};
    VkSemaphore submit_gate{};
    uint64_t submit_gate_value{};

    VkResult beginCommand(VkCommandBuffer command, const VkCommandBufferBeginInfo* info)
    {
        return fault == EFault::BEGIN ? VK_ERROR_OUT_OF_DEVICE_MEMORY : vkBeginCommandBuffer(command, info);
    }

    VkResult endCommand(VkCommandBuffer command)
    {
        return fault == EFault::END ? VK_ERROR_OUT_OF_DEVICE_MEMORY : vkEndCommandBuffer(command);
    }

    VkResult createFence(
        VkDevice device,
        const VkFenceCreateInfo* info,
        const VkAllocationCallbacks* callbacks,
        VkFence* output
    )
    {
        if (fault == EFault::FENCE)
        {
            *output = VK_NULL_HANDLE;
            return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        }
        return vkCreateFence(device, info, callbacks, output);
    }

    VkResult queueSubmit(VkQueue queue, uint32_t count, const VkSubmitInfo* submits, VkFence fence)
    {
        ++submissions;
        if (fault == EFault::SUBMIT)
        {
            return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        }
        if (submit_gate)
        {
            assert(count == 1 && submits->waitSemaphoreCount == 0 && !submits->pNext);
            VkSubmitInfo gated = *submits;
            VkTimelineSemaphoreSubmitInfo timeline{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
            timeline.waitSemaphoreValueCount = 1;
            timeline.pWaitSemaphoreValues = &submit_gate_value;
            const VkPipelineStageFlags stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
            gated.pNext = &timeline;
            gated.waitSemaphoreCount = 1;
            gated.pWaitSemaphores = &submit_gate;
            gated.pWaitDstStageMask = &stage;
            return vkQueueSubmit(queue, 1, &gated, fence);
        }
        return vkQueueSubmit(queue, count, submits, fence);
    }

    VkResult invalidate(VmaAllocator allocator, VmaAllocation allocation, VkDeviceSize offset, VkDeviceSize size)
    {
        return fault == EFault::INVALIDATE ? VK_ERROR_MEMORY_MAP_FAILED
                                           : vmaInvalidateAllocation(allocator, allocation, offset, size);
    }

    void destroyFence(VkDevice device, VkFence fence, const VkAllocationCallbacks* callbacks)
    {
        if (blocked && fence == watched_fence)
        {
            ++early_release;
            return;
        }
        vkDestroyFence(device, fence, callbacks);
    }

    void freeCommands(VkDevice device, VkCommandPool pool, uint32_t count, const VkCommandBuffer* commands)
    {
        if (blocked && count == 1 && *commands == watched_command)
        {
            ++early_release;
            return;
        }
        vkFreeCommandBuffers(device, pool, count, commands);
    }

    void destroyBuffer(VmaAllocator allocator, VkBuffer buffer, VmaAllocation allocation)
    {
        if (blocked && buffer == watched_buffer)
        {
            ++early_release;
            return;
        }
        vmaDestroyBuffer(allocator, buffer, allocation);
    }
} // namespace

// Compile the complete production provider. Interposition only delays illegal native
// destruction, so the pre-fix failure can be observed without freeing live GPU objects.
#define vkBeginCommandBuffer beginCommand
#define vkEndCommandBuffer endCommand
#define vkCreateFence createFence
#define vkQueueSubmit queueSubmit
#define vmaInvalidateAllocation invalidate
#define vkDestroyFence destroyFence
#define vkFreeCommandBuffers freeCommands
#define vmaDestroyBuffer destroyBuffer
#include "../src/comm/RenderServerBootstrap.cpp"
#include "../src/gpu/memory/VmaTypes.cpp"
#undef vmaDestroyBuffer
#undef vkFreeCommandBuffers
#undef vkDestroyFence
#undef vkBeginCommandBuffer
#undef vkEndCommandBuffer
#undef vkCreateFence
#undef vkQueueSubmit
#undef vmaInvalidateAllocation

namespace
{
    class ReadbackServer final : public lux::render::GeneralRenderServer
    {
    public:
        ReadbackServer(
            std::shared_ptr<lux::render::TRenderControlChannel<>> control,
            std::shared_ptr<lux::render::RenderChannelSync> sync,
            ImplOwner impl
        ) noexcept
            : GeneralRenderServer(
                  Channel::create(2),
                  std::move(control),
                  lux::render::TRenderUploadChannel<>::create(2, 1024),
                  std::move(sync),
                  std::move(impl)
              )
        {
        }

        using GeneralRenderServer::pollPendingReadbacks;
    };

    // Real source image, real copy recording and submission; only the selected native
    // failure boundary is injected. Every successful retry reads the same red pixels.
    lux::render::RenderTargetId checkReadbackAcquisition(lux::render::GeneralRenderServer::Impl& im)
    {
        using namespace lux::render;
        RenderTargetLayout layout;
        layout.slots[0] = RenderTargetSlotDesc{
            .format = lux::rdesc::ETextureFormat::RGBA8_UNORM,
            .usage = ERenderImageUsage::COLOR_ATTACHMENT | ERenderImageUsage::TRANSFER_SOURCE |
                     ERenderImageUsage::TRANSFER_DESTINATION,
            .final_state = ERenderResourceState::COLOR_ATTACHMENT
        };
        auto pool = OffscreenImagePool::create(*im.res_ctx_, layout, {2, 2}, 1);
        assert(pool);
        const auto image = (*pool)->binding().slot(ETargetSlot::SCENE_COLOR).images[0];
        RenderTargetEntry entry;
        entry.layout = layout;
        entry.pool = std::move(*pool);
        const auto target = im.targets_registry_->insert(std::move(entry));
        const VkDevice device = im.dev_ctx_->logicalDevice();
        auto command = CommandBufferOwner::create(device, im.res_ctx_->commandPool());
        assert(command);
        const auto cb = command->get();
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        assert(vkBeginCommandBuffer(cb, &begin) == VK_SUCCESS);
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(
            cb,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            0,
            0,
            nullptr,
            0,
            nullptr,
            1,
            &barrier
        );
        const VkClearColorValue red{{1.f, 0.f, 0.f, 1.f}};
        vkCmdClearColorImage(cb, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &red, 1, &barrier.subresourceRange);
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        vkCmdPipelineBarrier(
            cb,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            0,
            0,
            nullptr,
            0,
            nullptr,
            1,
            &barrier
        );
        assert(vkEndCommandBuffer(cb) == VK_SUCCESS);
        const VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        auto fence = FenceOwner::create(device, fence_info);
        assert(fence);
        auto handle = fence->get();
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cb;
        assert(vkQueueSubmit(im.dev_ctx_->graphicsQueue(), 1, &submit, handle) == VK_SUCCESS);
        assert(vkWaitForFences(device, 1, &handle, VK_TRUE, 5'000'000'000ull) == VK_SUCCESS);

        for (const auto failure :
             {EFault::BEGIN, EFault::END, EFault::FENCE, EFault::SUBMIT, EFault::INVALIDATE, EFault::NONE})
        {
            std::array<uint8_t, 16> pixels;
            pixels.fill(0xAB);
            GeneralRenderServer::Impl::PendingReadback job;
            job.target = target;
            job.dst_ptr = reinterpret_cast<uintptr_t>(pixels.data());
            job.dst_capacity = pixels.size();
            fault = failure;
            submissions = 0;
            const auto status = submitReadbackCopy(im, job);
            const bool is_recording_failure = failure == EFault::BEGIN || failure == EFault::END;
            const bool is_rejected = is_recording_failure || failure == EFault::FENCE || failure == EFault::SUBMIT;
            if (is_rejected)
            {
                assert(status == (is_recording_failure ? 7u : 8u));
                assert(!job.copy && pixels[0] == 0xAB);
                assert(submissions == (failure == EFault::SUBMIT ? 1u : 0u));
                fault = EFault::NONE;
                assert(submitReadbackCopy(im, job) == 0);
            }
            else
            {
                assert(status == 0 && job.copy);
            }
            handle = job.copy->fence.get();
            assert(vkWaitForFences(device, 1, &handle, VK_TRUE, 5'000'000'000ull) == VK_SUCCESS);
            const auto reply = finishReadbackCopy(im, job);
            assert(!job.copy);
            if (failure == EFault::INVALIDATE)
            {
                assert(reply.status == 10 && pixels[0] == 0xAB);
            }
            else
            {
                assert(reply.status == 0 && reply.bytes_written == 16 && reply.width == 2 && reply.height == 2);
                for (std::size_t pixel = 0; pixel < pixels.size(); pixel += 4)
                {
                    assert(
                        pixels[pixel] == 255 && pixels[pixel + 1] == 0 && pixels[pixel + 2] == 0 &&
                        pixels[pixel + 3] == 255
                    );
                }
            }
            fault = EFault::NONE;
        }
        std::puts(
            "readback: native begin/end/fence/submit rejection and retry, visibility failure, real red pixels PASS"
        );
        return target;
    }
} // namespace

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    using namespace lux::render;
    auto sync = std::make_shared<RenderChannelSync>();
    auto control = TRenderControlChannel<>::create(2);
    ServerConfig config;
    config.enable_validation = true;
    std::atomic<int> validation_errors{};
    config.validation_error_counter = &validation_errors;
    auto prepared = GeneralRenderServer::Impl::create(config, *sync);
    assert(prepared);
    auto& im = **prepared;
    const VkDevice device = im.dev_ctx_->logicalDevice();
    const VkCommandPool pool = im.res_ctx_->commandPool();
    const VmaAllocator allocator = im.dev_ctx_->vmaAllocator();
    auto server = std::make_unique<ReadbackServer>(control, sync, std::move(*prepared));
    const auto target = checkReadbackAcquisition(im);

    GeneralRenderServer::Impl::PendingReadback pending;
    pending.deadline = 1;
    pending.request_id = 41;
    VkBufferCreateInfo buffer{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    buffer.size = 16;
    buffer.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VmaAllocationCreateInfo allocation{};
    allocation.usage = VMA_MEMORY_USAGE_AUTO;
    allocation.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    auto backing = VmaBuffer::create(allocator, buffer, allocation);
    assert(backing);
    auto command = CommandBufferOwner::create(device, pool);
    assert(command);
    const auto cb = command->get();
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    assert(vkBeginCommandBuffer(cb, &begin) == VK_SUCCESS);
    assert(vkEndCommandBuffer(cb) == VK_SUCCESS);
    VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    auto fence = FenceOwner::create(device, fence_info);
    assert(fence);
    pending.copy.emplace(std::move(*backing), std::move(*command), std::move(*fence));
    VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    VkSemaphoreCreateInfo semaphore_info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    semaphore_info.pNext = &type;
    VkSemaphore gate{};
    assert(vkCreateSemaphore(device, &semaphore_info, nullptr, &gate) == VK_SUCCESS);
    const uint64_t value = 1;
    VkTimelineSemaphoreSubmitInfo timeline{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
    timeline.waitSemaphoreValueCount = 1;
    timeline.pWaitSemaphoreValues = &value;
    const VkPipelineStageFlags stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.pNext = &timeline;
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &gate;
    submit.pWaitDstStageMask = &stage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cb;
    assert(vkQueueSubmit(im.dev_ctx_->graphicsQueue(), 1, &submit, pending.copy->fence.get()) == VK_SUCCESS);
    watched_fence = pending.copy->fence.get();
    watched_command = pending.copy->command.get();
    watched_buffer = pending.copy->buffer.buffer();
    blocked = true;
    im.pending_readbacks_.push_back(std::move(pending));
    advancePendingReadbacks(im);
    assert(im.pending_readbacks_[0].reply && im.pending_readbacks_[0].reply->status == 9);
    assert(vkGetFenceStatus(device, watched_fence) == VK_NOT_READY);
    // Fill the original response channel: timeout must survive reply backpressure.
    unsigned occupied = 0;
    while (auto* response = control->responses.tryBeginWrite())
    {
        response->clear_keep_capacity();
        if (!control->responses.publishWrite())
        {
            break;
        }
        ++occupied;
    }
    assert(occupied > 0);
    server->pollPendingReadbacks();
    assert(im.pending_readbacks_[0].copy && im.pending_readbacks_[0].request_id == 41);
    while (control->responses.tryAcquireRead())
    {
    }
    server->pollPendingReadbacks();
    assert(im.pending_readbacks_[0].copy && im.pending_readbacks_[0].request_id == kInvalidRequestId);
    assert(control->responses.tryAcquireRead());
    const auto& response = control->responses.currentRead();
    assert(response.replies.size() == 1 && response.replies[0].request_id == 41);
    ReadbackTargetReply timeout{};
    std::memcpy(&timeout, response.payload.data() + response.replies[0].payload_offset, sizeof(timeout));
    assert(timeout.status == 9 && timeout.bytes_written == 0);
    server->pollPendingReadbacks();
    assert(!control->responses.tryAcquireRead());
    std::printf("actual blocked GPU readback: premature release attempts=%u (expected 0)\n", early_release);

    VkSemaphoreSignalInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO};
    signal.semaphore = gate;
    signal.value = 1;
    assert(vkSignalSemaphore(device, &signal) == VK_SUCCESS);
    assert(vkWaitForFences(device, 1, &watched_fence, VK_TRUE, 5'000'000'000ull) == VK_SUCCESS);
    blocked = false;
    assert(early_release == 0);
    // The original reply was already delivered. Physical completion does not reply or copy again.
    server->pollPendingReadbacks();
    assert(!control->responses.tryAcquireRead());
    assert(im.pending_readbacks_.empty());
    // Exercise the actual synchronous handler and its finite wait with a real blocked
    // copy, then retain the backing in the same pending queue after the immediate reply.
    std::array<uint8_t, 16> sync_pixels;
    sync_pixels.fill(0xAB);
    ReadbackTargetPayload payload{};
    payload.target = target;
    payload.dst_ptr = reinterpret_cast<uintptr_t>(sync_pixels.data());
    payload.dst_capacity = sync_pixels.size();
    payload.slot = static_cast<uint32_t>(ETargetSlot::SCENE_COLOR);
    TCommandStorage<64> program;
    TReplyPacket<64> replies;
    TFrameReplyBuilder<64> builder(replies);
    builder.begin();
    CmdRecord request{};
    request.request_id = 58;
    TExecuteContext<64, 64> context{program, builder, &im, &request};
    submit_gate = gate;
    submit_gate_value = 2;
    handleReadbackTarget(context, payload);
    submit_gate = VK_NULL_HANDLE;
    assert(replies.replies.size() == 1 && replies.replies[0].request_id == 58);
    assert(im.pending_readbacks_.size() == 1 && im.pending_readbacks_[0].copy);
    assert(im.pending_readbacks_[0].reply->status == 9);
    const auto sync_fence = im.pending_readbacks_[0].copy->fence.get();
    assert(vkGetFenceStatus(device, sync_fence) == VK_NOT_READY);
    assert(sync_pixels[0] == 0xAB);
    signal.value = 2;
    assert(vkSignalSemaphore(device, &signal) == VK_SUCCESS);
    assert(vkWaitForFences(device, 1, &sync_fence, VK_TRUE, 5'000'000'000ull) == VK_SUCCESS);
    server->pollPendingReadbacks();
    assert(im.pending_readbacks_.empty() && !control->responses.tryAcquireRead());
    assert(sync_pixels[0] == 0xAB);
    std::puts("readback: async reply backpressure/exactly once, actual synchronous timeout/no late copy PASS");
    vkDestroySemaphore(device, gate, nullptr);
    server.reset();
    assert(validation_errors.load() == 0);
    return early_release ? 1 : 0;
}
