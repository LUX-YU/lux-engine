#include <lux/engine/input/Input.hpp>

#include <lux/engine/input/detail/InputState.hpp>

#include <utility>

namespace lux::input
{
    Input::Input()
        : state_{std::make_unique<detail::InputState>()}
    {
    }

    Input::~Input() = default;

    void Input::evaluate(
        float dt,
        bool accept_keyboard,
        bool accept_pointer
    )
    {
        state_->mapper.update(
            state_->snapshot,
            state_->contexts,
            dt,
            accept_keyboard && !state_->snapshot.keyboard_captured_by_ui,
            accept_pointer && !state_->snapshot.mouse_captured_by_ui
        );
    }

    void Input::evaluate(
        InputSnapshot snapshot,
        float dt,
        bool accept_keyboard,
        bool accept_pointer
    )
    {
        state_->snapshot = std::move(snapshot);
        evaluate(dt, accept_keyboard, accept_pointer);
    }

    const InputSnapshot& Input::snapshot() const noexcept
    {
        return state_->snapshot;
    }

    ActionMapper& Input::mapper() noexcept
    {
        return state_->mapper;
    }

    const ActionMapper& Input::mapper() const noexcept
    {
        return state_->mapper;
    }

    InputActionRegistry& Input::actionRegistry() noexcept
    {
        return state_->mapper.actionRegistry();
    }

    const InputActionRegistry& Input::actionRegistry() const noexcept
    {
        return state_->mapper.actionRegistry();
    }

    InputContextStack& Input::contexts() noexcept
    {
        return state_->contexts;
    }

    const InputContextStack& Input::contexts() const noexcept
    {
        return state_->contexts;
    }
}
