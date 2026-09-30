#pragma once
#include <cstddef>
#include <span>
#include <vector>
namespace lux::editor::widgets
{
    struct TreeRow final
    {
        std::size_t source{}, depth{}, end{};
    };
    // Parent indices outside the span denote roots. Cycles/orphans remain visible exactly once.
    [[nodiscard]] std::vector<TreeRow> makeTreeRows(std::span<const std::size_t> parents);
}
