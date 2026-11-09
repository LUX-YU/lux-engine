#pragma once
#include <cstdint>
#include <lux/engine/ui/Geometry.hpp>
#include <lux/engine/ui/PaneHandle.hpp>
#include <vector>

namespace lux::ui
{
    enum class EDockError : std::uint8_t
    {
        INVALID_DATA,
        BUSY,
        WRONG_THREAD,
        DISABLED
    };
    enum class EDockSplit : std::uint8_t
    {
        LEAF,
        HORIZONTAL,
        VERTICAL
    };

    // Copyable runtime layout. Handles never retain or dereference a former Pane address.
    struct DockNode final
    {
        EDockSplit split{EDockSplit::LEAF};
        std::uint32_t first{UINT32_MAX}, second{UINT32_MAX};
        float ratio{0.5F};
        std::vector<PaneHandle> panes;
    };

    struct DockSurface final
    {
        std::uint32_t node{};
        Rect bounds;
        bool floating{};
    };

    struct AddedPanePlacement final
    {
        std::uint32_t node;
        std::uint32_t added_pane;
    };

    struct DockTree final
    {
        std::vector<DockNode> nodes;
        std::vector<DockSurface> surfaces;
    };

    // Complete post-replacement layout. New panes are referenced by their index in replacePanes(add).
    struct PanePlacement final
    {
        DockTree tree;
        std::vector<AddedPanePlacement> added;
    };
} // namespace lux::ui
