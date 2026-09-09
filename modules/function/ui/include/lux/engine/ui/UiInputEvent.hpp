#pragma once

#include <variant>
#include <array>

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
        A,
        B,
        C,
        D,
        E,
        F,
        G,
        H,
        I,
        J,
        K,
        L,
        M,
        N,
        O,
        P,
        Q,
        R,
        S,
        T,
        U,
        V,
        W,
        X,
        Y,
        Z,
        LEFT_SHIFT,
        RIGHT_SHIFT,
        LEFT_CONTROL,
        RIGHT_CONTROL,
        LEFT_ALT,
        RIGHT_ALT,
        COUNT,
    };

    struct UiInputSnapshot final
    {
        std::array<bool, static_cast<std::size_t>(EKey::COUNT)> held{};
        std::array<bool, static_cast<std::size_t>(EKey::COUNT)> pressed{};
        std::array<bool, 3> buttons{};
        Vec2 pointer_delta{};
        Vec2 wheel{};
        bool window_focused{};
        bool keyboard_blocked{};
        bool modal_open{};
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
