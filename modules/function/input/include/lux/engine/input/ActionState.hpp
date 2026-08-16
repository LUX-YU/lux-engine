#pragma once
#include "InputValue.hpp"
#include "InputBindingState.hpp"
#include "TriggerDesc.hpp"
#include <cstdint>

namespace lux::input
{
    // ------------------------------------------------------------------ //
    //  Action event bitfield                                              //
    // ------------------------------------------------------------------ //

    enum ActionEvent : uint8_t
    {
        ActionEvent_None      = 0,
        ActionEvent_Started   = 1 << 0,   ///< Became active this frame
        ActionEvent_Ongoing   = 1 << 1,   ///< Active but trigger not yet met
        ActionEvent_Triggered = 1 << 2,   ///< Trigger condition fully met
        ActionEvent_Completed = 1 << 3,   ///< Transitioned from active to inactive
        ActionEvent_Canceled  = 1 << 4,   ///< Interrupted before trigger was met
    };

    /// Snapshot of the current state for a single ActionId.
    ///
    /// Field semantics:
    ///   - value / prev_value:         Current and previous frame's accumulated input value.
    ///   - down / prev_down:           Whether any binding is physically active.
    ///   - held_seconds:               Time the action has been continuously active.
    ///   - events:                     Bitfield of ActionEvent flags — the primary query interface
    ///                                 for gameplay code (Started, Ongoing, Triggered, Completed, Canceled).
    ///   - trigger_state:              Internal runtime trigger evaluation result. Use events for queries.
    ///   - dominant_binding:           The BindingId that contributed the highest-priority trigger state.
    ///                                 Useful for debug visualization and device-source detection.
    struct ActionState
    {
        InputValue value{};
        InputValue prev_value{};

        bool  down      = false;
        bool  prev_down = false;
        float held_seconds = 0.0f;

        uint8_t events = ActionEvent_None;

        ETriggerState trigger_state      = ETriggerState::NONE;
        ETriggerState prev_trigger_state = ETriggerState::NONE;

        BindingId dominant_binding = InvalidBindingId;

        // ── Query helpers (all based on events / down / trigger_state) ── //
        [[nodiscard]] bool started()   const noexcept { return (events & ActionEvent_Started)   != 0; }
        [[nodiscard]] bool ongoing()   const noexcept { return (events & ActionEvent_Ongoing)   != 0; }
        [[nodiscard]] bool triggered() const noexcept { return (events & ActionEvent_Triggered) != 0; }
        [[nodiscard]] bool completed() const noexcept { return (events & ActionEvent_Completed) != 0; }
        [[nodiscard]] bool canceled()  const noexcept { return (events & ActionEvent_Canceled)  != 0; }
        [[nodiscard]] bool active()    const noexcept { return down || trigger_state != ETriggerState::NONE; }

    };

} // namespace lux::input
