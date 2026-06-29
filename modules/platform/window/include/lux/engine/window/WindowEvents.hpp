#pragma once

#include <filesystem>
#include <vector>
#include <lux/engine/common/Size2D.hpp>
#include <lux/engine/window/LuxWindowDefination.hpp>

namespace lux::window
{
    // ── Window Events ────────────────────────────────────────────
    // Plain structs — the type itself is the identity (no manual ID).
    // Each satisfies the lux::event::Event concept (movable + destructible).

    struct WindowResizeEvent
    {
        common::Size2D size;
    };

    struct FramebufferResizeEvent
    {
        common::Size2D size;
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
