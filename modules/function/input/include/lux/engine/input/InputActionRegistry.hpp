#pragma once
#include "InputActionDesc.hpp"
#include <lux/cxx/container/SparseSet.hpp>
#include <unordered_map>
#include <string>
#include <cassert>

namespace lux::input
{
    /// Registry of InputActionDesc instances.
    ///
    /// **All actions must be registered** before being used in an ActionMap.
    /// ActionMapper::accumulateActions() asserts that every binding's target action
    /// has a registered descriptor — this guarantees type-safe accumulation via
    /// the target value_type, and enables action-level modifiers and triggers.
    ///
    /// ActionId values are runtime-allocated small integers (starting from 1).
    /// Use the returned ActionId from registerAction() in all subsequent bindings
    /// and queries.
    class InputActionRegistry
    {
    public:
        /// Register an action description. Returns the auto-allocated ActionId.
        /// If an action with the same name already exists, updates the descriptor
        /// and returns the existing id (idempotent).
        ActionId registerAction(InputActionDesc desc)
        {
            auto it = name_to_id_.find(desc.name);
            if (it != name_to_id_.end())
            {
                // Already registered — update desc, return existing id.
                ActionId existing = it->second;
                desc.id = existing;
                descs_.at(existing) = std::move(desc);
                return existing;
            }

            // New action — auto-allocate id.
            ActionId id = descs_.insert(std::move(desc));
            descs_.at(id).id = id;
            name_to_id_[descs_.at(id).name] = id;
            return id;
        }

        /// Unregister an action.
        void unregisterAction(ActionId id)
        {
            if (descs_.contains(id))
            {
                name_to_id_.erase(descs_.at(id).name);
                descs_.erase(id);
            }
        }

        /// Find a registered action by id. Returns nullptr if not registered.
        [[nodiscard]] const InputActionDesc* find(ActionId id) const
        {
            return descs_.contains(id) ? &descs_.at(id) : nullptr;
        }

        /// Find a registered action by name (cold path). Returns InvalidActionId if not found.
        [[nodiscard]] ActionId findByName(const std::string& name) const
        {
            auto it = name_to_id_.find(name);
            return it != name_to_id_.end() ? it->second : InvalidActionId;
        }

        /// Clear all registered actions.
        void clear()
        {
            descs_.clear();
            name_to_id_.clear();
        }

        [[nodiscard]] const auto& keys() const noexcept
        {
            return descs_.keys();
        }
        [[nodiscard]] const auto& values() const noexcept
        {
            return descs_.values();
        }

    private:
        lux::cxx::OffsetAutoSparseSet<uint32_t, InputActionDesc, 1> descs_;
        std::unordered_map<std::string, ActionId> name_to_id_;
    };

} // namespace lux::input
