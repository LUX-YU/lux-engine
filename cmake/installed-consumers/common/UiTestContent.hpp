#pragma once
#include <lux/engine/ui/Element.hpp>

// Test probes still run at the real content boundary, inside Root's ImGui scope.
template <class Owner> class TUiTestContent final : public lux::ui::Element
{
public:
    explicit TUiTestContent(Owner& owner)
        : lux::ui::Element(owner, lux::ui::ElementId{"probe-content"}),
          owner_(owner)
    {}

private:
    void draw() noexcept override
    {
        owner_.drawTestContent(*this);
    }
    Owner& owner_;
};
