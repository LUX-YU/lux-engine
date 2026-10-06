#pragma once
#include <exception>
#include <lux/engine/ui/Pane.hpp>
#include <lux/engine/ui/Element.hpp>
#include <lux/engine/ui/Root.hpp>
#include <utility>
#include "UiTestContent.hpp"

// Test content uses the same root-owned drawing boundary as production Panes.
template <class Draw> class TUiDrawPane final : public lux::ui::Pane
{
public:
    TUiDrawPane(Draw draw)
        : Pane("Draw"),
          content_(*this, std::move(draw))
    {
        if (!addElement(content_))
            std::terminate(); // Fixed content in a detached Pane.
    }

private:
    class Content final : public lux::ui::Element
    {
    public:
        Content(TUiDrawPane& parent, Draw draw)
            : lux::ui::Element(lux::ui::ElementId{"content"}), draw_(std::move(draw))
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
