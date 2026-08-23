#pragma once

#include <cstdint>
#include <variant>

namespace lux::ui
{
    enum class EUiPointerButton : std::uint8_t
    {
        LEFT,
        RIGHT,
        MIDDLE,
        EXTRA_1,
        EXTRA_2
    };

    enum class EUiKey : std::uint16_t
    {
        NONE,
        TAB,
        LEFT_ARROW,
        RIGHT_ARROW,
        UP_ARROW,
        DOWN_ARROW,
        PAGE_UP,
        PAGE_DOWN,
        HOME,
        END,
        INSERT,
        DELETE_KEY,
        BACKSPACE,
        SPACE,
        ENTER,
        ESCAPE,
        A,
        C,
        V,
        X,
        Y,
        Z,
        LEFT_CTRL,
        LEFT_SHIFT,
        LEFT_ALT,
        LEFT_SUPER,
        RIGHT_CTRL,
        RIGHT_SHIFT,
        RIGHT_ALT,
        RIGHT_SUPER
    };

    struct UiPointerMove final
    {
        float x{0.0F};
        float y{0.0F};
    };

    struct UiPointerButton final
    {
        EUiPointerButton button{EUiPointerButton::LEFT};
        bool down{false};
    };

    struct UiPointerWheel final
    {
        float horizontal{0.0F};
        float vertical{0.0F};
    };

    struct UiKey final
    {
        EUiKey key{EUiKey::NONE};
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

    using UiInputEvent = std::variant<
        UiPointerMove,
        UiPointerButton,
        UiPointerWheel,
        UiKey,
        UiText,
        UiWindowFocus>;
} // namespace lux::ui
