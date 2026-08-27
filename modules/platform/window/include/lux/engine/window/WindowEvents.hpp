#pragma once

#include <cstdint>
#include <filesystem>
#include <variant>
#include <vector>

namespace lux::window
{
    struct WindowResizeEvent
    {
        std::uint32_t width{0};
        std::uint32_t height{0};
    };

    struct FramebufferResizeEvent
    {
        std::uint32_t width{0};
        std::uint32_t height{0};
    };

    struct WindowCloseEvent
    {
    };
    struct WindowFocusEvent
    {
    };
    struct WindowLostFocusEvent
    {
    };
    struct WindowMovedEvent
    {
    };
    struct WindowMinimizedEvent
    {
    };
    struct CursorEnterEvent
    {
    };
    struct CursorLeaveEvent
    {
    };

    struct CursorMoveEvent
    {
        double x{0.0};
        double y{0.0};
    };

    // Backend-native facts. Window records them without assigning Input
    // semantics; the Function Input platform source performs translation.
    struct WindowKeyEvent
    {
        int key{0};
        int scancode{0};
        int action{0};
        int modifiers{0};
    };

    struct WindowMouseButtonEvent
    {
        int button{0};
        int action{0};
        int modifiers{0};
    };

    struct WindowScrollEvent
    {
        double x{0.0};
        double y{0.0};
    };

    struct WindowTextEvent
    {
        std::uint32_t codepoint{0};
    };

    using WindowInputEvent = std::variant<WindowKeyEvent, WindowMouseButtonEvent, WindowScrollEvent, WindowTextEvent>;

    struct DrawReadyEvent
    {
    };
    struct DrawFinishedEvent
    {
    };

    struct FileDropEvent
    {
        std::vector<std::filesystem::path> paths;
    };
}
