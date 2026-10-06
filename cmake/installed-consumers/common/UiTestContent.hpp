#pragma once
#include <lux/engine/ui/Element.hpp>
#include <lux/engine/ui/Pane.hpp>

// Test probes still run at the real content boundary, inside Root's ImGui scope.
template <class Owner> class TUiTestContent final : public lux::ui::Element
{
public:
    explicit TUiTestContent(Owner& owner) : lux::ui::Element(lux::ui::ElementId{"probe-content"}), owner_(owner)
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
    template <class T, class... Args> T& makePane(lux::ui::Root& root, Args&&... args)
    {
        std::unique_ptr<lux::ui::Pane> candidate = std::make_unique<T>(std::forward<Args>(args)...);
        auto* result = static_cast<T*>(candidate.get());
        auto adopted = root.addPane(std::move(candidate));
        assert(adopted && !candidate && &adopted->get() == result);
        return *result;
    }
}
