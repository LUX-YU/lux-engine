#pragma once
#include <lux/engine/render/device/JobGpuResources.hpp>

#include <lux/engine/render/graph/RenderGraph.hpp>
#include <lux/engine/render/extensions/device/SubmissionFrontiers.hpp>

namespace lux::render::detail
{
    struct HistoryState final
    {
        [[nodiscard]] bool select(const DeviceSession&) noexcept;
        void publish() noexcept;

        const HistoryPlan* plan = nullptr;
        std::span<JobHistoryResources::Slot> slots;
        std::vector<std::uint64_t> sample_sequences;
        std::uint64_t sequence = 0;
        std::uint32_t retained_samples = 0;
        std::uint32_t latest = kGraphInvalidIndex;
        std::uint32_t previous = 0, current = 1;
        bool invalidate = false;
        bool clear_previous = true, clear_current = true;
    };

} // namespace lux::render::detail
