#pragma once
#include "ActionId.hpp"
#include "InputValue.hpp"
#include "InputModifier.hpp"
#include "TriggerDesc.hpp"
#include <string>
#include <vector>

namespace lux::input
{
    /// Static description of an Input Action.
    ///
    /// This separates the *definition* of an action (what value type it produces,
    /// what action-level modifiers/triggers it has) from the *runtime state*.
    ///
    /// **All actions must be registered** via InputActionRegistry before use.
    /// Unregistered actions will assert in debug builds and be skipped in release.
    /// This ensures type stability (value_type is always known) and makes the
    /// active action set transparent and auditable.
    struct InputActionDesc
    {
        ActionId id = 0;
        std::string name;
        EInputValueType value_type = EInputValueType::AXIS_1D;

        /// Whether this action's bindings consume input (preventing lower-priority
        /// contexts from reading the same physical source).
        bool consume_input = true;

        /// Action-level modifiers applied *after* all binding contributions are
        /// accumulated (e.g. Normalize2D on a WASD-composed movement vector).
        std::vector<ModifierSpec> action_modifiers;

        /// Action-level triggers evaluated on the accumulated/modified value.
        /// E.g. "Hold 0.1s" on Aim, regardless of which binding sourced the input.
        ///
        /// Evaluation order:
        ///   1. Per-binding triggers (ActionBinding::binding_triggers) are evaluated first.
        ///   2. Passing bindings are accumulated into the action value.
        ///   3. Action-level modifiers are applied.
        ///   4. Action-level triggers are evaluated on the final accumulated value.
        std::vector<TriggerDesc> action_triggers;
    };

} // namespace lux::input
