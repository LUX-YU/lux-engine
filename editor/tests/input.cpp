#include <cassert>
#include <lux/engine/input/InputSnapshot.hpp>
#include <lux/engine/ui/Root.hpp>
#include <lux/engine/ui/WindowInput.hpp>
using namespace lux;
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
    assert(ui::feedWindowInput(root, snapshot));
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
    assert(ui::feedWindowInput(root, composition));
    assert(root.update({{640, 480}, 0.016F}, data, ui::Root::Capture{capture}));
    assert(root.inputSnapshot().composing && root.inputSnapshot().keyboard_captured);
}
