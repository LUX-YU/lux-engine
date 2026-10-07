#pragma once

#include <lux/engine/object/ObjectRuntime.hpp>
#include <lux/engine/ui/Ids.hpp>

namespace lux::ui
{
    class Root;

    // Non-owning identity of one registration, including the originating Root generation.
    // A non-null handle may be stale; only Root::resolvePane establishes a synchronous borrow.
    class PaneHandle final
    {
    public:
        constexpr PaneHandle() noexcept = default;
        [[nodiscard]] constexpr bool isNull() const noexcept
        {
            return root_.isNull() || pane_.isNull();
        }
        friend constexpr bool operator==(const PaneHandle&, const PaneHandle&) noexcept = default;

    private:
        friend class Root;
        constexpr PaneHandle(object::ObjectId root, PaneId pane) noexcept : root_(root), pane_(pane) {}
        object::ObjectId root_;
        PaneId pane_;
    };
} // namespace lux::ui
