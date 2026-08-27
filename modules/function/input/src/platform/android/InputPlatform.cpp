#include <lux/engine/input/Input.hpp>

#include <lux/engine/input/detail/InputState.hpp>
#include <lux/engine/window/LuxWindow.hpp>

#include <utility>

namespace lux::input
{
    void Input::sample(lux::window::LuxWindow& window)
    {
        // Android Window does not expose GameActivity input yet. Drain any
        // placeholder events and publish a deterministic empty snapshot.
        (void)window.drainInputEvents();

        InputSnapshot next;
        window.size(next.window_width, next.window_height);
        window.framebufferSize(next.framebuffer_width, next.framebuffer_height);
        state_->snapshot = std::move(next);
        state_->sampled = true;
    }
}
