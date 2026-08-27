#pragma once
#include "ActionId.hpp"
#include "PhysicalInput.hpp"
#include "InputValue.hpp"
#include "InputModifier.hpp"
#include "InputBindingState.hpp"
#include "TriggerDesc.hpp"
#include <string>
#include <vector>

namespace lux::input
{
    /// One entry in an ActionMap: maps a PhysicalInput → ActionId through triggers.
    ///
    /// Evaluation flow:
    ///   1. Raw input is extracted from the PhysicalInput source.
    ///   2. The raw scalar is projected through `contribution` to produce the binding value.
    ///   3. `binding_modifiers` are applied to the projected value.
    ///   4. `binding_triggers` are evaluated on the modified value.
    ///      - If no binding triggers are defined, defaults to Down (triggered when non-zero).
    ///   5. Bindings that pass their triggers are accumulated into the parent action.
    ///   6. Action-level modifiers / triggers (InputActionDesc) are applied on the result.
    struct ActionBinding
    {
        BindingId binding_id = InvalidBindingId;
        ActionId action = InvalidActionId;

        PhysicalInput source{};

        InputValue contribution = InputValue::makeAxis1D(1.0f);

        /// Per-binding modifier chain applied after contribution projection.
        std::vector<ModifierSpec> binding_modifiers;

        /// Per-binding trigger descriptions (replaces the old Interaction system).
        /// Evaluated per-binding before accumulation into the parent action.
        /// See also: InputActionDesc::action_triggers for action-level triggers.
        std::vector<TriggerDesc> binding_triggers;

        /// For future rebinding support.
        std::string mapping_name;
    };

} // namespace lux::input
