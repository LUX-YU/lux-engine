#pragma once
#include <lux/engine/ui/Element.hpp>
#include <lux/engine/ui/Pane.hpp>

// Test probes still run at the real content boundary, inside Root's ImGui scope.
template <class Owner> class TUiTestContent final : public lux::ui::Element
{
public:
    explicit TUiTestContent(Owner& owner) : lux::ui::Element(owner, lux::ui::ElementId{"probe-content"}), owner_(owner)
    {}

private:
    void draw() noexcept override
    {
        owner_.drawTestContent(*this);
    }
    Owner& owner_;
};

#include <lux/engine/ui/Root.hpp>
#include <cassert>
#include <type_traits>
namespace ui_test
{
    template <class Parent> decltype(auto) parent(Parent& value)
    {
        if constexpr (std::derived_from<Parent, lux::ui::Root>)
            return value.dispatcherRef();
        else
            return (value);
    }
    template <class Parent> void mount(Parent& parent, lux::ui::Pane& pane)
    {
        if constexpr (std::derived_from<Parent, lux::ui::Root>)
        {
            assert(!pane.attachedRoot());
            auto ready = parent.prepareMount(pane);
            assert(ready);
            assert(parent.commit(*ready));
        }
    }
}
