#pragma once

#include <variant>

#include <lux/engine/ui/Geometry.hpp>

namespace lux::ui
{
    enum class EPointerButton
    {
        LEFT,
        MIDDLE,
        RIGHT,
    };

    enum class EKey
    {
        NONE,
        TAB,
        ENTER,
        ESCAPE,
        SPACE,
        BACKSPACE,
        DELETE_KEY,
        LEFT,
        RIGHT,
        UP,
        DOWN,
        HOME,
        END,
    };

    struct UiPointerMove final
    {
        Point position;
    };

    struct UiPointerButton final
    {
        EPointerButton button{EPointerButton::LEFT};
        bool down{false};
    };

    struct UiPointerWheel final
    {
        Vec2 delta;
    };

    struct UiKey final
    {
        EKey key{EKey::NONE};
        bool down{false};
    };

    struct UiText final
    {
        char32_t codepoint{0};
    };

    struct UiWindowFocus final
    {
        bool focused{false};
    };

    using UiInputEvent = std::variant<UiPointerMove, UiPointerButton, UiPointerWheel, UiKey, UiText, UiWindowFocus>;
} // namespace lux::ui
