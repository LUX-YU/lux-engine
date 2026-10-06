#pragma once
#include <lux/engine/ui/Pane.hpp>
#include <lux/engine/ui/Root.hpp>
namespace lux::ui::detail
{
    struct RootTestAccess final
    {
        static void apply(Root& root) noexcept
        {
            root.applyPendingChanges();
        }
        static PaneId id(const Pane& pane) noexcept
        {
            return pane.id_;
        }
        static Pane* find(Root& root, PaneId id) noexcept
        {
            return root.findPane(id);
        }
    };
} // namespace lux::ui::detail
namespace ui_test
{
    using lux::ui::detail::RootTestAccess;
    inline void apply(lux::ui::Root& root) noexcept
    {
        RootTestAccess::apply(root);
    }
    inline auto paneId(const lux::ui::Pane& pane) noexcept
    {
        return RootTestAccess::id(pane);
    }
    inline auto findPane(lux::ui::Root& root, lux::ui::PaneId id) noexcept
    {
        return RootTestAccess::find(root, id);
    }
} // namespace ui_test
