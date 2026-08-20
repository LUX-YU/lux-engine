#pragma once

#include <filesystem>
#include <vector>
#include <cstdint>
#include <lux/engine/window/LuxWindowDefination.hpp>

namespace lux::window
{
    // ── Window Events ────────────────────────────────────────────
    // Plain structs — the type itself is the identity (no manual ID).
    // Each satisfies the lux::event::Event concept (movable + destructible).

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
        double x;
        double y;
    };

    struct MouseButtonEvent
    {
        MouseButtonAction action;
    };

    struct MouseScrollEvent
    {
        MouseScrollAction action;
    };

    struct KeyEvent
    {
        KeyAction action;
    };

    struct DrawReadyEvent
    {
    };
    struct DrawFinishedEvent
    {
    };

    /// Emitted when the OS drops one or more files onto the window (GLFW drop
    /// callback). Paths are absolute. Fired on the main thread during
    /// glfwPollEvents(); subscribers should defer heavy work (e.g. asset
    /// import) rather than block the callback.
    struct FileDropEvent
    {
        std::vector<std::filesystem::path> paths;
    };

} // namespace lux::window
