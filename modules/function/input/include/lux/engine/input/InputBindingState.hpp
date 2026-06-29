#pragma once
#include "ActionId.hpp"
#include "InputValue.hpp"
#include "TriggerDesc.hpp"
#include <cstdint>

namespace lux::input
{
    using BindingId = uint32_t;
    inline constexpr BindingId InvalidBindingId = 0;

    /// Per-frame intermediate state for a single ActionBinding.
    /// Populated during evaluateBindings(), consumed by accumulateActions().
    struct InputBindingState
    {
        BindingId binding_id = InvalidBindingId;
        ActionId  action_id  = InvalidActionId;

        InputValue raw_value{};
        InputValue modified_value{};

        bool raw_pressed_edge  = false;
        bool raw_released_edge = false;

        bool  down      = false;
        bool  prev_down = false;
        float held_seconds = 0.0f;

        ETriggerState trigger_state      = ETriggerState::NONE;
        ETriggerState prev_trigger_state = ETriggerState::NONE;

        bool blocked_by_ui      = false;
        bool blocked_by_consume = false;
    };

} // namespace lux::input
