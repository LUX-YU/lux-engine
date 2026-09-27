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

    enum class EActionEvent : std::uint8_t
    {
        NONE = 0,
        STARTED = 1 << 0,   ///< Became active this frame
        ONGOING = 1 << 1,   ///< Active but trigger not yet met
        TRIGGERED = 1 << 2, ///< Trigger condition fully met
        COMPLETED = 1 << 3, ///< Transitioned from active to inactive
        CANCELED = 1 << 4,  ///< Interrupted before trigger was met
    };

    [[nodiscard]] constexpr std::uint8_t actionEventMask(EActionEvent event) noexcept
    {
        return static_cast<std::uint8_t>(event);
    }

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

        bool down = false;
        bool prev_down = false;
        float held_seconds = 0.0f;

        std::uint8_t events = actionEventMask(EActionEvent::NONE);

        ETriggerState trigger_state = ETriggerState::NONE;
        ETriggerState prev_trigger_state = ETriggerState::NONE;

        BindingId dominant_binding = InvalidBindingId;

        // ── Query helpers (all based on events / down / trigger_state) ── //
        [[nodiscard]] bool started() const noexcept
        {
            return (events & actionEventMask(EActionEvent::STARTED)) != 0;
        }
        [[nodiscard]] bool ongoing() const noexcept
        {
            return (events & actionEventMask(EActionEvent::ONGOING)) != 0;
        }
        [[nodiscard]] bool triggered() const noexcept
        {
            return (events & actionEventMask(EActionEvent::TRIGGERED)) != 0;
        }
        [[nodiscard]] bool completed() const noexcept
        {
            return (events & actionEventMask(EActionEvent::COMPLETED)) != 0;
        }
        [[nodiscard]] bool canceled() const noexcept
        {
            return (events & actionEventMask(EActionEvent::CANCELED)) != 0;
        }
        [[nodiscard]] bool active() const noexcept
        {
            return down || trigger_state != ETriggerState::NONE;
        }
    };

} // namespace lux::input
