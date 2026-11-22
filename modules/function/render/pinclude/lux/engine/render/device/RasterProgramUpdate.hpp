#pragma once

#include <lux/engine/render/graph/execute/RasterStreamPlan.hpp>
#include <lux/engine/render/runtime/ExecutionBindingId.hpp>
#include <chrono>

namespace lux::render::detail
{
    // The device owns the selected inputs and every unpublished native use through driver return.
    struct RasterProgramUpdate final
    {
        RasterProgramUpdate(DeviceSession& device, ExecutionBindingId binding) noexcept
            : device(device), binding(binding) {}
        ~RasterProgramUpdate() noexcept;
        RasterProgramUpdate(const RasterProgramUpdate&) = delete;
        RasterProgramUpdate& operator=(const RasterProgramUpdate&) = delete;

        struct Stream final
        {
            std::uint32_t pass;
            std::vector<RasterProgramLink> links;
            std::unique_ptr<DynamicRasterPrograms> candidate;
        };

        struct Selection final
        {
            std::uint32_t producer;
            std::size_t first_revision, count;
        };

        [[nodiscard]] exec::task<RenderResult<void>> compile(execution::CpuScheduler) noexcept;

        DeviceSession& device;
        const ExecutionBindingId binding;
        std::vector<std::uint64_t> revisions;
        std::vector<Selection> selections;
        std::vector<Stream> streams;
        stdexec::inplace_stop_source stop;
        std::optional<RenderError> returned;
#ifdef LUX_RENDER_DEVELOPMENT_METRICS
        std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
#endif
    };
} // namespace lux::render::detail
