#include "StagingArena.hpp"
#include <algorithm>
#include <lux/engine/render/native/vulkan/DescriptorArray.hpp>
#include <lux/engine/render/native/vulkan/DeviceSession.hpp>
#include <lux/engine/render/native/vulkan/ExecutionContext.hpp>
#include <lux/engine/render/native/vulkan/transfer/UploadContext.hpp>

#include <exception>

namespace lux::render::vulkan
{
    DeviceSession::DeviceSession(
        gapi::vk::PhysicalDevice physical,
        gapi::vk::LogicalDevice logical,
        EnabledDeviceFeatures enabled,
        VulkanAllocator memory
    ) noexcept
        : physical_device_(std::move(physical)), logical_device_(std::move(logical)), enabled_features_(enabled),
          allocator_(std::move(memory)),
          resources_(this, (enabled.texture_descriptors ? 1u : 0u) | (enabled.texture_compression_bc ? 2u : 0u)),
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
        global_storages_.clear();
        // Retired physical sets leave before their device-level descriptor allocation.
        retirement_.reset();
        physical_queues_.clear();
        uploads_.reset();
        staging_arena_.reset();
        sampled_images_.reset();
    }

    RenderResult<void> DeviceSession::drain() noexcept
    {
        std::optional<RenderError> failure;
        for (const auto& queue : physical_queues_)
        {
            auto result = queue->waitIdle(UINT64_MAX);
            if (!result && !failure)
            {
                failure = result.error();
            }
        }
        if (failure)
        {
            return cxx::unexpected(*failure);
        }
        auto collected = collect();
        dispatchCompletions();
        return collected;
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
        if (retirement_)
        {
            retirement_->collectReady();
        }
    }

} // namespace lux::render::vulkan
