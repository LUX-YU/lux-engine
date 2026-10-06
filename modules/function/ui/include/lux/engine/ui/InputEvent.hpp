#pragma once

#include <variant>
#include <array>
#include <cstdint>

#include <lux/engine/ui/Geometry.hpp>

namespace lux::ui
{
    enum class EInputError
    {
        FULL,
        CLOSED,
        INVALID_INPUT,
    };

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

    struct InputSnapshot final
    {
        std::array<bool, static_cast<std::size_t>(EKey::COUNT)> held{};
        std::array<bool, static_cast<std::size_t>(EKey::COUNT)> pressed{};
        std::array<bool, 3> buttons{};
        Vec2 pointer_delta{};
        Vec2 wheel{};
        bool window_focused{};
        bool keyboard_blocked{};
        bool keyboard_captured{};
        bool pointer_captured{};
        bool modal_open{};
        bool composing{};
        std::uint64_t sequence{}; // Last native input adopted by this UI.
    };
    struct PointerMove final
    {
        Point position;
    };

    struct PointerButton final
    {
        EPointerButton button{EPointerButton::LEFT};
        bool down{false};
    };

    struct PointerWheel final
    {
        Vec2 delta;
    };

    struct Key final
    {
        EKey key{EKey::NONE};
        bool down{false};
    };

    struct Text final
    {
        char32_t codepoint{0};
    };

    struct WindowFocus final
    {
        bool focused{false};
    };

    // Capture was revoked by a modal, hidden content or focus loss.
    struct PointerCancel final
    {};

    enum class ECompositionStage : std::uint8_t
    {
        STARTED,
        UPDATED,
        COMMITTED,
        CANCELLED
    };
    // Status only. Committed characters have one path: Text events.
    struct Composition final
    {
        ECompositionStage stage;
    };

    using VInputEvent =
        std::variant<PointerMove, PointerButton, PointerWheel, Key, Text, WindowFocus, PointerCancel, Composition>;
} // namespace lux::ui
