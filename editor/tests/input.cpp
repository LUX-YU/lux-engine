#include <cassert>
#include <lux/engine/editor/detail/WindowInput.hpp>
#include <lux/engine/input/InputSnapshot.hpp>
#include <lux/engine/ui/Root.hpp>
using namespace lux;
using namespace lux::editor;
int main()
{
    auto created = ui::Root::create();
    assert(created);
    auto& root = **created;
    ui::DrawData data;
    auto capture = [](const ui::DrawData&) noexcept -> cxx::expected<void, ui::ECaptureError> { return {}; };
    input::InputSnapshot snapshot;
    snapshot.events.emplace_back(input::FocusAction{true, 1});
    snapshot.events.emplace_back(input::CursorAction{20, 20, 2});
    snapshot.events.emplace_back(input::CharInput{0x4e2d, 3});
    assert(feedWindowInput(root, snapshot));
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
    input::InputSnapshot composition;
    composition.events.emplace_back(input::CompositionAction{input::ECompositionStage::STARTED, 4});
    assert(feedWindowInput(root, composition));
    assert(root.update({{640, 480}, 0.016F}, data, ui::Root::Capture{capture}));
    assert(root.inputSnapshot().composing && root.inputSnapshot().keyboard_captured);
}
