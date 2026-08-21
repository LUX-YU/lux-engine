#pragma once
#include "ActionBinding.hpp"
#include "BindingIdAllocator.hpp"
#include <span>
#include <vector>

namespace lux::input
{
    /// A container of ActionBindings with a fluent builder API.
    class ActionMap
    {
    public:
        // ------------------------------------------------------------------ //
        //  Fluent builder API                                                 //
        // ------------------------------------------------------------------ //

        /// Bind a keyboard key to an action.
        ActionMap& bindKey(ActionId id,
                           EKey key,
                           InputValue contribution = InputValue::makeAxis1D(1.0f),
                           std::vector<ModifierSpec> binding_modifiers = {},
                           std::vector<TriggerDesc>  binding_triggers = {},
                           std::string mapping_name = {})
        {
            ActionBinding b;
            b.binding_id       = nextBindingId();
            b.action           = id;
            b.source           = KeyInput{ key };
            b.contribution     = contribution;
            b.binding_modifiers = std::move(binding_modifiers);
            b.binding_triggers  = std::move(binding_triggers);
            b.mapping_name      = std::move(mapping_name);
            bindings_.push_back(std::move(b));
            return *this;
        }

        /// Bind a mouse button to an action.
        ActionMap& bindMouseButton(ActionId id,
                                   EMouseButton button,
                                   InputValue contribution = InputValue::makeAxis1D(1.0f),
                                   std::vector<ModifierSpec> binding_modifiers = {},
                                   std::vector<TriggerDesc>  binding_triggers = {},
                                   std::string mapping_name = {})
        {
            ActionBinding b;
            b.binding_id       = nextBindingId();
            b.action           = id;
            b.source           = MouseButtonInput{ button };
            b.contribution     = contribution;
            b.binding_modifiers = std::move(binding_modifiers);
            b.binding_triggers  = std::move(binding_triggers);
            b.mapping_name      = std::move(mapping_name);
            bindings_.push_back(std::move(b));
            return *this;
        }

        /// Bind a mouse axis (delta or scroll) to an action.
        ActionMap& bindMouseAxis(ActionId id,
                                 MouseAxisInput::EAxis axis,
                                 InputValue contribution = InputValue::makeAxis1D(1.0f),
                                 float scale = 1.f,
                                 std::vector<ModifierSpec> binding_modifiers = {},
                                 std::vector<TriggerDesc>  binding_triggers = {},
                                 std::string mapping_name = {})
        {
            ActionBinding b;
            b.binding_id       = nextBindingId();
            b.action           = id;
            b.source           = MouseAxisInput{ axis, scale };
            b.contribution     = contribution;
            b.binding_modifiers = std::move(binding_modifiers);
            b.binding_triggers  = std::move(binding_triggers);
            b.mapping_name      = std::move(mapping_name);
            bindings_.push_back(std::move(b));
            return *this;
        }

        /// Remove all bindings for a specific action.
        ActionMap& unbindAll(ActionId id)
        {
            std::erase_if(bindings_,
                [id](const ActionBinding& b) { return b.action == id; });
            return *this;
        }

        /// Remove every binding.
        ActionMap& clear()
        {
            bindings_.clear();
            return *this;
        }

        // ------------------------------------------------------------------ //
        //  Accessors                                                          //
        // ------------------------------------------------------------------ //

        [[nodiscard]] std::span<ActionBinding>       bindings()       { return bindings_; }
        [[nodiscard]] std::span<const ActionBinding> bindings() const { return bindings_; }

    private:
        BindingId nextBindingId() { return BindingIdAllocator::next(); }

        std::vector<ActionBinding> bindings_;
    };

} // namespace lux::input
