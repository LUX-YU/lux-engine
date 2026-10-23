#pragma once
#include <chrono>
#include <cstddef>

namespace lux::ui
{
    // Last update only. Counts are taken during the existing traversals, including hidden maintenance.
    // draw includes layout/ImGui render; capture includes the one required draw-data freeze. Unique texture inspection
    // is opt-in through DrawData.
    struct UpdateStatistics final
    {
        std::chrono::nanoseconds maintenance{}, draw{}, capture{};
        std::size_t panes{}, elements{};
        std::size_t draw_lists{}, draw_commands{}, draw_vertices{}, draw_indices{};
        bool captured{};
    };
} // namespace lux::ui
