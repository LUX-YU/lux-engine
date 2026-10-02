#pragma once
#include <lux/engine/object/ObjectDispatcher.hpp>
#include <lux/engine/ui/Pane.hpp>
#include <lux/engine/ui/Element.hpp>
#include <lux/engine/ui/Root.hpp>
#include <utility>
#include "UiTestContent.hpp"

// Test content uses the same root-owned drawing boundary as production Panes.
template <class Draw> class TUiDrawPane final : public lux::ui::Pane
{
public:
    TUiDrawPane(lux::ui::Root& root, Draw draw)
        : Pane(root.dispatcherRef(), lux::ui::PaneId{"test.draw"}, lux::ui::PaneTypeId{"test.draw"}, "Draw"),
          content_(*this, std::move(draw))
    {
        setContent(content_);
        ui_test::mount(root, *this);
    }

private:
    class Content final : public lux::ui::Element
    {
    public:
        Content(TUiDrawPane& parent, Draw draw)
            : lux::ui::Element(parent, lux::ui::ElementId{"content"}), draw_(std::move(draw))
        {}

    private:
        void draw() noexcept override
        {
            draw_();
        }
        Draw draw_;
    };
    Content content_;
};
