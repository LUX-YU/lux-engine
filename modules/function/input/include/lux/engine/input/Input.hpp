#pragma once

#include <lux/engine/function/visibility.h>
#include <lux/engine/input/ActionMapper.hpp>
#include <lux/engine/input/InputContextStack.hpp>
#include <lux/engine/input/InputSnapshot.hpp>

#include <memory>

namespace lux::window
{
    class LuxWindow;
}

namespace lux::input
{
    namespace detail
    {
        struct InputState;
    }

    class LUX_FUNCTION_PUBLIC Input final
    {
    public:
        Input();
        ~Input();

        Input(const Input&) = delete;
        Input& operator=(const Input&) = delete;
        Input(Input&&) = delete;
        Input& operator=(Input&&) = delete;

        void sample(lux::window::LuxWindow& window);

        void evaluate(
            float dt,
            bool accept_keyboard = true,
            bool accept_pointer = true
        );

        void evaluate(
            InputSnapshot snapshot,
            float dt,
            bool accept_keyboard = true,
            bool accept_pointer = true
        );

        [[nodiscard]] const InputSnapshot& snapshot() const noexcept;

        [[nodiscard]] ActionMapper& mapper() noexcept;
        [[nodiscard]] const ActionMapper& mapper() const noexcept;

        [[nodiscard]] InputActionRegistry& actionRegistry() noexcept;
        [[nodiscard]] const InputActionRegistry& actionRegistry() const noexcept;

        [[nodiscard]] InputContextStack& contexts() noexcept;
        [[nodiscard]] const InputContextStack& contexts() const noexcept;

    private:
        std::unique_ptr<detail::InputState> state_;
    };
}
