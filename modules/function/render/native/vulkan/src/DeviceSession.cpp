#include <lux/engine/render/native/vulkan/DeviceSession.hpp>
#include "ExecutionState.hpp"
#include <algorithm>

#include <exception>

namespace lux::render::vulkan
{
    DeviceSession::DeviceSession(VulkanDevice native, VulkanAllocator memory) noexcept
        : device(std::move(native)), allocator(std::move(memory))
    {
    }

    RenderResult<std::unique_ptr<DeviceSession>> DeviceSession::create(
        const InstanceContext& instance,
        const DeviceOptions& options,
        std::uint32_t slots,
        std::size_t retirement_capacity
    ) noexcept
    {
        if (slots == 0 || retirement_capacity == 0) return cxx::unexpected(RenderError{kInvalidArgument});
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
                if (result->device.queue(role).handle == result->queues_[j]->nativeQueue().handle)
                {
                    result->queues_[i] = result->queues_[j];
                    result->retirements_[i] = result->retirements_[j];
                    break;
                }
            }
            if (result->queues_[i]) continue;
            auto queue = SubmissionQueue::create(result->device, slots, role);
            if (!queue) return cxx::unexpected(queue.error());
            result->queue_owners_[i] = std::move(*queue);
            result->queues_[i] = result->queue_owners_[i].get();
            auto retirement = RetirementQueue::create(*result->queues_[i], retirement_capacity);
            if (!retirement) return cxx::unexpected(retirement.error());
            result->retirement_owners_[i].emplace(std::move(*retirement));
            result->retirements_[i] = &*result->retirement_owners_[i];
        }
        return result;
    }

    RenderResult<void> DeviceSession::drain() noexcept
    {
        for (const auto& queue : queue_owners_)
        {
            if (!queue) continue; // Cold construction/teardown, including partially initialized owners.
            if (auto ticket = queue->lastSubmitted())
            {
                auto result = queue->wait(*ticket, UINT64_MAX);
                if (!result) return cxx::unexpected(result.error());
            }
        }
        return collect();
    }

    RenderResult<void> DeviceSession::collect() noexcept
    {
        for (auto& retirement : retirement_owners_)
        {
            if (!retirement) continue;
            if (auto result = retirement->collect(); !result) return cxx::unexpected(result.error());
        }
        std::erase_if(scoped_, [](const auto& entry) { return entry->leases == 0; });
        return {};
    }

    DeviceSession::~DeviceSession() noexcept
    {
        auto result = drain();
        const bool is_device_lost = !result && result.error().type == kNativeFailure &&
            result.error().args[0] == static_cast<std::uint32_t>(VK_ERROR_DEVICE_LOST);
        if (!result && !is_device_lost) std::terminate();
        // Retired contexts still hold scoped leases, including the device-lost path.
        for (auto& retirement : retirement_owners_) retirement.reset();
        contexts_.clear();
        scoped_.clear();
        // Retirement owners, queue owners, allocator, device: reverse member destruction order.
    }
}
