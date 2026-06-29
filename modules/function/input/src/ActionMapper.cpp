#include <lux/engine/input/ActionMapper.hpp>
#include <lux/engine/window/InputSnapshot.hpp>
#include <cassert>
#include <cmath>

namespace lux::input
{
    // -----------------------------------------------------------------------
    // Static defaults
    // -----------------------------------------------------------------------
    const ActionState ActionMapper::kDefaultState_{};
    const InputValue  ActionMapper::kDefaultValue_{};

    // -----------------------------------------------------------------------
    // Static helpers
    // -----------------------------------------------------------------------

    ActionMapper::RawExtract ActionMapper::extractRaw(
        const lux::window::InputSnapshot& snapshot,
        const ActionBinding& binding)
    {
        return std::visit([&](const auto& input) -> RawExtract
        {
            using T = std::decay_t<decltype(input)>;
            RawExtract raw{};

            if constexpr (std::is_same_v<T, KeyInput>) {
                raw.just_pressed  = snapshot.isKeyJustPressed(input.key);
                raw.just_released = snapshot.isKeyJustReleased(input.key);
                raw.held          = snapshot.isKeyHeld(input.key);
                raw.value         = raw.held || raw.just_pressed ? 1.f : 0.f;
            }
            else if constexpr (std::is_same_v<T, MouseButtonInput>) {
                raw.just_pressed  = snapshot.isMouseButtonJustPressed(input.button);
                raw.just_released = snapshot.isMouseButtonJustReleased(input.button);
                raw.held          = snapshot.isMouseButtonHeld(input.button);
                raw.value         = raw.held || raw.just_pressed ? 1.f : 0.f;
            }
            else if constexpr (std::is_same_v<T, MouseAxisInput>) {
                float delta = 0.f;
                switch (input.axis) {
                    case MouseAxisInput::EAxis::DELTA_X:  delta = static_cast<float>(snapshot.cursor_dx); break;
                    case MouseAxisInput::EAxis::DELTA_Y:  delta = static_cast<float>(snapshot.cursor_dy); break;
                    case MouseAxisInput::EAxis::SCROLL_X: delta = static_cast<float>(snapshot.scroll_dx); break;
                    case MouseAxisInput::EAxis::SCROLL_Y: delta = static_cast<float>(snapshot.scroll_dy); break;
                }
                raw.value         = delta * input.scale;
                raw.held          = (raw.value != 0.f);
                raw.just_pressed  = raw.held;
                raw.just_released = false;
            }

            return raw;
        }, binding.source);
    }

    InputValue ActionMapper::projectContribution(float raw_scalar,
                                                 const InputValue& contribution)
    {
        InputValue out = contribution;
        out.v.x *= raw_scalar;
        out.v.y *= raw_scalar;
        out.v.z *= raw_scalar;
        return out;
    }

    // ETriggerState merge priority: Triggered > Ongoing > None
    static int triggerPriority(ETriggerState ts) noexcept
    {
        switch (ts) {
            case ETriggerState::TRIGGERED: return 2;
            case ETriggerState::ONGOING:   return 1;
            case ETriggerState::NONE:      return 0;
        }
        return 0;
    }

    // -----------------------------------------------------------------------
    //  Pipeline entry point
    // -----------------------------------------------------------------------

    void ActionMapper::update(const lux::window::InputSnapshot& snapshot,
                              InputContextStack& stack,
                              float dt,
                              bool want_kb,
                              bool want_mouse)
    {
        dt_ = dt;

        beginFrame(dt);
        evaluateBindings(snapshot, stack, dt, want_kb, want_mouse);
        accumulateActions();
        evaluateActionLayer(dt);
        finalizeEvents(dt);
    }

    // -----------------------------------------------------------------------
    //  1. beginFrame — save prev into state internals, apply injections
    // -----------------------------------------------------------------------

    void ActionMapper::beginFrame(float /*dt*/)
    {
        // ── Carry forward prev fields in states_ (action set is stable across frames) ──
        for (auto& key : states_.keys()) {
            auto& s = states_.at(key);
            s.prev_value         = s.value;
            s.prev_down          = s.down;
            s.prev_trigger_state = s.trigger_state;
            // held_seconds carries forward — finalizeEvents will += dt if still active.
            // Reset current-frame fields.
            s.value         = InputValue{};
            s.down          = false;
            s.trigger_state = ETriggerState::NONE;
            s.events        = ActionEvent_None;
            s.dominant_binding = InvalidBindingId;
        }

        // ── Carry forward prev fields in binding_states_, then clear ──
        // We need prev data for evaluateBindings, but bindings are rebuilt each frame.
        // Save prev into a temp SparseSet, then clear binding_states_.
        // evaluateBindings will re-populate with fresh data + prev from old.
        prev_binding_states_.clear();
        for (auto& key : binding_states_.keys()) {
            auto& obs = binding_states_.at(key);
            auto& pbs = prev_binding_states_[key];
            pbs.prev_down          = obs.down;
            pbs.held_seconds       = obs.held_seconds;
            pbs.prev_trigger_state = obs.trigger_state;
        }
        binding_states_.clear();

        // ── One-shot injected triggered events ──
        for (auto& key : injected_triggered_.keys()) {
            auto& value = injected_triggered_.at(key);
            auto& s = states_[key];
            s.value  = value;
            s.down   = true;
            s.trigger_state = ETriggerState::TRIGGERED;
            s.events = ActionEvent_Triggered;
        }
        injected_triggered_.clear();

        // ── Persisted injected values ──
        for (auto& key : injected_value_.keys()) {
            auto& value = injected_value_.at(key);
            auto& s = states_[key];
            s.value = value;
            s.down  = !value.nearlyZero();
            s.trigger_state = s.down ? ETriggerState::ONGOING : ETriggerState::NONE;
        }
    }

