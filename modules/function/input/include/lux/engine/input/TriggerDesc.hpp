#pragma once
#include "ActionId.hpp"
#include <cstdint>
#include <vector>

namespace lux::input
{
    // ------------------------------------------------------------------ //
    //  Trigger logic type — how multiple triggers of the same type combine //
    // ------------------------------------------------------------------ //

    enum class ETriggerLogicType : uint8_t
    {
        /// Any *one* Explicit trigger passing is enough to satisfy the group.
        EXPLICIT,
        /// *All* Implicit triggers must pass simultaneously.
        IMPLICIT,
        /// If *any* Blocker fires, the action is blocked regardless of others.
        BLOCKER,
    };

    // ------------------------------------------------------------------ //
    //  Trigger kind — the specific condition being evaluated               //
    // ------------------------------------------------------------------ //

    enum class ETriggerKind : uint8_t
    {
        DOWN,            ///< Active whenever the raw value exceeds actuation threshold
        PRESSED,         ///< Fires on the frame actuation threshold is crossed (rising edge)
        RELEASED,        ///< Fires on the frame actuation drops below threshold (falling edge)
        HOLD,            ///< Must be held for ≥ hold_time_seconds
        HOLD_AND_RELEASE,///< Like HOLD but fires only on release after hold_time_seconds
        TAP,             ///< Press+release within tap_time_seconds
        PULSE,           ///< Fires repeatedly at pulse_interval while held
        CHORD_ACTION,    ///< Another action must be active simultaneously
        COMBO,           ///< [NOT YET IMPLEMENTED] A sequence of actions must fire in order within timeout.
                       ///< Currently always evaluates to ETriggerState::NONE.
    };

    // ------------------------------------------------------------------ //
    //  Trigger state — three-valued result of a single trigger evaluation  //
    // ------------------------------------------------------------------ //

    enum class ETriggerState : uint8_t
    {
        NONE,      ///< Not relevant this frame
        ONGOING,   ///< Condition partially met, waiting for completion
        TRIGGERED, ///< Condition fully met this frame
    };

    // ------------------------------------------------------------------ //
    //  TriggerDesc — declarative description of a trigger condition        //
    // ------------------------------------------------------------------ //

    struct TriggerDesc
    {
        ETriggerKind      kind  = ETriggerKind::DOWN;
        ETriggerLogicType logic = ETriggerLogicType::EXPLICIT;

        float actuation_threshold = 0.5f;   ///< Axis magnitude considered "actuated"
        float hold_time_seconds   = 0.0f;   ///< For Hold / HoldAndRelease
        float tap_time_seconds    = 0.2f;   ///< For Tap
        float pulse_interval      = 0.1f;   ///< For Pulse

        ActionId chord_action = InvalidActionId;          ///< For ChordAction
        std::vector<ActionId> combo_sequence;              ///< For Combo [NOT YET IMPLEMENTED]
        float combo_timeout_seconds = 0.0f;               ///< For Combo [NOT YET IMPLEMENTED]
    };

    // ------------------------------------------------------------------ //
    //  Per-trigger runtime state (mutable, one per TriggerDesc instance)  //
    // ------------------------------------------------------------------ //

    struct TriggerRuntimeState
    {
        float elapsed   = 0.0f;   ///< General-purpose timer (Hold / Tap / Pulse)
        int   tap_count = 0;      ///< For Combo sequences
        bool  actuated  = false;  ///< Was actuated last frame
    };

} // namespace lux::input
