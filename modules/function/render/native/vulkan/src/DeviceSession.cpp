#include "StagingArena.hpp"
#include <algorithm>
#include <lux/engine/render/native/vulkan/DescriptorArray.hpp>
#include <lux/engine/render/native/vulkan/DeviceSession.hpp>
#include <lux/engine/render/native/vulkan/ExecutionContext.hpp>
#include <lux/engine/render/native/vulkan/transfer/UploadContext.hpp>

#include <exception>

namespace lux::render::vulkan
{
    namespace
    {
        struct CloseReceiver final
        {
            using receiver_concept = stdexec::receiver_t;
            bool& joined;

            auto get_env() const noexcept
            {
                return stdexec::env{stdexec::prop{stdexec::get_scheduler, stdexec::inline_scheduler{}}};
            }

            void set_value() && noexcept
            {
                joined = true;
            }
        };
    } // namespace

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
        close();
    }

    void DeviceSession::requireCleanupComplete(const RenderResult<void>& result) noexcept
    {
        const bool device_lost = !result && result.error().type == kNativeFailure &&
                                 result.error().args[0] == static_cast<std::uint32_t>(VK_ERROR_DEVICE_LOST);
        if (!result && !device_lost)
        {
            std::terminate();
        }
    }

    void DeviceSession::close() noexcept
    {
        if (closed_)
        {
            return;
        }
        // Members that can produce cleanup are emptied while admission is still open.
        global_storages_.clear();
        cleanup_scope_.close();
        bool joined = false;
        auto join = stdexec::connect(cleanup_scope_.join(), CloseReceiver{joined});
        stdexec::start(join);
        do
        {
            requireCleanupComplete(drain());
        } while (!joined);
        uploads_.reset();
        staging_arena_.reset();
        sampled_images_.reset();
        closed_ = true;
    }

    RenderResult<void> DeviceSession::drain() noexcept
    {
        std::optional<RenderError> failure;
        for (const auto& queue : physical_queues_)
        {
            auto result = queue->waitIdle(UINT64_MAX);
            if (!failure)
            {
                if (!result)
                {
                    failure = result.error();
                }
                else if (!*result)
                {
                    failure = nativeError(VK_TIMEOUT);
                }
            }
        }
        auto collected = collect();
        dispatchCompletions();
        return failure ? RenderResult<void>{cxx::unexpected(*failure)} : collected;
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

    bool DeviceSession::pendingGpuWork() const noexcept
    {
        return std::ranges::any_of(physical_queues_, [](const auto& queue) { return !queue->idle(); });
    }

    RenderResult<bool> DeviceSession::waitForProgress(std::uint64_t timeout_ns) noexcept
    {
        for (const auto& queue : physical_queues_)
        {
            if (!queue->idle())
            {
                return queue->wait(queue->completed() + 1, timeout_ns);
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
        if (uploads_)
        {
            uploads_->flush();
        }
    }

} // namespace lux::render::vulkan
