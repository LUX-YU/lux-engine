#include <lux/engine/render/runtime/DeviceSession.hpp>

namespace lux::render::vulkan
{
    RenderResult<void> DeviceContext::drain() noexcept
    {
        for (auto& queue : queue_owners)
        {
            if (!queue)
            {
                continue;
            }
            if (queue->recording())
            {
                return cxx::unexpected(RenderError{kBusy});
            }
            const auto ticket = queue->lastSubmitted();
            if (!ticket)
            {
                continue;
            }
            auto completed = queue->wait(*ticket, UINT64_MAX);
            if (!completed)
            {
                return cxx::unexpected(resourceError(completed.error()));
            }
            if (!*completed)
            {
                return cxx::unexpected(RenderError{kBusy});
            }
        }
        return {};
    }

    RenderResult<std::vector<VkPhysicalDevice>> InstanceContext::physicalDevices() const noexcept
    {
        for (;;)
        {
            std::uint32_t count{};
            auto result = vkEnumeratePhysicalDevices(instance.native(), &count, nullptr);
            if (result != VK_SUCCESS)
            {
                return cxx::unexpected(resourceError(nativeError(result)));
            }
            std::vector<VkPhysicalDevice> devices(count);
            result = vkEnumeratePhysicalDevices(instance.native(), &count, devices.data());
            if (result == VK_INCOMPLETE)
            {
                continue;
            }
            if (result != VK_SUCCESS)
            {
                return cxx::unexpected(resourceError(nativeError(result)));
            }
            devices.resize(count);
            return devices;
        }
    }

    DeviceSession::DeviceSession(const InstanceContext& host, DeviceContext native, Sampler sampler) noexcept
        : instance(host), device(std::move(native)),
          resources{std::move(sampler), device.allocator, *device.queues[0], {}, {}}
    {
    }

    DeviceSession::~DeviceSession() noexcept
    {
        // Wait every actual Native ticket (including partial submits) before releasing domain resources.
        resources.executions.clear();
        // Domain resources can also be used by cold Native transfer work outside a Graph.
        auto drained = device.drain();
        if (!drained)
        {
            const bool device_lost = drained.error().type == kNativeFailure &&
                                     drained.error().args[0] == static_cast<std::uint32_t>(VK_ERROR_DEVICE_LOST);
            if (!device_lost)
            {
                std::terminate();
            }
        }
    }

    RenderResult<std::unique_ptr<DeviceSession>> DeviceSession::create(
        const InstanceContext& host,
        const BackendOptions& native
    ) noexcept
    {
        auto device = VulkanDevice::create(host.instance, native.device);
        if (!device)
        {
            return cxx::unexpected(resourceError(device.error()));
        }
        auto allocator = VulkanAllocator::create(*device);
        if (!allocator)
        {
            return cxx::unexpected(resourceError(allocator.error()));
        }
        std::array<std::unique_ptr<SubmissionQueue>, 3> owners;
        std::array<SubmissionQueue*, 3> queues{};
        for (unsigned role = 0; role < 3; ++role)
        {
            const auto selected = device->queue(static_cast<EQueueRole>(role));
            for (unsigned earlier = 0; earlier < role; ++earlier)
            {
                if (queues[earlier]->nativeQueue().handle == selected.handle)
                {
                    queues[role] = queues[earlier];
                    break;
                }
            }
            if (queues[role])
            {
                continue;
            }
            auto queue =
                SubmissionQueue::create(*device, native.capacity.queue_submissions, static_cast<EQueueRole>(role));
            if (!queue)
            {
                return cxx::unexpected(resourceError(queue.error()));
            }
            owners[role] = std::move(*queue);
            queues[role] = owners[role].get();
        }
        auto sampler = Sampler::create(*device);
        if (!sampler)
        {
            return cxx::unexpected(resourceError(sampler.error()));
        }
        auto result = std::unique_ptr<DeviceSession>{new DeviceSession(
            host,
            {std::move(*device), std::move(*allocator), std::move(owners), queues},
            std::move(*sampler)
        )};
        result->graphs.reserve(native.capacity.graphs);
        result->resources.executions.prepareCapacity(native.capacity.graphs, native.capacity.graphs);
        return result;
    }

    BufferFactory ResourceContext::bufferFactory(VkDeviceSize size, VkBufferUsageFlags usage) const noexcept
    {
        return {allocator, size, usage, initialization_queue};
    }

    RenderResult<std::unique_ptr<Buffer>> BufferFactory::operator()() const noexcept
    {
        const bool invalid = size == 0 || size % 4 != 0 || !(usage & VK_BUFFER_USAGE_TRANSFER_DST_BIT) ||
                             queue.device() != allocator.device();
        if (invalid)
        {
            return cxx::unexpected(RenderError{kInvalidArgument});
        }
        auto candidate = Buffer::create(allocator, size, usage, EMemoryAccess::DEVICE);
        if (!candidate)
        {
            return cxx::unexpected(resourceError(candidate.error()));
        }
        auto batch = queue.begin();
        if (!batch)
        {
            return cxx::unexpected(resourceError(batch.error()));
        }
        vkCmdFillBuffer(batch->native(), candidate->native(), 0, size, 0);
        auto ticket = std::move(*batch).submit();
        if (!ticket)
        {
            return cxx::unexpected(resourceError(ticket.error()));
        }
        // Minimal cold initialization. Bulk asynchronous staging belongs to R3-D.
        auto done = queue.wait(*ticket, UINT64_MAX);
        if (!done || !*done)
        {
            if (!queue.deviceLost())
            {
                std::terminate();
            }
            return cxx::unexpected(resourceError(done ? RenderError{kBusy} : done.error()));
        }
        return std::make_unique<Buffer>(std::move(*candidate));
    }

    bool BufferFactory::isCompatible(const Buffer& buffer) const noexcept
    {
        return buffer.device() == allocator.device() && buffer.size() == size && buffer.usage() == usage &&
               buffer.access() == EMemoryAccess::DEVICE;
    }
} // namespace lux::render::vulkan
