#include <lux/engine/render/native/vulkan/DeviceSession.hpp>
#include <lux/engine/render/native/vulkan/DescriptorArray.hpp>
#include <lux/engine/render/native/vulkan/ExecutionContext.hpp>
#include <algorithm>
#include <lux/engine/render/native/vulkan/transfer/UploadContext.hpp>
#include "StagingArena.hpp"

#include <exception>

namespace lux::render::vulkan
{
    DeviceSession::DeviceSession(VulkanDevice native, VulkanAllocator memory) noexcept
        : device_(std::move(native)), allocator_(std::move(memory)),
          resources_(this, (device_.textureDescriptors() ? 1u : 0u) | (device_.textureCompressionBc() ? 2u : 0u)),
          globals_(resources_) {}

    DeviceSession::~DeviceSession() noexcept
    {
        auto result = drain();
        const bool is_device_lost = !result && result.error().type == kNativeFailure &&
            result.error().args[0] == static_cast<std::uint32_t>(VK_ERROR_DEVICE_LOST);
        if (!result && !is_device_lost) std::terminate();
        // Logical subscriptions/actual Sets end while Native retirement and Queue owners still exist.
        contexts_.clear();
        globals_.clear();
        // On Device Lost, batches retain resources until queue teardown instead of a completed fence.
        // Release them while the allocator/device and descriptor-table lifetime check still exist.
        for (auto& queue : queue_owners_) queue.reset();
        uploads_.reset();
        staging_arena_.reset();
        if (!sampled_images_.expired()) std::terminate();
        sampled_images_.reset();
        // Queue resource sets leave before allocator and device destruction.
    }

    RenderResult<std::unique_ptr<DeviceSession>> DeviceSession::create(
        const InstanceContext& instance,
        const DeviceOptions& options,
        std::uint32_t slots
    ) noexcept
    {
        if (slots == 0) return cxx::unexpected(RenderError{kInvalidArgument});
        auto device = VulkanDevice::create(instance, options);
        if (!device) return cxx::unexpected(device.error());
        auto allocator = VulkanAllocator::create(*device);
        if (!allocator) return cxx::unexpected(allocator.error());
        auto result = std::unique_ptr<DeviceSession>(new DeviceSession(std::move(*device), std::move(*allocator)));
        for (std::size_t i = 0; i < 3; ++i)
        {
            const auto role = static_cast<EQueueRole>(i);
            for (std::size_t j = 0; j < i; ++j)
            {
                if (result->device_.queue(role).handle == result->queues_[j]->nativeQueue().handle)
                {
                    result->queues_[i] = result->queues_[j];
                    break;
                }
            }
            if (result->queues_[i]) continue;
            auto queue = SubmissionQueue::create(result->device_, slots, role);
            if (!queue) return cxx::unexpected(queue.error());
            result->queue_owners_[i] = std::move(*queue);
            result->queues_[i] = result->queue_owners_[i].get();
        }
        return result;
    }

    RenderResult<void> DeviceSession::drain() noexcept
    {
        std::optional<RenderError> failure;
        for (const auto& queue : queue_owners_)
        {
            if (!queue) continue; // Cold construction/teardown, including partially initialized owners.
            if (auto ticket = queue->lastSubmitted())
            {
                auto result = queue->wait(*ticket, UINT64_MAX);
                if (!result && !failure) failure = result.error();
            }
        }
        if (failure) return cxx::unexpected(*failure);
        return collect();
    }

    SubmissionQueue* DeviceSession::queueFor(SubmissionTicket ticket) noexcept
    {
        for (auto* owner : queues_)
            if (owner == &ticket.owner() && owner->owns(ticket)) return owner;
        return nullptr;
    }

    bool DeviceSession::deviceLost() const noexcept
    {
        for (const auto& owner : queue_owners_)
            if (owner && owner->deviceLost()) return true;
        return false;
    }

    RenderResult<void> DeviceSession::collect() noexcept
    {
        std::optional<RenderError> failure;
        for (auto& queue : queue_owners_)
            if (queue)
                if (auto result = queue->poll(); !result && !failure) failure = result.error();
        if (failure) return cxx::unexpected(*failure);
        return {};
    }

    void DeviceSession::dispatchCompletions() noexcept
    {
        for (auto& queue : queue_owners_) if (queue) queue->dispatchCompleted();
    }


}
