#pragma once
#include "TriggerDesc.hpp"
#include "InputValue.hpp"
#include <vector>
#include <cmath>
#include <functional>

namespace lux::input
{
    // ------------------------------------------------------------------ //
    //  Single-trigger evaluator                                           //
    // ------------------------------------------------------------------ //

    /// Evaluate one TriggerDesc against the current input value.
    /// \param desc            The trigger description.
    /// \param rt              Mutable runtime state for this trigger instance.
    /// \param value           The current (modified) InputValue for the binding or action.
    /// \param dt              Frame delta time.
    /// \param isActionActive  Callback to query whether another ActionId is currently active
    ///                        (used by ChordAction / Combo). May be nullptr if unused.
    ///
    /// TODO: The std::function<bool(ActionId)> parameter incurs type erasure overhead
    ///       on every call. For hot paths with many bindings/actions per frame, consider
    ///       migrating to a lightweight callable (function_ref or template parameter).
    inline ETriggerState evaluateTrigger(
        const TriggerDesc& desc,
        TriggerRuntimeState& rt,
        const InputValue& value,
        float dt,
        const std::function<bool(ActionId)>& isActionActive = nullptr)
    {
        // Helper: is the value "actuated" (exceeds threshold)?
        const float magnitude = [&]() -> float {
            switch (value.type) {
            case EInputValueType::BOOL:   return std::fabs(value.v.x);
            case EInputValueType::AXIS_1D: return std::fabs(value.v.x);
            case EInputValueType::AXIS_2D: return std::sqrt(value.v.x * value.v.x + value.v.y * value.v.y);
            case EInputValueType::AXIS_3D: return std::sqrt(value.v.x * value.v.x + value.v.y * value.v.y + value.v.z * value.v.z);
            }
            return 0.0f;
        }();

        const bool actuated = magnitude >= desc.actuation_threshold;
        const bool was_actuated = rt.actuated;
        rt.actuated = actuated;

        switch (desc.kind)
        {
        case ETriggerKind::DOWN:
            return actuated ? ETriggerState::TRIGGERED : ETriggerState::NONE;

        case ETriggerKind::PRESSED:
            return (actuated && !was_actuated) ? ETriggerState::TRIGGERED : ETriggerState::NONE;

        case ETriggerKind::RELEASED:
            return (!actuated && was_actuated) ? ETriggerState::TRIGGERED : ETriggerState::NONE;

        case ETriggerKind::HOLD:
            if (actuated) {
                rt.elapsed += dt;
                if (rt.elapsed >= desc.hold_time_seconds)
                    return ETriggerState::TRIGGERED;
                return ETriggerState::ONGOING;
            }
            rt.elapsed = 0.0f;
            return ETriggerState::NONE;

        case ETriggerKind::HOLD_AND_RELEASE:
            if (actuated) {
                rt.elapsed += dt;
                return ETriggerState::ONGOING;
            }
            if (was_actuated && rt.elapsed >= desc.hold_time_seconds) {
                rt.elapsed = 0.0f;
                return ETriggerState::TRIGGERED;
            }
            rt.elapsed = 0.0f;
            return ETriggerState::NONE;

        case ETriggerKind::TAP:
            if (actuated && !was_actuated) {
                rt.elapsed = 0.0f;
            }
            if (actuated) {
                rt.elapsed += dt;
                if (rt.elapsed > desc.tap_time_seconds)
                    return ETriggerState::NONE; // Held too long — not a tap
                return ETriggerState::ONGOING;
            }
            if (was_actuated && rt.elapsed <= desc.tap_time_seconds) {
                rt.elapsed = 0.0f;
                return ETriggerState::TRIGGERED;
            }
            return ETriggerState::NONE;

        case ETriggerKind::PULSE:
            if (actuated) {
                rt.elapsed += dt;
                if (rt.elapsed >= desc.pulse_interval) {
                    rt.elapsed -= desc.pulse_interval;
                    return ETriggerState::TRIGGERED;
                }
                return ETriggerState::ONGOING;
            }
            rt.elapsed = 0.0f;
            return ETriggerState::NONE;

        case ETriggerKind::CHORD_ACTION:
            if (isActionActive && isActionActive(desc.chord_action))
                return actuated ? ETriggerState::TRIGGERED : ETriggerState::NONE;
            return actuated ? ETriggerState::ONGOING : ETriggerState::NONE;

        case ETriggerKind::COMBO:
            // NOT YET IMPLEMENTED — always returns None.
            // A full combo state machine (sequential action matching with timeout)
            // is planned as a future extension.
            return ETriggerState::NONE;
        }

        return ETriggerState::NONE;
    }

    // ------------------------------------------------------------------ //
    //  Composite trigger group evaluator                                  //
    // ------------------------------------------------------------------ //

    /// Evaluate a group of triggers using Explicit / Implicit / Blocker logic.
    /// Returns the combined ETriggerState for the group.
    ///
    /// Rules (matching UE5 Enhanced Input):
    ///   - If any Blocker fires Triggered → result is None (blocked).
    ///   - All Implicit triggers must be Triggered for the Implicit group to pass.
    ///   - Any one Explicit trigger being Triggered is enough for the Explicit group.
    ///   - If no triggers are defined, the result defaults to Triggered (pass-through).
    inline ETriggerState evaluateTriggerGroup(
        const std::vector<TriggerDesc>& descs,
        std::vector<TriggerRuntimeState>& rts,
        const InputValue& value,
        float dt,
        const std::function<bool(ActionId)>& isActionActive = nullptr)
    {
        if (descs.empty())
            return ETriggerState::TRIGGERED; // No triggers = always pass

        // Ensure runtime state vector matches desc count.
        if (rts.size() < descs.size())
            rts.resize(descs.size());

        bool has_explicit  = false;
        bool any_explicit  = false;
        bool has_implicit  = false;
        bool all_implicit  = true;
        bool any_ongoing   = false;

        for (size_t i = 0; i < descs.size(); ++i) {
            ETriggerState result = evaluateTrigger(descs[i], rts[i], value, dt, isActionActive);

            switch (descs[i].logic)
            {
            case ETriggerLogicType::BLOCKER:
                if (result == ETriggerState::TRIGGERED)
                    return ETriggerState::NONE; // Blocked
                break;

            case ETriggerLogicType::IMPLICIT:
                has_implicit = true;
                if (result != ETriggerState::TRIGGERED)
                    all_implicit = false;
                if (result == ETriggerState::ONGOING)
                    any_ongoing = true;
                break;

            case ETriggerLogicType::EXPLICIT:
                has_explicit = true;
                if (result == ETriggerState::TRIGGERED)
                    any_explicit = true;
                if (result == ETriggerState::ONGOING)
                    any_ongoing = true;
                break;
            }
        }

        // Implicit: all must pass
        bool implicit_ok = !has_implicit || all_implicit;

        // Explicit: at least one must pass (or no explicits exist)
        bool explicit_ok = !has_explicit || any_explicit;

        if (implicit_ok && explicit_ok)
            return ETriggerState::TRIGGERED;

        if (any_ongoing)
            return ETriggerState::ONGOING;

        return ETriggerState::NONE;
    }

} // namespace lux::input
