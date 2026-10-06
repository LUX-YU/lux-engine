#include <algorithm>
#include <cassert>
#include <cstdio>
#include <lux/engine/editor/EditorUIRoot.hpp>
#include <lux/engine/editor/WindowInput.hpp>
#include <lux/engine/input/Input.hpp>
#include <lux/engine/input/InputSnapshot.hpp>
#include <lux/engine/ui/Controls.hpp>
#include <lux/engine/ui/Layout.hpp>
#include <lux/engine/ui/Pane.hpp>

using namespace lux;
using namespace lux::editor;
namespace
{
    class Pane final : public ui::Pane
    {
    public:
        Pane(object::ObjectDispatcherRef queue, std::string name, unsigned& count)
            : ui::Pane(queue, ui::PaneId{name}, ui::PaneTypeId{"test"}, name), destroyed_(count),
              layout_(*this, ui::ElementId{"content"}), text_(layout_, ui::ElementId{"text"}, "Content")
        {
            assert(setContent(layout_));
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
        Listener(EditorUIRoot& root) : LuxObject(root.dispatcherRef()), root_(root) {}
        void changed(const ui::AttachmentChanged&) noexcept
        {
            ++calls;
            auto blocked = root_.clearProjectUi();
            assert(!blocked && blocked.error().code == EFrameworkError::BUSY);
        }
        EditorUIRoot& root_;
        unsigned calls{};
    };
} // namespace
int main()
{
    auto queue = object::ObjectMessageQueue::create(128);
    assert(queue);
    auto created = EditorUIRoot::create(queue->dispatcherRef());
    assert(created);
    auto& root = **created;
    Listener listener(root);
    auto connection = object::LuxObject::connect(&root, &ui::Root::attachmentChanged, &listener, &Listener::changed);
    assert(connection);
    unsigned destroyed{};
    std::unique_ptr<ui::Pane> a = std::make_unique<Pane>(queue->dispatcherRef(), "a", destroyed);
    auto* address = a.get();
    auto handle = root.takePane(a);
    assert(handle && !a && root.projectPane(*handle) == address && root.projectPaneCount() == 1);
    assert(address->ownership() == object::EObjectOwnership::EXTERNAL);
    std::unique_ptr<ui::Pane> duplicate = std::make_unique<Pane>(queue->dispatcherRef(), "a", destroyed);
    assert(!root.takePane(duplicate) && duplicate && root.projectPaneCount() == 1);
    duplicate.reset();
    std::vector<std::unique_ptr<ui::Pane>> batch;
    batch.push_back(std::make_unique<Pane>(queue->dispatcherRef(), "b", destroyed));
    batch.push_back(std::make_unique<Pane>(queue->dispatcherRef(), "a", destroyed));
    assert(!root.mountProjectUi(batch) && batch[0] && batch[1] && root.projectPaneCount() == 1);
    batch.clear();
    assert(root.requestFocus(*address));
    assert(root.capturePointer(*address));
    assert(root.removePane(*handle));
    assert(!root.projectPane(*handle) && !root.findPane(ui::PaneIdView{"a"}) && !root.focusedPane());
    assert(root.projectPaneCount() == 0 && destroyed == 4);
    std::unique_ptr<ui::Pane> next = std::make_unique<Pane>(queue->dispatcherRef(), "a", destroyed);
    auto next_handle = root.takePane(next);
    assert(next_handle && !root.projectPane(*handle) && root.projectPane(*next_handle));
    auto capture = [&](const ui::DrawData&) noexcept -> cxx::expected<void, ui::ECaptureError>
    {
        auto blocked = root.clearProjectUi();
        assert(!blocked && blocked.error().code == EFrameworkError::BUSY);
        return {};
    };
    ui::DrawData data;
    input::InputSnapshot snapshot;
    snapshot.events.emplace_back(input::FocusAction{true, 1});
    snapshot.events.emplace_back(input::CursorAction{20, 20, 2});
    snapshot.events.emplace_back(input::CharInput{0x4e2d, 3});
    assert(feedWindowInput(root, snapshot));
    assert(root.frame({{640, 480}, 0.016F}, &data, capture));
    const auto sequence = root.inputSnapshot().sequence;
    assert(sequence <= 3);
    assert(root.frame({{640, 480}, 0.016F}, nullptr, capture));
    assert(root.inputSnapshot().sequence == sequence);
    for (unsigned frame{}; root.inputSnapshot().sequence < 3 && frame < 8; ++frame)
    {
        assert(root.frame({{640, 480}, 0.016F}, &data, capture));
    }
    assert(root.inputSnapshot().sequence == 3);
    input::InputSnapshot composition;
    composition.events.emplace_back(input::CompositionAction{input::ECompositionStage::STARTED, 4});
    assert(feedWindowInput(root, composition));
    assert(root.frame({{640, 480}, 0.016F}, &data, capture));
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
    assert(root.clearProjectUi() && destroyed == 5 && listener.calls == 4);
    assert(std::ranges::all_of(root.panes(), [](auto* pane) { return pane == nullptr; }));
    std::puts(
        "PASS ownership, atomic refusal, stable addresses/handles, routing removal, callback guard and input sequence"
    );
}
