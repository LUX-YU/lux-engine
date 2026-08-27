#pragma once

#include <lux/engine/input/PhysicalInput.hpp>

#include <array>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <span>
#include <variant>
#include <vector>

namespace lux::input
{
    struct CharInput
    {
        std::uint32_t codepoint{0};
    };

    enum class ETouchPhase : std::uint8_t
    {
        BEGAN,
        MOVED,
        STATIONARY,
        ENDED,
        CANCELED,
    };

    struct TouchPoint
    {
        std::int32_t id{-1};
        ETouchPhase phase{ETouchPhase::ENDED};
        float x{0.0f};
        float y{0.0f};
        float dx{0.0f};
        float dy{0.0f};
    };

    struct TouchAction
    {
        std::int32_t id{-1};
        ETouchPhase phase{ETouchPhase::ENDED};
        float x{0.0f};
        float y{0.0f};
    };

    using InputEvent = std::variant<KeyAction, MouseButtonAction, MouseScrollAction, TouchAction>;

    struct InputSnapshot
    {
        static constexpr std::size_t kKeyboardBitCount = 512;
        static constexpr std::size_t kMaxTouchPoints = 10;

        std::bitset<kKeyboardBitCount> keys_held;
        std::bitset<kKeyboardBitCount> keys_just_pressed;
        std::bitset<kKeyboardBitCount> keys_just_released;

        std::uint8_t mouse_held{0};
        std::uint8_t mouse_just_pressed{0};
        std::uint8_t mouse_just_released{0};

        std::array<TouchPoint, kMaxTouchPoints> touches{};
        std::uint8_t touch_count{0};

        double cursor_x{0.0};
        double cursor_y{0.0};
        double cursor_dx{0.0};
        double cursor_dy{0.0};
        double scroll_dx{0.0};
        double scroll_dy{0.0};

        std::uint32_t window_width{0};
        std::uint32_t window_height{0};
        std::uint32_t framebuffer_width{0};
        std::uint32_t framebuffer_height{0};

        std::vector<InputEvent> events;
        std::vector<CharInput> text_events;

        bool keyboard_captured_by_ui{false};
        bool mouse_captured_by_ui{false};

        double sample_timestamp{0.0};
        float sample_dt{0.0f};

        [[nodiscard]] bool isKeyHeld(EKey key) const noexcept
        {
            const auto index = static_cast<std::size_t>(static_cast<int>(key));
            return index < keys_held.size() && keys_held[index];
        }

        [[nodiscard]] bool isKeyJustPressed(EKey key) const noexcept
        {
            const auto index = static_cast<std::size_t>(static_cast<int>(key));
            return index < keys_just_pressed.size() && keys_just_pressed[index];
        }

        [[nodiscard]] bool isKeyJustReleased(EKey key) const noexcept
        {
            const auto index = static_cast<std::size_t>(static_cast<int>(key));
            return index < keys_just_released.size() && keys_just_released[index];
        }

        [[nodiscard]] bool isMouseButtonHeld(EMouseButton button) const noexcept
        {
            const auto index = static_cast<int>(button);
            return index >= 0 && index < 8 && ((mouse_held >> index) & 1u) != 0;
        }

        [[nodiscard]] bool isMouseButtonJustPressed(EMouseButton button) const noexcept
        {
            const auto index = static_cast<int>(button);
            return index >= 0 && index < 8 && ((mouse_just_pressed >> index) & 1u) != 0;
        }

        [[nodiscard]] bool isMouseButtonJustReleased(EMouseButton button) const noexcept
        {
            const auto index = static_cast<int>(button);
            return index >= 0 && index < 8 && ((mouse_just_released >> index) & 1u) != 0;
        }

        [[nodiscard]] std::span<const TouchPoint> activeTouches() const noexcept
        {
            return {touches.data(), touch_count};
        }

        [[nodiscard]] const TouchPoint* primaryTouch() const noexcept
        {
            return touch_count == 0 ? nullptr : &touches[0];
        }

        [[nodiscard]] bool anyTouchDown() const noexcept
        {
            for (std::uint8_t index = 0; index < touch_count; ++index)
            {
                const auto phase = touches[index].phase;
                if (phase == ETouchPhase::BEGAN || phase == ETouchPhase::MOVED || phase == ETouchPhase::STATIONARY)
                {
                    return true;
                }
            }
            return false;
        }

        [[nodiscard]] bool anyTouchJustBegan() const noexcept
        {
            for (std::uint8_t index = 0; index < touch_count; ++index)
            {
                if (touches[index].phase == ETouchPhase::BEGAN)
                {
                    return true;
                }
            }
            return false;
        }

        [[nodiscard]] bool allTouchesJustEnded() const noexcept
        {
            bool saw_end = false;
            for (std::uint8_t index = 0; index < touch_count; ++index)
            {
                const auto phase = touches[index].phase;
                if (phase == ETouchPhase::ENDED || phase == ETouchPhase::CANCELED)
                {
                    saw_end = true;
                }
                else
                {
                    return false;
                }
            }
            return saw_end;
        }
    };
}
