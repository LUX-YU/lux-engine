#include <algorithm>
#include <lux/engine/render/device/DescriptorArray.hpp>
#include <lux/engine/render/device/DeviceBackend.hpp>
#include <lux/engine/render/device/DeviceSession.hpp>

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

    DeviceSession::DeviceSession() noexcept : backend_(std::make_unique<DeviceBackend>()) {}

    RenderResult<std::unique_ptr<DeviceSession>> DeviceSession::create(
        const DeviceOptions& options,
        const nri::CallbackInterface& diagnostic,
        bool validation,
        std::uint32_t slots
    ) noexcept
    {
        auto result = std::unique_ptr<DeviceSession>(new DeviceSession);
        auto initialized = DeviceBackend::initialize(*result, options, diagnostic, validation, slots);
        if (!initialized)
        {
            return cxx::unexpected(initialized.error());
        }
        return result;
    }

    std::size_t DeviceSession::queueIndex(const SubmissionQueue& queue) const noexcept
    {
        for (std::size_t index = 0; index < physical_queues_.size(); ++index)
        {
            if (physical_queues_[index].get() == &queue)
            {
                return index;
            }
        }
        std::terminate(); // Internal submissions always belong to this device.
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
        cleanup_scope_.close();
        bool joined = false;
        auto join = stdexec::connect(cleanup_scope_.join(), CloseReceiver{joined});
        stdexec::start(join);
        do
        {
            requireCleanupComplete(drain());
        } while (!joined);
        sampled_images_.reset();
        physical_queues_.clear();
        if (device_)
        {
            nri::nriDestroyDevice(std::exchange(device_, nullptr));
        }
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
        auto collected = pollQueues();
        dispatchQueueCompletions();
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

    RenderResult<void> DeviceSession::pollQueues() noexcept
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

    void DeviceSession::dispatchQueueCompletions() noexcept
    {
        for (auto& queue : physical_queues_)
        {
            queue->dispatchCompleted();
        }
    }

} // namespace lux::render::detail
