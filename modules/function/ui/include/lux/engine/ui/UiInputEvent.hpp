#pragma once

// Platform-neutral adapter seam into this ImGui-based UI foundation.

#include <variant>

#include <imgui.h>

namespace lux::ui
{
    struct UiPointerMove final
    {
        float x{0.0F};
        float y{0.0F};
    };

    struct UiPointerButton final
    {
        ImGuiMouseButton button{ImGuiMouseButton_Left};
        bool down{false};
    };

    struct UiPointerWheel final
    {
        float horizontal{0.0F};
        float vertical{0.0F};
    };

    struct UiKey final
    {
        ImGuiKey key{ImGuiKey_None};
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

    using UiInputEvent =
        std::variant<UiPointerMove, UiPointerButton, UiPointerWheel, UiKey, UiText, UiWindowFocus>;
} // namespace lux::ui
