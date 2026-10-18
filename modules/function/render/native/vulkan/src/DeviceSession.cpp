#include "StagingArena.hpp"
#include <algorithm>
#include <lux/engine/render/native/vulkan/DescriptorArray.hpp>
#include <lux/engine/render/native/vulkan/DeviceSession.hpp>
#include <lux/engine/render/native/vulkan/ExecutionContext.hpp>
#include <lux/engine/render/native/vulkan/transfer/UploadContext.hpp>

#include <exception>

namespace lux::render::vulkan
{
    DeviceSession::DeviceSession(VulkanDevice native, VulkanAllocator memory) noexcept
        : device_(std::move(native)), allocator_(std::move(memory)),
          resources_(this, (device_.textureDescriptors() ? 1u : 0u) | (device_.textureCompressionBc() ? 2u : 0u)),
          global_storages_(resources_)
    {
    }

    DeviceSession::~DeviceSession() noexcept
    {
        auto result = drain();
        const bool is_device_lost = !result && result.error().type == kNativeFailure &&
                                    result.error().args[0] == static_cast<std::uint32_t>(VK_ERROR_DEVICE_LOST);
        if (!result && !is_device_lost)
        {
            std::terminate();
        }
        // Logical subscriptions/actual Sets end while Native retirement and Queue owners still exist.
        contexts_.clear();
        global_storages_.clear();
        // On Device Lost, batches retain resources until queue teardown instead of a completed fence.
        // Release them while the allocator/device and descriptor-table lifetime check still exist.
        retirement_.reset();
        physical_queues_.clear();
        uploads_.reset();
        staging_arena_.reset();
        if (!sampled_images_.expired())
        {
            std::terminate();
        }
        sampled_images_.reset();
        // Queue resource sets leave before allocator and device destruction.
    }

    RenderResult<std::unique_ptr<DeviceSession>> DeviceSession::create(
        const InstanceContext& instance,
        const DeviceOptions& options,
        std::uint32_t slots
    ) noexcept
    {
        if (slots == 0)
        {
            return cxx::unexpected(RenderError{kInvalidArgument});
        }
        auto device = VulkanDevice::create(instance, options);
        if (!device)
        {
            return cxx::unexpected(device.error());
        }
        auto allocator = VulkanAllocator::create(*device);
        if (!allocator)
        {
            return cxx::unexpected(allocator.error());
        }
        auto result = std::unique_ptr<DeviceSession>(new DeviceSession(std::move(*device), std::move(*allocator)));
        result->physical_queues_.reserve(3);
        for (std::size_t i = 0; i < 3; ++i)
        {
            const auto role = static_cast<EQueueRole>(i);
            const auto handle = result->device_.queue(role).handle;
            const auto existing = std::ranges::find_if(
                result->physical_queues_,
                [&](const auto& queue) { return queue->nativeQueue().handle == handle; }
            );
            const auto index = static_cast<std::uint8_t>(existing - result->physical_queues_.begin());
            if (existing == result->physical_queues_.end())
            {
                auto queue = SubmissionQueue::create(result->device_, slots, role);
                if (!queue)
                {
                    return cxx::unexpected(queue.error());
                }
                result->physical_queues_.push_back(std::move(*queue));
            }
            result->queue_roles_[i] = index;
        }
        result->retirement_.reset(new RetirementQueue(result->physical_queues_));
        return result;
    }

    RenderResult<void> DeviceSession::drain() noexcept
    {
        std::optional<RenderError> failure;
        for (const auto& queue : physical_queues_)
        {
            if (auto ticket = queue->lastSubmitted())
            {
                auto result = queue->wait(*ticket, UINT64_MAX);
                if (!result && !failure)
                {
                    failure = result.error();
                }
            }
        }
        if (failure)
        {
            return cxx::unexpected(*failure);
        }
        return collect();
    }

    SubmissionQueue* DeviceSession::queueFor(SubmissionTicket ticket) noexcept
    {
        for (const auto& owner : physical_queues_)
        {
            if (owner.get() == &ticket.owner() && owner->owns(ticket))
            {
                return owner.get();
            }
        }
        return nullptr;
    }

    bool DeviceSession::deviceLost() const noexcept
    {
        for (const auto& owner : physical_queues_)
        {
            if (owner->deviceLost())
            {
                return true;
            }
        }
        return false;
    }

    RenderResult<void> DeviceSession::collect() noexcept
    {
        std::optional<RenderError> failure;
        for (auto& queue : physical_queues_)
        {
            if (auto result = queue->poll(); !result && !failure)
            {
                failure = result.error();
            }
        }
        if (retirement_)
        {
            retirement_->collectReady();
        }
        if (failure)
        {
            return cxx::unexpected(*failure);
        }
        return {};
    }

    void DeviceSession::dispatchCompletions() noexcept
    {
        for (auto& queue : physical_queues_)
        {
            queue->dispatchCompleted();
        }
    }

} // namespace lux::render::vulkan