    // -----------------------------------------------------------------------
    //  2. evaluateBindings — extract raw, run trigger evaluation, fill state
    // -----------------------------------------------------------------------

    void ActionMapper::evaluateBindings(const lux::window::InputSnapshot& snapshot,
                                        InputContextStack& stack,
                                        float dt,
                                        bool want_kb,
                                        bool want_mouse)
    {
        bool kb_consumed    = !want_kb;
        bool mouse_consumed = !want_mouse;

        // Callback for ChordAction triggers at binding level.
        auto isActionActive = [this](ActionId aid) -> bool {
            return states_.contains(aid) && states_.at(aid).active();
        };

        auto active = stack.active();

        for (auto it = active.rbegin(); it != active.rend(); ++it) {
            InputContext* ctx = *it;
            if (!ctx) continue;
            if (!ctx->enabled()) continue;

            const bool skip_kb    = kb_consumed;
            const bool skip_mouse = mouse_consumed;

            for (auto& binding : ctx->actionMap().bindings()) {
                const bool is_axis      = std::holds_alternative<MouseAxisInput>(binding.source);
                const bool is_kb        = std::holds_alternative<KeyInput>(binding.source);
                const bool is_mouse_btn = std::holds_alternative<MouseButtonInput>(binding.source);

                // --- Consumption / category filtering ---
                bool blocked = false;
                if (is_kb        && skip_kb)    blocked = true;
                if (is_mouse_btn && skip_mouse) blocked = true;
                if (is_axis      && skip_mouse) blocked = true;

                // Look up or create the binding state.
                auto& bs = binding_states_[binding.binding_id];
                bs.binding_id = binding.binding_id;
                bs.action_id  = binding.action;

                // Seed prev data from previous frame if available.
                if (prev_binding_states_.contains(binding.binding_id)) {
                    auto& pbs = prev_binding_states_.at(binding.binding_id);
                    bs.prev_down          = pbs.prev_down;
                    bs.held_seconds       = pbs.held_seconds;
                    bs.prev_trigger_state = pbs.prev_trigger_state;
                }

                if (blocked) {
                    bs.blocked_by_consume = true;
                    continue;
                }

                // Extract raw input.
                RawExtract raw = extractRaw(snapshot, binding);

                bs.down              = raw.held || raw.just_pressed;
                bs.raw_pressed_edge  = raw.just_pressed;
                bs.raw_released_edge = raw.just_released;

                // Project raw scalar through contribution vector.
                InputValue projected = projectContribution(raw.value, binding.contribution);

                // Apply per-binding modifiers.
                if (!binding.binding_modifiers.empty())
                    projected = applyModifiers(projected, binding.binding_modifiers);

                bs.raw_value      = projectContribution(raw.value, binding.contribution);
                bs.modified_value = projected;

                // Evaluate per-binding triggers.
                if (!binding.binding_triggers.empty()) {
                    auto& rts = binding_trigger_rts_[binding.binding_id];
                    bs.trigger_state = evaluateTriggerGroup(
                        binding.binding_triggers, rts, projected, dt, isActionActive);
                } else {
                    // No binding triggers: default behavior — Down when value is non-zero.
                    bs.trigger_state = bs.down ? ETriggerState::TRIGGERED : ETriggerState::NONE;
                }

                // Update held_seconds.
                if (bs.down && bs.prev_down)
                    bs.held_seconds += dt;
                else if (bs.down)
                    bs.held_seconds = dt;
                else
                    bs.held_seconds = 0.f;
            }

            // Mark categories as consumed for lower-priority contexts.
            if (ctx->consumesKeyboard()) kb_consumed    = true;
            if (ctx->consumesMouse())    mouse_consumed = true;
        }
    }

    // -----------------------------------------------------------------------
    //  3. accumulateActions — merge active bindings into per-action values
    // -----------------------------------------------------------------------

