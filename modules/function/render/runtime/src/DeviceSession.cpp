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
                return cxx::unexpected(completed.error());
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
                return cxx::unexpected(nativeError(result));
            }
            std::vector<VkPhysicalDevice> devices(count);
            result = vkEnumeratePhysicalDevices(instance.native(), &count, devices.data());
            if (result == VK_INCOMPLETE)
            {
                continue;
            }
            if (result != VK_SUCCESS)
            {
                return cxx::unexpected(nativeError(result));
            }
            devices.resize(count);
            return devices;
        }
    }

    DeviceSession::DeviceSession(InstanceContext host, DeviceContext native, Sampler sampler) noexcept
        : instance(std::move(host)), device(std::move(native)), resources{std::move(sampler), device.allocator, {}}
    {
        root_domain_ = domains_.emplace(std::make_unique<ResourceDomain>());
        domain_order_.push_back(root_domain_);
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
        for (auto i = domain_order_.rbegin(); i != domain_order_.rend(); ++i)
        {
            domains_.erase(*i);
        }
    }

    RenderResult<std::unique_ptr<DeviceSession>> DeviceSession::create(const BackendOptions& native) noexcept
    {
        auto instance = VulkanInstance::create(native.instance);
        if (!instance)
        {
            return cxx::unexpected(instance.error());
        }
        auto device = VulkanDevice::create(*instance, native.device);
        if (!device)
        {
            return cxx::unexpected(device.error());
        }
        auto allocator = VulkanAllocator::create(*device);
        if (!allocator)
        {
            return cxx::unexpected(allocator.error());
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
                return cxx::unexpected(queue.error());
            }
            owners[role] = std::move(*queue);
            queues[role] = owners[role].get();
        }
        auto sampler = Sampler::create(*device);
        if (!sampler)
        {
            return cxx::unexpected(sampler.error());
        }
        auto result = std::unique_ptr<DeviceSession>{new DeviceSession(
            {std::move(*instance)},
            {std::move(*device), std::move(*allocator), std::move(owners), queues},
            std::move(*sampler)
        )};
        result->graphs.reserve(native.capacity.graphs);
        result->resources.executions.prepareCapacity(native.capacity.graphs, native.capacity.graphs);
        return result;
    }

    RenderResult<ResourceDomainId> DeviceSession::createResourceDomain(ResourceDomainId parent) noexcept
    {
        const auto* ancestor = tryGetResourceDomain(parent);
        if (!ancestor)
        {
            return cxx::unexpected(RenderError{kInvalidIdentity});
        }
        const auto id = domains_.emplace(std::make_unique<ResourceDomain>(parent, ancestor));
        domain_order_.push_back(id);
        return id;
    }

    ResourceDomain* DeviceSession::tryGetResourceDomain(ResourceDomainId id) noexcept
    {
        auto* found = domains_.find(id);
        return found ? found->get() : nullptr;
    }

    const ResourceDomain* DeviceSession::tryGetResourceDomain(ResourceDomainId id) const noexcept
    {
        const auto* found = domains_.find(id);
        return found ? found->get() : nullptr;
    }

    ResourceDomain& DeviceSession::getResourceDomain(ResourceDomainId id) noexcept
    {
        auto* found = tryGetResourceDomain(id);
        if (!found)
        {
            std::terminate();
        }
        return *found;
    }

    const ResourceDomain& DeviceSession::getResourceDomain(ResourceDomainId id) const noexcept
    {
        const auto* found = tryGetResourceDomain(id);
        if (!found)
        {
            std::terminate();
        }
        return *found;
    }

    RenderResult<void> DeviceSession::removeResourceDomain(ResourceDomainId id) noexcept
    {
        if (!tryGetResourceDomain(id))
        {
            return cxx::unexpected(RenderError{kInvalidIdentity});
        }
        if (id == root_domain_)
        {
            return cxx::unexpected(RenderError{kBusy});
        }
        for (const auto& domain : domains_)
        {
            if (domain->parentId() == id)
            {
                return cxx::unexpected(RenderError{kBusy});
            }
        }
        auto drained = device.drain();
        if (!drained)
        {
            return cxx::unexpected(drained.error());
        }
        domains_.erase(id);
        std::erase(domain_order_, id);
        return {};
    }

    BufferFactory ResourceContext::bufferFactory(VkDeviceSize size, VkBufferUsageFlags usage) const noexcept
    {
        return {allocator, size, usage};
    }

    RenderResult<std::unique_ptr<Buffer>> BufferFactory::operator()() const noexcept
    {
        auto candidate = Buffer::create(allocator, size, usage, EMemoryAccess::DEVICE);
        if (!candidate)
        {
            return cxx::unexpected(candidate.error());
        }
        return std::make_unique<Buffer>(std::move(*candidate));
    }

    bool BufferFactory::isCompatible(const Buffer& buffer) const noexcept
    {
        return buffer.device() == allocator.device() && buffer.size() == size && buffer.usage() == usage &&
               buffer.access() == EMemoryAccess::DEVICE;
    }
} // namespace lux::render::vulkan
