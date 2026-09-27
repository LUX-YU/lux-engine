#include <lux/engine/input/Input.hpp>

#include <lux/engine/input/detail/GlfwInputTranslation.hpp>
#include <lux/engine/input/detail/InputState.hpp>
#include <lux/engine/window/LuxWindow.hpp>
#include <utility>

namespace lux::input
{
    void Input::sample(lux::window::LuxWindow& window)
    {
        auto& next = state_->snapshot;
        next.keys_just_pressed.reset();
        next.keys_just_released.reset();
        next.mouse_just_pressed = next.mouse_just_released = 0;
        next.scroll_dx = next.scroll_dy = 0;
        next.keyboard_captured_by_ui = next.mouse_captured_by_ui = false;
        next.events.clear();

        const auto events = window.drainInputEvents();
        detail::applyGlfwWindowEvents(next, events);

        window.getCursorPos(&next.cursor_x, &next.cursor_y);
        next.cursor_dx = state_->sampled ? next.cursor_x - state_->previous_cursor_x : 0.0;
        next.cursor_dy = state_->sampled ? next.cursor_y - state_->previous_cursor_y : 0.0;
        state_->previous_cursor_x = next.cursor_x;
        state_->previous_cursor_y = next.cursor_y;

        window.size(next.window_width, next.window_height);
        window.framebufferSize(next.framebuffer_width, next.framebuffer_height);

        const double now = lux::window::LuxWindow::timeAfterFirstInitialization();
        next.sample_timestamp = now;
        next.sample_dt = state_->sampled ? static_cast<float>(now - state_->previous_sample_time) : 0.0f;
        state_->previous_sample_time = now;
        state_->sampled = true;
    }
}
