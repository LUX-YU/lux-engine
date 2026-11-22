#pragma once
#include <lux/engine/render/device/JobGpuResources.hpp>

#include <lux/engine/render/graph/execute/RenderGraphInstance.hpp>
#include <lux/engine/render/extensions/device/SubmissionFrontiers.hpp>

namespace lux::render::detail
{
    struct RenderGraphInstance::HistoryState final
    {
        struct View final
        {
            GraphResourceId resource;
            std::uint32_t pass, index;
            EImageViewTarget target;
        };

        [[nodiscard]] bool select(const DeviceSession&) noexcept;
        void publish() noexcept;

        const HistoryPlan* plan = nullptr;
        std::span<JobHistoryResources::Slot> slots;
        std::vector<std::uint64_t> sample_sequences;
        std::vector<View> views;
        std::uint64_t sequence = 0;
        std::uint32_t retained_samples = 0;
        std::uint32_t latest = kGraphInvalidIndex;
        std::uint32_t previous = 0, current = 1;
        bool invalidate = false;
        bool clear_previous = true, clear_current = true;
    };

} // namespace lux::render::detail