    void ActionMapper::accumulateActions()
    {
        for (auto& bid : binding_states_.keys()) {
            auto& bs = binding_states_.at(bid);
            if (bs.blocked_by_ui || bs.blocked_by_consume) continue;

            // Only accumulate bindings whose trigger passed.
            if (bs.trigger_state == ETriggerState::NONE) continue;

            auto& s = states_[bs.action_id];

            // All actions must be registered. Assert in debug, skip in release.
            const InputActionDesc* desc = registry_.find(bs.action_id);
            assert(desc
                && "ActionMapper: binding references an unregistered ActionId — "
                   "call actionRegistry().registerAction() before use");
            if (!desc) continue; // Release safety: skip unregistered actions.

            // Always use typed accumulation — target_type guarantees no type drift.
            s.value = accumulateValue(s.value, bs.modified_value, desc->value_type);

            s.down  = true;

            // Track trigger state: higher priority wins.
            if (triggerPriority(bs.trigger_state) > triggerPriority(s.trigger_state))
                s.trigger_state = bs.trigger_state;

            // Track dominant binding (highest trigger priority).
            if (s.dominant_binding == InvalidBindingId ||
                triggerPriority(bs.trigger_state) > 0) {
                s.dominant_binding = bid;
            }
        }
    }

    // -----------------------------------------------------------------------
    //  4. evaluateActionLayer — action-level modifiers + triggers
    // -----------------------------------------------------------------------

    void ActionMapper::evaluateActionLayer(float dt)
    {
        // Callback for ChordAction triggers: is a given action currently active?
        auto isActionActive = [this](ActionId aid) -> bool {
            return states_.contains(aid) && states_.at(aid).active();
        };

        for (auto& id : states_.keys()) {
            auto& s = states_.at(id);
            const InputActionDesc* desc = registry_.find(id);
            if (!desc) continue;

            // Apply action-level modifiers on the accumulated value.
            if (!desc->action_modifiers.empty())
                s.value = applyModifiers(s.value, desc->action_modifiers);

            // Evaluate action-level triggers.
            if (!desc->action_triggers.empty()) {
                auto& rts = action_trigger_rts_[id];

                ETriggerState prev_ts = s.trigger_state;
                s.trigger_state = evaluateTriggerGroup(
                    desc->action_triggers, rts, s.value, dt, isActionActive);

                // Map ETriggerState → events flags.
                switch (s.trigger_state) {
                case ETriggerState::TRIGGERED:
                    s.events |= ActionEvent_Triggered;
                    break;
                case ETriggerState::ONGOING:
                    s.events |= ActionEvent_Ongoing;
                    break;
                case ETriggerState::NONE:
                    break;
                }
            } else {
                // No action-level triggers: derive events from binding-level trigger state.
                switch (s.trigger_state) {
                case ETriggerState::TRIGGERED:
                    s.events |= ActionEvent_Triggered;
                    break;
                case ETriggerState::ONGOING:
                    s.events |= ActionEvent_Ongoing;
                    break;
                case ETriggerState::NONE:
                    break;
                }
            }
        }
    }

    // -----------------------------------------------------------------------
    //  5. finalizeEvents — edge detection + event bitfield
    // -----------------------------------------------------------------------

    void ActionMapper::finalizeEvents(float dt)
    {
        for (auto& id : states_.keys()) {
            auto& s = states_.at(id);
            bool was_active = s.prev_down || s.prev_trigger_state != ETriggerState::NONE;

            bool is_started   = s.active() && !was_active;
            bool is_completed = !s.active() && was_active;

            if (is_started)
                s.events |= ActionEvent_Started;
            if (is_completed)
                s.events |= ActionEvent_Completed;

            // held_seconds
            if (s.active() && was_active)
                s.held_seconds += dt;
            else if (s.active())
                s.held_seconds = dt;
            else
                s.held_seconds = 0.f;
        }
    }

    // -----------------------------------------------------------------------
    // Query helpers
    // -----------------------------------------------------------------------

    const ActionState& ActionMapper::state(ActionId id) const noexcept
    {
        return states_.contains(id) ? states_.at(id) : kDefaultState_;
    }

    bool ActionMapper::performed(ActionId id) const noexcept
    { return state(id).performed(); }

    bool ActionMapper::ongoing(ActionId id) const noexcept
    { return state(id).ongoing(); }

    bool ActionMapper::canceled(ActionId id) const noexcept
    { return state(id).canceled(); }

    bool ActionMapper::active(ActionId id) const noexcept
    { return state(id).active(); }

    bool ActionMapper::triggered(ActionId id) const noexcept
    { return state(id).triggered(); }

    float ActionMapper::axis(ActionId id) const noexcept
    { return state(id).value.as1D(); }

    const InputValue& ActionMapper::getValue(ActionId id) const noexcept
    {
        return states_.contains(id) ? states_.at(id).value : kDefaultValue_;
    }

    const InputBindingState* ActionMapper::bindingState(BindingId id) const noexcept
    {
        return binding_states_.contains(id) ? &binding_states_.at(id) : nullptr;
    }

    // -----------------------------------------------------------------------
    // Injection
    // -----------------------------------------------------------------------

    void ActionMapper::injectTriggered(ActionId id, const InputValue& value)
    { injected_triggered_[id] = value; }

    void ActionMapper::injectValue(ActionId id, const InputValue& value)
    { injected_value_[id] = value; }

    void ActionMapper::injectPerformed(ActionId id, float value)
    { injected_triggered_[id] = InputValue::makeAxis1D(value); }

    void ActionMapper::injectAxis(ActionId id, float value)
    { injected_value_[id] = InputValue::makeAxis1D(value); }

} // namespace lux::input
