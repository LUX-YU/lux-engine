#pragma once

#include "Support.hpp"
#include <lux/engine/core/execution/ExecutionRuntime.hpp>
#include <lux/engine/render/runtime/FrameOperation.hpp>

namespace foundation_test
{
    // Drives the same private round and workspace from the standalone native probes.
    class FrameHarness final
    {
    public:
        FrameHarness(DeviceSession& device, std::uint32_t count) noexcept
            : device_(device), progress_(count, &device, device.queueCount()),
              workspace_(count, device.queueCount(), 0)
        {
        }

        [[nodiscard]] std::uint32_t frameCount() const noexcept { return progress_.frameCount(); }

        [[nodiscard]] bool beginFrame(std::uint32_t& slot) noexcept
        {
            if (!open()) return false;
            slot = operation_->context().slot_index;
            return true;
        }

        [[nodiscard]] bool submitting() const noexcept
        {
            return operation_ && operation_->submitting();
        }

        [[nodiscard]] bool submitting(const RenderGraphInstance& instance) const noexcept
        {
            return operation_ && operation_->submitting(instance);
        }

        [[nodiscard]] bool dumpSubmissions(DumpSink sink) const noexcept
        {
            return workspace_.submissions.dumpSubmissions(progress_, sink);
        }

        [[nodiscard]] RenderResult<bool> waitForProgress(std::uint64_t timeout) noexcept
        {
            return progress_.waitForProgress(device_, timeout);
        }

        [[nodiscard]] exec::task<RenderResult<std::uint32_t>>
        render(std::span<RenderGraphInstance* const> instances, std::span<const ResourceInstanceId> storages) noexcept
        {
            if (auto error = device_.terminalError(); error.type) co_return lux::cxx::unexpected(error);
            const bool in_flight = operation_ && operation_->active();
            if (!in_flight && (instances.empty() || !open())) co_return 0;
            co_return co_await operation_->run(instances, storages);
        }

    private:
        [[nodiscard]] bool open() noexcept
        {
            if (operation_ && operation_->active()) return true;
            operation_.emplace(
                device_, nullptr, progress_, workspace_, lux::execution::ExecutionRuntime::instance().cpuScheduler()
            );
            auto opened = operation_->tryBegin();
            return opened && *opened;
        }

        DeviceSession& device_;
        FrameProgress progress_;
        FrameOperation::Workspace workspace_;
        std::optional<FrameOperation> operation_;
    };
} // namespace foundation_test
