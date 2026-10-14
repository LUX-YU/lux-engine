#include <lux/engine/render/native/vulkan/DeviceSession.hpp>
#include "ExecutionState.hpp"
#include <algorithm>

#include <exception>

namespace lux::render::vulkan
{
    DeviceSession::DeviceSession(VulkanDevice native, VulkanAllocator memory) noexcept
        : device_(std::move(native)), allocator_(std::move(memory))
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
                if (result->device_.queue(role).handle == result->queues_[j]->nativeQueue().handle)
                {
                    result->queues_[i] = result->queues_[j];
                    result->retirements_[i] = result->retirements_[j];
                    break;
                }
            }
            if (result->queues_[i]) continue;
            auto queue = SubmissionQueue::create(result->device_, slots, role);
            if (!queue) return cxx::unexpected(queue.error());
            result->queue_owners_[i] = std::move(*queue);
            result->queues_[i] = result->queue_owners_[i].get();
            result->retirement_owners_[i].emplace(*result, *result->queues_[i], retirement_capacity);
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
        collectScoped();
        return {};
    }

    void DeviceSession::collectScoped() noexcept
    {
        std::erase_if(scoped_, [&](const auto& entry) {
            if (entry->leases || entry->live_scope) return false;
            destroyFinished(entry->backing);
            return true;
        });
    }

    void DeviceSession::releaseScope(SceneId scene, ViewId view) noexcept
    {
        for (auto& entry : scoped_)
        {
            const bool scene_matches = entry->requirement.scope != EResourceScope::GLOBAL && entry->scope.scene == scene;
            const bool view_matches = view.isNull() || entry->scope.view == view;
            if (scene_matches && view_matches) entry->live_scope = false;
        }
        collectScoped();
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
        for (auto& entry : scoped_) destroyFinished(entry->backing);
        scoped_.clear();
        while (!buffers_.empty()) destroyFinished(&*buffers_.begin());
        // Destroy aliases before allocations, including aliases not explicitly released by the caller.
        for (;;)
        {
            auto alias = std::find_if(images_.begin(), images_.end(), [](const auto& value) { return value.source_; });
            if (alias == images_.end()) break;
            destroyFinished(&*alias);
        }
        while (!images_.empty()) destroyFinished(&*images_.begin());
        // Retirement owners, queue owners, allocator, device: reverse member destruction order.
    }
}
