#include <lux/engine/input/detail/GlfwInputTranslation.hpp>

#include <cassert>
#include <cstdint>
#include <vector>

int main()
{
    using namespace lux::input;
    using namespace lux::window;

    const std::vector<WindowInputEvent> events{
        WindowKeyEvent{-1, 1, GLFW_PRESS, 0},
        WindowKeyEvent{GLFW_KEY_SPACE, 2, GLFW_PRESS, GLFW_MOD_SHIFT},
        WindowKeyEvent{GLFW_KEY_SPACE, 2, GLFW_REPEAT, GLFW_MOD_SHIFT},
        WindowKeyEvent{GLFW_KEY_SPACE, 2, GLFW_RELEASE, GLFW_MOD_SHIFT},
        WindowMouseButtonEvent{GLFW_MOUSE_BUTTON_LEFT, GLFW_PRESS, 0},
        WindowMouseButtonEvent{GLFW_MOUSE_BUTTON_LEFT, GLFW_RELEASE, 0},
        WindowScrollEvent{1.5, -2.0},
        WindowTextEvent{0x4E2Du},
    };

    InputSnapshot snapshot;
    detail::applyGlfwWindowEvents(snapshot, events);

    assert(snapshot.events.size() == 7u);
    assert(snapshot.text_events.size() == 1u);
    assert(snapshot.text_events.front().codepoint == 0x4E2Du);
    assert(snapshot.isKeyJustPressed(EKey::KEY_SPACE));
    assert(snapshot.isKeyJustReleased(EKey::KEY_SPACE));
    assert(!snapshot.isKeyHeld(EKey::KEY_SPACE));
    assert(!snapshot.isKeyHeld(EKey::UNKNOWN));
    assert(snapshot.isMouseButtonJustPressed(EMouseButton::MOUSE_BUTTON_LEFT));
    assert(snapshot.isMouseButtonJustReleased(EMouseButton::MOUSE_BUTTON_LEFT));
    assert(!snapshot.isMouseButtonHeld(EMouseButton::MOUSE_BUTTON_LEFT));
    assert(snapshot.scroll_dx == 1.5);
    assert(snapshot.scroll_dy == -2.0);

    const auto* unknown = std::get_if<KeyAction>(&snapshot.events[0]);
    const auto* press = std::get_if<KeyAction>(&snapshot.events[1]);
    const auto* repeat = std::get_if<KeyAction>(&snapshot.events[2]);
    const auto* release = std::get_if<KeyAction>(&snapshot.events[3]);
    assert(unknown && unknown->key == EKey::UNKNOWN);
    assert(press && press->state == EInputState::PRESS);
    assert(press->modifier == EKeyModifier::KEY_MOD_SHIFT);
    assert(repeat && repeat->state == EInputState::REPEAT);
    assert(release && release->state == EInputState::RELEASE);

    return 0;
}
