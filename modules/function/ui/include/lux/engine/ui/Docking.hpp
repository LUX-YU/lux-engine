#pragma once
#include <cstdint>
#include <lux/engine/ui/Geometry.hpp>
#include <vector>

namespace lux::ui
{
    class Pane;
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
    // Synchronous borrowed placement. Root retains registration identities, never these pointers.
    struct DockNode final
    {
        EDockSplit split{EDockSplit::LEAF};
        std::uint32_t first{UINT32_MAX}, second{UINT32_MAX};
        float ratio{0.5F};
        std::vector<Pane*> panes;
    };
    struct DockSurface final
    {
        std::uint32_t node{};
        Rect bounds;
        bool floating{};
    };
    struct DockTree final
    {
        std::vector<DockNode> nodes;
        std::vector<DockSurface> surfaces;
    };
} // namespace lux::ui
