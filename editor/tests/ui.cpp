#include "../../cmake/installed-consumers/common/UiTestContent.hpp"
#include "api_contract.hpp"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <lux/engine/input/Input.hpp>
#include <lux/engine/input/InputSnapshot.hpp>
#include <lux/engine/ui/Controls.hpp>
#include <lux/engine/ui/Layout.hpp>
#include <lux/engine/ui/Pane.hpp>
#include <lux/engine/ui/Root.hpp>

using namespace lux;

static_assert(!api_contract::PublicPaneOwners<ui::Root>);
static_assert(!api_contract::PublicPendingDrain<ui::Root>);
static_assert(!api_contract::PublicRuntimePaneId<ui::Pane>);
static_assert(!std::is_constructible_v<object::LuxObject, object::LuxObject*>);
static_assert(std::is_default_constructible_v<ui::Layout>);

namespace
{
    class Pane final : public ui::Pane
    {
    public:
        Pane(std::string name, unsigned& count) : ui::Pane(name), destroyed_(count), layout_{}, text_("Content")
        {
            assert(layout_.addElement(text_) && addElement(layout_));
        }
        ~Pane() override
        {
            ++destroyed_;
        }

    private:
        unsigned& destroyed_;
        ui::Layout layout_;
        ui::Label text_;
    };
    class Listener final : public object::LuxObject
    {
    public:
        Listener(ui::Root& root) : LuxObject(), root_(root) {}
        void changed(const ui::PaneChanged&) noexcept
        {
            ++calls;
            auto blocked = root_.clearPanes();
            assert(!blocked && blocked.error() == ui::EPaneError::BUSY);
        }
        ui::Root& root_;
        unsigned calls{};
    };
} // namespace
int main()
{
    auto& queue = object::ObjectRuntime::instance();
    auto created = ui::Root::create();
    assert(created);
    auto& root = **created;
    Listener listener(root);
    auto connection = object::LuxObject::connect(&root, &ui::Root::paneChanged, &listener, &Listener::changed);
    assert(connection);
    unsigned destroyed{};
    std::unique_ptr<ui::Pane> a = std::make_unique<Pane>("a", destroyed);
    auto* address = a.get();
    auto adopted = root.addPane(std::move(a));
    assert(adopted && !a && &adopted->get() == address && ui_test::paneCount(root) == 1);
    const auto identity = address->objectId();
    std::unique_ptr<ui::Pane> duplicate = std::make_unique<Pane>("a", destroyed);
    assert(root.addPane(std::move(duplicate)) && !duplicate && ui_test::paneCount(root) == 2);
    std::vector<std::unique_ptr<ui::Pane>> batch;
    batch.push_back(std::make_unique<Pane>("b", destroyed));
    batch.push_back({});
    auto* candidate = batch[0].get();
    assert(!root.addPanes(batch) && batch[0].get() == candidate && ui_test::paneCount(root) == 2);
    batch.clear();
    assert(root.requestFocus(*address) && root.capturePointer(*address));
    auto removed = root.removePane(*address);
    assert(removed && !address->attachedRoot() && !root.focusedPane());
    assert(ui_test::paneCount(root) == 1 && destroyed == 1);
    removed->reset();
    assert(destroyed == 2);
    std::unique_ptr<ui::Pane> next = std::make_unique<Pane>("a", destroyed);
    auto next_pane = root.addPane(std::move(next));
    assert(
        next_pane && !object::ObjectRuntime::instance().resolve(identity) && next_pane->get().attachedRoot() == &root
    );
    auto capture = [&](const ui::DrawData&) noexcept -> cxx::expected<void, ui::ECaptureError>
    {
        auto blocked = root.clearPanes();
        assert(!blocked && blocked.error() == ui::EPaneError::BUSY);
        return {};
    };
    ui::DrawData data;
    assert(root.feedInput(ui::WindowFocus{true}, 1));
    assert(root.feedInput(ui::PointerMove{{20, 20}}, 2));
    assert(root.feedInput(ui::Text{0x4e2d}, 3));
    assert(root.update({{640, 480}, 0.016F}, data, ui::Root::Capture{capture}));
    const auto sequence = root.inputSnapshot().sequence;
    assert(sequence <= 3);
    assert(root.update());
    assert(root.inputSnapshot().sequence == sequence);
    for (unsigned frame{}; root.inputSnapshot().sequence < 3 && frame < 8; ++frame)
    {
        assert(root.update({{640, 480}, 0.016F}, data, ui::Root::Capture{capture}));
    }
    assert(root.inputSnapshot().sequence == 3);
    assert(root.feedInput(ui::Composition{ui::ECompositionStage::STARTED}, 4));
    assert(root.update({{640, 480}, 0.016F}, data, ui::Root::Capture{capture}));
    assert(root.inputSnapshot().composing && root.inputSnapshot().keyboard_captured);
    input::Input actions;
    const auto action = actions.actionRegistry().registerAction({.name = "framework.shortcut"});
    input::InputContext shortcuts{"framework"};
    shortcuts.actionMap().bindKey(action, input::EKey::KEY_A);
    actions.contexts().push(&shortcuts);
    input::InputSnapshot key;
    key.keys_held.set(static_cast<std::size_t>(input::EKey::KEY_A));
    key.keys_just_pressed = key.keys_held;
    actions.evaluate(key, 0.016F, !root.inputSnapshot().keyboard_captured);
    assert(!actions.mapper().active(action));
    actions.evaluate(key, 0.016F, true);
    assert(actions.mapper().active(action));
    assert(root.clearPanes() && destroyed == 4 && listener.calls == 6);
    assert((ui_test::paneCount(root) == 0));
    std::puts(
        "PASS ownership, atomic refusal, stable addresses/handles, routing removal, callback guard and input sequence"
    );
}
