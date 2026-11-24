#pragma once

#include "Support.hpp"
#include <lux/engine/render/graph/execute/FrameExecutor.hpp>
#include <lux/engine/render/runtime/JobExecution.hpp>
#include <lux/engine/core/execution/ExecutionRuntime.hpp>
#include <lux/engine/render/runtime/DeviceRound.hpp>

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
        FrameHarness(DeviceSession& device, std::uint32_t count, ResourceUploader* uploader = nullptr) noexcept
            : device_(device), progress_(count, &device, device.queueCount()),
              round_(device, progress_), uploader_(uploader)
        {
        }

        [[nodiscard]] std::uint32_t frameCount() const noexcept { return progress_.frameCount(); }

        [[nodiscard]] bool beginFrame(std::uint32_t& slot) noexcept
        {
            // The standalone driver owns admission, just as DeviceEntry does in the Server.
            if (device_.terminalError().type || !open()) return false;
            slot = round_.slot();
            return true;
        }

        [[nodiscard]] bool submitting() const noexcept
        {
            return round_.stage == runtime_detail::DeviceRound::EStage::SUBMITTING;
        }

        [[nodiscard]] bool completed(std::uint32_t slot, const runtime_detail::JobExecution& job) const noexcept
        {
            const auto* receipt = round_.submissions.acceptedFrameUse(slot, 0, job.binding_identity);
            return receipt && receipt->completed;
        }

        [[nodiscard]] bool dumpSubmissions(DumpSink sink) const noexcept
        {
            return round_.submissions.dumpSubmissions(progress_, sink);
        }

        [[nodiscard]] RenderResult<bool> waitForProgress(std::uint64_t timeout) noexcept
        {
            return progress_.waitForProgress(device_, timeout);
        }

        [[nodiscard]] exec::task<RenderResult<std::uint32_t>>
        render(
            std::span<runtime_detail::JobExecution* const> executions, std::span<const InstalledFrameResource> storages
        ) noexcept
        {
            if (auto error = device_.terminalError(); error.type) co_return lux::cxx::unexpected(error);
            if (!round_.active() && (executions.empty() || !open())) co_return 0;
            if (std::exchange(inputs_pending_, false))
            {
                round_.executions.assign(executions.begin(), executions.end());
                round_.inputs.storages.assign(storages.begin(), storages.end());
                round_.resource_uploads.clear();
                for (std::uint32_t i = 0; i < storages.size(); ++i)
                {
                    const auto& source = storages[i];
                    if (source.ops->selectRun) source.ops->selectRun(source.instance, round_.replicaFrame());
                    if (source.ops->prepareFrame) round_.resource_uploads.emplace_back(i);
                }
                if (auto selected = round_.selectConstants(); !selected)
                {
                    round_.cancel(selected.error());
                    co_return lux::cxx::unexpected(selected.error());
                }
            }
            const auto ready = round_.prepareNativeWork();
            if (!ready)
            {
                round_.cancel(ready.error());
                co_return lux::cxx::unexpected(ready.error());
            }
            if (!*ready) co_return 0;
            co_return co_await round_.submitNativeWork(lux::execution::ExecutionRuntime::instance().cpuScheduler());
        }

    private:
        [[nodiscard]] bool open() noexcept
        {
            if (round_.active()) return true;
            round_.stage = runtime_detail::DeviceRound::EStage::IDLE;
            auto selected = progress_.availableFrame(device_);
            if (!selected || !*selected) return false;
            round_.begin(**selected, uploader_);
            inputs_pending_ = true;
            return true;
        }

        DeviceSession& device_;
        FrameProgress progress_;
        runtime_detail::DeviceRound round_;
        ResourceUploader* uploader_;
        bool inputs_pending_ = false;
    };
} // namespace foundation_test
