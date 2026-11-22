#pragma once

#include "Support.hpp"
#include <lux/engine/render/graph/execute/FrameExecutor.hpp>
#include <lux/engine/render/runtime/JobExecution.hpp>
#include <lux/engine/core/execution/ExecutionRuntime.hpp>
#include <lux/engine/render/runtime/FrameOperation.hpp>

namespace foundation_test
{
    // Native probes install detached Jobs without a RenderServer. Give each installation a distinct execution identity.
    inline runtime_detail::JobExecution::Installation testInstallation() noexcept
    {
        static std::atomic<std::uint64_t> next{1};
        return {.identity = {RenderJobId{0, 1}, next.fetch_add(1, std::memory_order_relaxed)}};
    }

    inline RenderResult<std::span<nri::CommandBuffer* const>> recordFrame(
        const runtime_detail::JobExecution& job, const DeviceSession& device, std::uint32_t slot, std::uint32_t batch
    ) noexcept
    {
        if (const auto error = device.terminalError(); error.type) return lux::cxx::unexpected(error);
        if (slot >= job.frameCount()) return lux::cxx::unexpected(RenderError{kDeviceArgument});
        const auto* frame = job.frame(slot).recordingFrame();
        if (!frame) return lux::cxx::unexpected(RenderError{kDeviceArgument});
        return FrameExecutor::recordBatch(*frame, device, batch);
    }

    inline RasterDrawList& rasterDraws(runtime_detail::JobExecution& job, const CompiledPass& pass, std::uint32_t slot)
    {
        auto& frame = job.frame(slot);
        frame.invalidatePass(job.graph(), pass.binding_slot);
        const auto& raster = *job.graph().rasterCommands(pass);
        return frame.raster_draws[std::get<RasterListCommands>(raster.draw).data_index];
    }

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

        [[nodiscard]] bool completed(std::uint32_t slot, const runtime_detail::JobExecution& job) const noexcept
        {
            const auto* receipt = workspace_.submissions.acceptedFrameUse(slot, 0, job.binding_identity);
            return receipt && receipt->completed;
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
        render(
            std::span<runtime_detail::JobExecution* const> executions, std::span<const ResourceInstanceId> storages,
            std::span<const SubmissionOrder> order = {}
        ) noexcept
        {
            if (auto error = device_.terminalError(); error.type) co_return lux::cxx::unexpected(error);
            const bool in_flight = operation_ && operation_->active();
            if (!in_flight && (executions.empty() || !open())) co_return 0;
            co_return co_await operation_->run(executions, storages, order);
        }

    private:
        [[nodiscard]] bool open() noexcept
        {
            if (operation_ && operation_->active()) return true;
            operation_.emplace(
                device_, progress_, workspace_, lux::execution::ExecutionRuntime::instance().cpuScheduler()
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
