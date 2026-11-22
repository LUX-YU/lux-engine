#pragma once

#include <lux/engine/render/extensions/resource/ResourceProgramUses.hpp>
#include <lux/engine/render/graph/execute/RasterStreamPlan.hpp>
#include <lux/engine/render/runtime/ExecutionBindingId.hpp>
#include <chrono>

namespace lux::render::detail
{
    // The device owns the selected inputs and every unpublished native use through driver return.
    struct RasterProgramUpdate final
    {
        RasterProgramUpdate(
            DeviceSession& device, const ResourceRegistry& registry, ExecutionBindingId binding
        ) noexcept
            : device(device), registry(registry), binding(binding) {}
        ~RasterProgramUpdate() noexcept;
        RasterProgramUpdate(const RasterProgramUpdate&) = delete;
        RasterProgramUpdate& operator=(const RasterProgramUpdate&) = delete;

        struct PendingProgram final
        {
            RasterProgramLink link;
            ResourceProgramSource source;
        };

        struct Stream final
        {
            std::uint32_t pass;
            std::vector<PendingProgram> programs;
            std::unique_ptr<DynamicRasterPrograms> candidate;
        };

        struct Selection final
        {
            std::uint32_t producer;
            std::size_t first_revision, count;
        };

        [[nodiscard]] exec::task<RenderResult<void>> compile(execution::CpuScheduler) noexcept;
        [[nodiscard]] bool publishReturnedUses() noexcept;

        DeviceSession& device;
        const ResourceRegistry& registry;
        const ExecutionBindingId binding;
        std::vector<std::uint64_t> revisions;
        struct Publication final
        {
            ResourceProgramSource source;
            NativeProgramId program;
        };

        std::vector<Publication> pending_publications;
        std::vector<Selection> selections;
        std::vector<Stream> streams;
        stdexec::inplace_stop_source stop;
        std::optional<RenderError> returned;
#ifdef LUX_RENDER_DEVELOPMENT_METRICS
        std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
#endif
    };
} // namespace lux::render::detail
