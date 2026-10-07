#include <cassert>
#include <iostream>
#include <lux/engine/editor/detail/ProjectUiMount.hpp>
#include <lux/engine/ui/Pane.hpp>
#include <lux/engine/ui/Root.hpp>
#include <type_traits>

using namespace lux;
static_assert(!std::is_copy_constructible_v<editor::detail::ProjectUiMount>);
static_assert(std::is_nothrow_move_constructible_v<editor::detail::ProjectUiMount>);

namespace
{
    class Pane final : public ui::Pane
    {
    public:
        explicit Pane(unsigned& destroyed) : ui::Pane("Project"), destroyed_(destroyed) {}
        ~Pane() override
        {
            ++destroyed_;
        }

    private:
        unsigned& destroyed_;
    };
    void mount(ui::Root& root, editor::detail::ProjectUiMount& responsibility, unsigned& destroyed)
    {
        responsibility.prepare(root, 1);
        std::unique_ptr<ui::Pane> candidate = std::make_unique<Pane>(destroyed);
        auto arm = [&](std::span<const ui::PaneHandle> added) noexcept { responsibility.arm(added); };
        auto result = root.replacePanes({}, std::span{&candidate, 1}, arm);
        assert(result && result->empty());
    }
} // namespace
int main()
{
    auto made = ui::Root::create({.docking = false});
    assert(made);
    auto& root = **made;
    auto global = root.addPane(std::make_unique<ui::Pane>("Global"));
    assert(global);
    const auto global_handle = root.paneHandle(global->get());
    unsigned destroyed{};
    {
        editor::detail::ProjectUiMount a;
        mount(root, a, destroyed);
        const auto stale = a.handles().front();
        auto extracted = root.removePane(*root.resolvePane(stale));
        assert(extracted);
        extracted->reset();
        auto reused = root.addPane(std::make_unique<ui::Pane>("Unrelated"));
        assert(reused);
        const auto reused_handle = root.paneHandle(reused->get());
        {
            auto moved = std::move(a);
            assert(a.handles().empty());
        }
        assert(destroyed == 1 && root.resolvePane(reused_handle) && root.resolvePane(global_handle));
        editor::detail::ProjectUiMount b, c;
        mount(root, b, destroyed);
        mount(root, c, destroyed);
        const auto latest = c.handles().front();
        b = std::move(c);
        assert(destroyed == 2 && c.handles().empty() && root.resolvePane(latest));
    }
    assert(destroyed == 3 && root.resolvePane(global_handle));
    {
        editor::detail::ProjectUiMount prepared_only;
        prepared_only.prepare(root, 8);
    }
    assert(root.resolvePane(global_handle));
    std::cout << "PASS: unique unmount responsibility, move replacement, stale/reused handles and globals\n";
}
