#pragma once
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <lux/engine/ui/UpdateStatistics.hpp>

namespace lux::editor
{
    // Owner-thread snapshot. Durations/counters accumulate since construction; ui and scenes
    // describe the latest iteration. total excludes native event wait, reported separately.
    // No recorder, histogram or callbacks are retained by the host.
    struct FrameStatistics final
    {
        std::uint64_t iterations{}, captured_frames{}, waits{}, object_messages{};
        std::uint64_t backpressured_iterations{}, backpressure_waits{};
        std::chrono::nanoseconds total{}, execution_collect{}, task_dispatch{}, object_dispatch{};
        std::chrono::nanoseconds ui_maintenance{}, ui_draw{}, ui_capture{}, ui_publish{}, scene_drive{}, wait{};
        ui::UpdateStatistics ui;
        std::size_t scenes{};
    };
} // namespace lux::editor
