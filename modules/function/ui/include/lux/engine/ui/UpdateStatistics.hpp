#pragma once
#include <chrono>
#include <cstddef>

namespace lux::ui
{
    // Last update only. Counts are taken during the existing traversals, including hidden maintenance.
    // draw includes layout/ImGui render; capture includes copying + the synchronous resource pin callback.
    struct UpdateStatistics final
    {
        std::chrono::nanoseconds maintenance{}, draw{}, capture{};
        std::size_t panes{}, elements{};
        std::size_t draw_lists{}, draw_commands{}, draw_vertices{}, draw_indices{}, textures{};
        bool captured{};
    };
} // namespace lux::ui
