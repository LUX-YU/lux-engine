#pragma once

#include <cstddef>
#include <span>
#include <lux/engine/ui/Ids.hpp>
#include <vector>
#include <lux/cxx/compile_time/expected.hpp>
#include <lux/engine/function/visibility.h>
#include <lux/engine/ui/Geometry.hpp>
#include <memory>
#include <cstdint>

namespace lux::ui
{
    struct DockLayout final
    {
        PaneId left, center, right, bottom;
        float left_width{260}, right_width{350}, bottom_height{200};
        PaneId toolbar;
    };

    enum class EDockError
    {
        INVALID_DATA
    };

    // Window placement only. Persistent layout/source identities belong to the caller.
    enum class EDockSplit : std::uint8_t
    {
        LEAF,
        HORIZONTAL,
        VERTICAL
    };
    struct DockNode final
    {
        EDockSplit split{EDockSplit::LEAF};
        std::uint32_t first{UINT32_MAX}, second{UINT32_MAX};
        float ratio{0.5F};
        std::vector<PaneId> windows;
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
    class LUX_FUNCTION_PUBLIC PreparedDockTree final
    {
    public:
        ~PreparedDockTree();
        PreparedDockTree(PreparedDockTree&&) noexcept;
        PreparedDockTree& operator=(PreparedDockTree&&) noexcept;
        PreparedDockTree(const PreparedDockTree&) = delete;
        PreparedDockTree& operator=(const PreparedDockTree&) = delete;

    private:
        friend class Root;
        struct Data;
        explicit PreparedDockTree(std::unique_ptr<Data>) noexcept;
        std::unique_ptr<Data> data_;
    };

}
