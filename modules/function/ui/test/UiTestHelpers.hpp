#pragma once
#include <cassert>
#include <lux/engine/ui/Pane.hpp>
#include <lux/engine/ui/Root.hpp>
namespace ui_test
{
    inline void apply(lux::ui::Root& root) noexcept
    {
        assert(root.update());
    }
    inline auto paneHandle(const lux::ui::Pane& pane) noexcept
    {
        auto* root = pane.attachedRoot();
        return root ? root->paneHandle(pane) : lux::ui::PaneHandle{};
    }
    inline auto resolvePane(lux::ui::Root& root, lux::ui::PaneHandle handle) noexcept
    {
        return root.resolvePane(handle);
    }
} // namespace ui_test
