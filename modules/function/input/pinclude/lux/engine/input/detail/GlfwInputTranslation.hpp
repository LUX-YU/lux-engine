#pragma once

#include <lux/engine/input/InputSnapshot.hpp>
#include <lux/engine/window/WindowEvents.hpp>

#include <GLFW/glfw3.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <variant>

namespace lux::input::detail
{
    [[nodiscard]] inline EKey glfwKey(int key) noexcept
    {
        if (key < 0 || key >= static_cast<int>(InputSnapshot::kKeyboardBitCount))
        {
            return EKey::UNKNOWN;
        }
        return static_cast<EKey>(key);
    }

    [[nodiscard]] inline EMouseButton glfwMouseButton(int button) noexcept
    {
        if (button < GLFW_MOUSE_BUTTON_1 || button > GLFW_MOUSE_BUTTON_8)
        {
            return EMouseButton::UNKNOWN;
        }
        return static_cast<EMouseButton>(button);
    }

    [[nodiscard]] inline EInputState glfwInputState(int action) noexcept
    {
        switch (action)
        {
        case GLFW_PRESS:
            return EInputState::PRESS;
        case GLFW_RELEASE:
            return EInputState::RELEASE;
        case GLFW_REPEAT:
            return EInputState::REPEAT;
        default:
            return EInputState::UNKNOWN;
        }
    }

    [[nodiscard]] inline EKeyModifier glfwModifiers(int modifiers) noexcept
    {
        constexpr int kKnownModifiers =
            GLFW_MOD_SHIFT |
            GLFW_MOD_CONTROL |
            GLFW_MOD_ALT |
            GLFW_MOD_SUPER |
            GLFW_MOD_CAPS_LOCK |
            GLFW_MOD_NUM_LOCK;
        return static_cast<EKeyModifier>(modifiers & kKnownModifiers);
    }

    inline void applyGlfwWindowEvents(
        InputSnapshot& snapshot,
        std::span<const lux::window::WindowInputEvent> events
    )
    {
        for (const auto& raw : events)
        {
            if (const auto* key = std::get_if<lux::window::WindowKeyEvent>(&raw))
            {
                const EKey translated_key = glfwKey(key->key);
                const EInputState translated_state = glfwInputState(key->action);
                const auto index = static_cast<std::size_t>(key->key);
                if (key->key >= 0 && index < snapshot.keys_held.size())
                {
                    if (translated_state == EInputState::PRESS)
                    {
                        snapshot.keys_held.set(index);
                        snapshot.keys_just_pressed.set(index);
                    }
                    else if (translated_state == EInputState::RELEASE)
                    {
                        snapshot.keys_held.reset(index);
                        snapshot.keys_just_released.set(index);
                    }
                    else if (translated_state == EInputState::REPEAT)
                    {
                        snapshot.keys_held.set(index);
                    }
                }

                snapshot.events.emplace_back(KeyAction{
                    translated_key,
                    key->scancode,
                    translated_state,
                    glfwModifiers(key->modifiers)
                });
                continue;
            }

            if (const auto* mouse = std::get_if<lux::window::WindowMouseButtonEvent>(&raw))
            {
                const EMouseButton translated_button =
                    glfwMouseButton(mouse->button);
                const EInputState translated_state =
                    glfwInputState(mouse->action);
                if (mouse->button >= 0 && mouse->button < 8)
                {
                    const auto bit = static_cast<std::uint8_t>(
                        1u << mouse->button
                    );
                    if (translated_state == EInputState::PRESS)
                    {
                        snapshot.mouse_held |= bit;
                        snapshot.mouse_just_pressed |= bit;
                    }
                    else if (translated_state == EInputState::RELEASE)
                    {
                        snapshot.mouse_held = static_cast<std::uint8_t>(
                            snapshot.mouse_held & ~bit
                        );
                        snapshot.mouse_just_released |= bit;
                    }
                }

                snapshot.events.emplace_back(MouseButtonAction{
                    translated_button,
                    translated_state,
                    glfwModifiers(mouse->modifiers)
                });
                continue;
            }

            if (const auto* scroll = std::get_if<lux::window::WindowScrollEvent>(&raw))
            {
                snapshot.scroll_dx += scroll->x;
                snapshot.scroll_dy += scroll->y;
                snapshot.events.emplace_back(
                    MouseScrollAction{scroll->x, scroll->y}
                );
                continue;
            }

            if (const auto* text = std::get_if<lux::window::WindowTextEvent>(&raw))
            {
                snapshot.text_events.push_back(CharInput{text->codepoint});
            }
        }
    }
}
