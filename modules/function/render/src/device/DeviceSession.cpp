#include <algorithm>
#include <lux/engine/render/device/DescriptorArray.hpp>
#include <lux/engine/render/device/DeviceSession.hpp>
#include <lux/engine/render/execution/ExecutionContext.hpp>
#include <lux/engine/render/memory/StagingArena.hpp>
#include <lux/engine/render/upload/UploadContext.hpp>

#include <exception>
#include <lux/engine/render/device/Error.hpp>

namespace lux::render::detail
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
        std::unique_ptr<nri::Device, decltype(&nri::nriDestroyDevice)> device,
        nri::CoreInterface api,
        nri::WrapperVKInterface vk,
        EnabledDeviceFeatures enabled
    ) noexcept
        : device_(std::move(device)), api_(api), vk_(vk), enabled_features_(enabled),
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
        const bool device_lost = !result && result.error().type == kDeviceFailure &&
                                 result.error().args[0] == static_cast<std::uint32_t>(nri::Result::DEVICE_LOST);
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
                    failure = RenderError{kDeviceBusy};
                }
            }
        }
        auto collected = collect();
        dispatchCompletions();
        return failure ? RenderResult<void>{cxx::unexpected(*failure)} : collected;
    }

    bool DeviceSession::deviceLost() const noexcept
    {
        for (const auto& queue : physical_queues_)
        {
            if (queue->deviceLost())
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
        const auto count = physical_queues_.size();
        const auto& frame = frames_[next_frame_];
        if (frame.phase == EFramePhase::SUBMITTED)
        {
            for (std::size_t offset = 0; offset < count; ++offset)
            {
                const auto index = (wait_queue_cursor_ + offset) % count;
                auto& queue = *physical_queues_[index];
                const auto serial = frame.accepted_serials[index];
                if (serial > queue.completed())
                {
                    wait_queue_cursor_ = static_cast<std::uint8_t>((index + 1) % count);
                    return queue.wait(serial, timeout_ns);
                }
            }
        }
        // No submitted frame is blocking reuse. Rotate through the actual Queue frontiers
        // so background uploads and cleanup do not always wait behind physical queue zero.
        for (std::size_t offset = 0; offset < count; ++offset)
        {
            const auto index = (wait_queue_cursor_ + offset) % count;
            auto& queue = physical_queues_[index];
            if (!queue->idle())
            {
                wait_queue_cursor_ = static_cast<std::uint8_t>((index + 1) % count);
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

} // namespace lux::render::detail
