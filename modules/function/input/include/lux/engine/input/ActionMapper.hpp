#pragma once
#include "InputContextStack.hpp"
#include "ActionState.hpp"
#include "InputModifier.hpp"
#include "InputBindingState.hpp"
#include "InputActionRegistry.hpp"
#include "TriggerEvaluator.hpp"
#include <lux/engine/function/visibility.h>
#include <lux/engine/window/InputSnapshot.hpp>
#include <lux/cxx/container/SparseSet.hpp>
#include <span>
#include <vector>

namespace lux::input
{
    /// Processes the InputContextStack each frame to produce ActionState values.
    ///
    /// The update pipeline is split into discrete phases:
    ///   1. beginFrame()         — save prev states internally, apply injections
    ///   2. evaluateBindings()   — extract raw, run binding triggers, fill BindingState
    ///   3. accumulateActions()  — merge active bindings into per-action values
    ///   4. evaluateActionLayer()— apply action modifiers + action triggers
    ///   5. finalizeEvents()     — compute Started/Ongoing/Triggered/Completed/Canceled
    ///
    /// Usage:
    ///   mapper.update(snapshot, stack, dt, want_kb, want_mouse);
    ///   if (mapper.triggered(JUMP)) { ... }
    ///
    /// --- Future extensions ---
    /// TODO: InputUserSettings layer (mouse sensitivity, invert-Y, stick dead zones,
    ///       per-action rebinding overrides) between raw extraction and modifier application.
    /// TODO: Gamepad input support — extend InputSnapshot with gamepad axes/buttons,
    ///       add GamepadAxisInput / GamepadButtonInput to PhysicalInput variant.
    class LUX_FUNCTION_PUBLIC ActionMapper
    {
    public:
        // ------------------------------------------------------------------ //
        //  Action registry                                                    //
        // ------------------------------------------------------------------ //

        [[nodiscard]] InputActionRegistry&       actionRegistry()       noexcept { return registry_; }
        [[nodiscard]] const InputActionRegistry& actionRegistry() const noexcept { return registry_; }

        // ------------------------------------------------------------------ //
        //  Per-frame update                                                   //
        // ------------------------------------------------------------------ //

        void update(const lux::window::InputSnapshot& snapshot,
                    InputContextStack& stack,
                    float dt,
                    bool want_kb    = true,
                    bool want_mouse = true);

        // ------------------------------------------------------------------ //
        //  Query API — call after update()                                    //
        // ------------------------------------------------------------------ //

        [[nodiscard]] bool  ongoing  (ActionId id) const noexcept;
        [[nodiscard]] bool  canceled (ActionId id) const noexcept;
        [[nodiscard]] bool  active   (ActionId id) const noexcept;
        [[nodiscard]] bool  triggered(ActionId id) const noexcept;

        /// Multi-dimensional value query.
        [[nodiscard]] const InputValue& getValue(ActionId id) const noexcept;

        /// Full state struct for a given action.
        [[nodiscard]] const ActionState& state(ActionId id) const noexcept;

        /// Per-binding state (for debug inspection).
        [[nodiscard]] const InputBindingState* bindingState(BindingId id) const noexcept;

        // ------------------------------------------------------------------ //
        //  Injection API — use for programmatic / replay inputs               //
        // ------------------------------------------------------------------ //

        /// Inject a one-frame triggered event with the given value.
        void injectTriggered(ActionId id, const InputValue& value);

        /// Inject a persistent value until overwritten next update().
        void injectValue(ActionId id, const InputValue& value);

    private:
        // ── Pipeline stages ─────────────────────────────────── //

        void beginFrame(float dt);

        void evaluateBindings(const lux::window::InputSnapshot& snapshot,
                              InputContextStack& stack,
                              float dt,
                              bool want_kb, bool want_mouse);

        void accumulateActions();

        void evaluateActionLayer(float dt);

        void finalizeEvents(float dt);

        // ── Helpers ─────────────────────────────────────────── //

        struct RawExtract {
            float value        = 0.f;
            bool  just_pressed = false;
            bool  just_released= false;
            bool  held         = false;
        };

        static RawExtract extractRaw(const lux::window::InputSnapshot& snapshot,
                                     const ActionBinding& binding);

        static InputValue projectContribution(float raw_scalar,
                                              const InputValue& contribution);

        // ── State ───────────────────────────────────────────── //

        InputActionRegistry registry_;

        lux::cxx::SparseSet<uint32_t, ActionState, 1> states_;

        lux::cxx::SparseSet<uint32_t, InputBindingState, 1> binding_states_;

        // Per-binding trigger runtime state (persists across frames for timers).
        lux::cxx::SparseSet<uint32_t, std::vector<TriggerRuntimeState>, 1> binding_trigger_rts_;

        // Per-action trigger runtime state (persists across frames for timers).
        lux::cxx::SparseSet<uint32_t, std::vector<TriggerRuntimeState>, 1> action_trigger_rts_;

        lux::cxx::SparseSet<uint32_t, InputValue, 1> injected_value_;
        lux::cxx::SparseSet<uint32_t, InputValue, 1> injected_triggered_;

        // Cached prev-frame binding data for carry-forward (reused each frame via clear()).
        lux::cxx::SparseSet<uint32_t, InputBindingState, 1> prev_binding_states_;

        float dt_ = 0.f;

        static const ActionState kDefaultState_;
        static const InputValue  kDefaultValue_;
    };

} // namespace lux::input
