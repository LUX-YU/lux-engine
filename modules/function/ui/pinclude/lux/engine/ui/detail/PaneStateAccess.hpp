#pragma once
#include <lux/engine/ui/Pane.hpp>
namespace lux::ui::detail
{
struct PaneStateAccess final
{
    static void setFocused(Pane &pane, bool value)
    {
        pane.setFocused(value);
    }
    static void setHovered(Pane &pane, bool value) noexcept
    {
        pane.setHovered(value);
    }
    static const char *windowLabel(const Pane &pane) noexcept
    {
        return pane.window_label_.c_str();
    }
    static PaneDrawContext context(std::vector<UiContextIdView> &values) noexcept
    {
        return PaneDrawContext(values);
    }
    static void draw(Pane &pane, Frame &frame, PaneDrawContext &context)
    {
        pane.draw(frame, context);
    }
};
} // namespace lux::ui::detail
