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
    {};
    struct WindowFocusEvent
    {
        std::uint64_t sequence{};
    };
    struct WindowLostFocusEvent
    {
        std::uint64_t sequence{};
    };
    struct WindowMovedEvent
    {
        int x{}, y{};
    };
    struct WindowMinimizedEvent
    {
        bool minimized{};
    };
    struct WindowPlacementEvent
    {};
    struct CursorEnterEvent
    {};
    struct CursorLeaveEvent
    {};

    struct CursorMoveEvent
    {
        double x{0.0};
        double y{0.0};
        std::uint64_t sequence{};
    };

    // Backend-native facts. Window records them without assigning Input
    // semantics; the Function Input platform source performs translation.
    struct WindowKeyEvent
    {
        int key{0};
        int scancode{0};
        int action{0};
        int modifiers{0};
        std::uint64_t sequence{};
    };

    struct WindowMouseButtonEvent
    {
        int button{0};
        int action{0};
        int modifiers{0};
        std::uint64_t sequence{};
    };

    struct WindowScrollEvent
    {
        double x{0.0};
        double y{0.0};
        std::uint64_t sequence{};
    };

    struct WindowTextEvent
    {
        std::uint32_t codepoint{0};
        std::uint64_t sequence{};
    };

    enum class ECompositionStage : std::uint8_t
    {
        STARTED,
        UPDATED,
        COMMITTED,
        CANCELLED
    };
    struct WindowCompositionEvent
    {
        ECompositionStage stage;
        std::uint64_t sequence{};
    };

    using VWindowInputEvent = std::variant<
        WindowKeyEvent,
        WindowMouseButtonEvent,
        WindowScrollEvent,
        WindowTextEvent,
        CursorMoveEvent,
        WindowFocusEvent,
        WindowLostFocusEvent,
        WindowCompositionEvent>;

    struct DrawReadyEvent
    {};
    struct DrawFinishedEvent
    {};

    struct FileDropEvent
    {
        std::vector<std::filesystem::path> paths;
    };
}
