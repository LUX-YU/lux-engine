#include <algorithm>
#include <lux/engine/render/device/CompiledPrograms.hpp>
#include <lux/engine/render/device/DeviceBackend.hpp>
#include <lux/engine/render/extensions/device/DescriptorTable.hpp>
#include <lux/engine/render/extensions/device/DeviceSession.hpp>

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

    DeviceSession::DeviceSession(cxx::execution::ManualContext::Scheduler native_scheduler) noexcept
        : native_scheduler_(native_scheduler), descriptor_table_(*this)
    {
    }

    RenderResult<std::vector<nri::AdapterDesc>> DeviceSession::enumerateAdapters() noexcept
    {
        std::uint32_t count = 0;
        auto status = nri::nriEnumerateAdapters(nullptr, count);
        if (status != nri::Result::SUCCESS)
        {
            return cxx::unexpected(nativeError(status));
        }
        std::vector<nri::AdapterDesc> adapters(count);
        if (count)
        {
            status = nri::nriEnumerateAdapters(adapters.data(), count);
            if (status != nri::Result::SUCCESS)
            {
                return cxx::unexpected(nativeError(status));
            }
            adapters.resize(count);
        }
        return adapters;
    }

    RenderResult<std::unique_ptr<DeviceSession>> DeviceSession::create(
        const DeviceOptions& options,
        const nri::CallbackInterface& diagnostic,
        bool validation,
        cxx::execution::ManualContext::Scheduler native_scheduler
    ) noexcept
    {
        auto result = std::unique_ptr<DeviceSession>(new DeviceSession(native_scheduler));
        auto initialized = DeviceBackend::initialize(*result, options, diagnostic, validation);
        if (!initialized)
        {
            return cxx::unexpected(initialized.error());
        }
        result->use_device_addresses_ = options.buffer_device_address;
        result->graph_queue_costs_ = options.graph_queue_costs;
        result->programs_ = std::make_unique<CompiledPrograms>(*result, options.cached_unused_programs);
        return result;
    }

    cxx::execution::ManualContext::Scheduler DeviceSession::nativeScheduler() const noexcept
    {
        return native_scheduler_;
    }

    RenderError DeviceSession::terminalError() const noexcept
    {
        for (const auto& queue : physical_queues_)
        {
            if (const auto error = queue->terminalError(); error.type)
            {
                return error;
            }
        }
        return {};
    }

    exec::task<RenderResult<void>> DeviceSession::waitFrontiers(SubmissionFrontiers frontiers) noexcept
    {
        if (!frontiers.belongsTo(this, queueCount()))
            co_return cxx::unexpected(RenderError{kDeviceArgument});
        RenderError failure{};
        for (std::size_t q = 0; q < queueCount(); ++q)
        {
            if (frontiers.serials()[q] == 0) continue;
            auto result = co_await physicalQueue(q).whenSubmissionCompletes(frontiers.serials()[q]);
            if (!result && failure.type == 0) failure = result.error();
        }
        if (failure.type) co_return cxx::unexpected(failure);
        co_return RenderResult<void>{};
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

    void DeviceSession::requireCleanupComplete(const RenderResult<void>& result) noexcept
    {
        const bool device_lost = !result && isNativeFailure(result.error(), nri::Result::DEVICE_LOST);
        const bool retired_unknown = !result && result.error().type == kOutcomeUnknown;
        if (!result && !device_lost && !retired_unknown)
        {
            std::terminate();
        }
    }

    DeviceSession::~DeviceSession() noexcept
    {
        // Members that can produce cleanup are emptied while admission is still open.
        if (programs_) programs_->clear();
        cleanup_scope_.close();
        bool joined = false;
        auto join = stdexec::connect(cleanup_scope_.join(), CloseReceiver{joined});
        stdexec::start(join);
        do
        {
            requireCleanupComplete(drain());
        } while (!joined);
        programs_.reset();
        descriptor_table_.clear();
        physical_queues_.clear();
        if (device_)
        {
            nri::nriDestroyDevice(std::exchange(device_, nullptr));
        }
        releaseBackendData();
    }

    std::uint32_t DeviceSession::resourceCapabilities() const noexcept
    {
        return (descriptors().capacity(nri::DescriptorType::TEXTURE) ? 1u : 0u) |
            (enabledFeatures().texture_compression_bc ? 2u : 0u) |
            (descriptors().capacity(nri::DescriptorType::STRUCTURED_BUFFER) ? 4u : 0u);
    }

    RenderResult<void> DeviceSession::drain() noexcept
    {
        std::optional<RenderError> failure;
        for (const auto& queue : physical_queues_)
        {
            auto result = queue->waitIdle(UINT64_MAX);
            if (!result && queue->idle())
            {
                continue;
            }
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
        auto collected = progressCompletions();
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
        return std::ranges::any_of(
            physical_queues_,
            [](const auto& queue) { return !queue->terminalError().type && queue->completed() < queue->submitted(); }
        );
    }

    RenderResult<void> DeviceSession::progressCompletions() noexcept
    {
        std::optional<RenderError> failure;
        for (auto& queue : physical_queues_)
        {
            if (auto result = queue->poll(); !result && !failure)
            {
                failure = result.error();
            }
        }
        for (auto& queue : physical_queues_)
        {
            queue->dispatchCompleted();
        }
        if (failure)
        {
            return cxx::unexpected(*failure);
        }
        return {};
    }

} // namespace lux::render::detail
