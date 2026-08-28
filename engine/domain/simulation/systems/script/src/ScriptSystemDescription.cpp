#include <lux/engine/simulation/systems/ScriptSystemDescription.hpp>

#include <algorithm>
#include <new>
#include <optional>
#include <stdexcept>
#include <type_traits>

namespace lux::simulation::script
{
    std::size_t ScriptSystemDescriptionBuilder::BindingHash::operator()(
        const ScriptBindingDescription& binding
    ) const noexcept
    {
        const auto target_hash = std::visit(
            [](const auto& target) noexcept
            {
                using Target = std::remove_cvref_t<decltype(target)>;
                const auto endpoint = [&]() noexcept
                {
                    if constexpr (std::is_same_v<Target, HookScriptTarget>)
                        return target.hook.value;
                    else
                        return target.event.value;
                }();
                const auto system_hash = std::hash<std::uint64_t>{}(target.system.value);
                const auto endpoint_hash = std::hash<std::uint64_t>{}(endpoint);
                const auto kind_hash = std::is_same_v<Target, HookScriptTarget> ? 0U : 1U;
                return system_hash ^ (endpoint_hash << 1U) ^ kind_hash;
            },
            binding.target
        );
        const auto symbol_hash = std::hash<lux::script::ScriptSymbolId>{}(binding.symbol);
        return symbol_hash ^ (target_hash + 0x9e3779b9U + (symbol_hash << 6U) + (symbol_hash >> 2U));
    }

    lux::cxx::expected<void, EScriptSystemDescriptionError>
    ScriptSystemDescriptionBuilder::addMount(
        ScriptMountDescription mount
    ) noexcept
    {
        if (!mount.id.valid() || mount.asset.isNull() ||
            (std::holds_alternative<EntityScriptMount>(mount.scope) &&
             !std::get<EntityScriptMount>(mount.scope).object.valid()))
        {
            return lux::cxx::unexpected(
                EScriptSystemDescriptionError::INVALID_MOUNT);
        }
        try
        {
            std::unordered_set<ScriptBindingDescription, BindingHash> binding_index;
            binding_index.reserve(mount.bindings.size());
            for (const auto& binding : mount.bindings)
            {
                const bool is_valid_target = std::visit(
                    [](const auto& target) noexcept
                    {
                        using Target = std::remove_cvref_t<decltype(target)>;
                        if constexpr (std::is_same_v<Target, HookScriptTarget>)
                            return target.system.valid() && target.hook.valid();
                        else
                            return target.system.valid() && target.event.valid();
                    },
                    binding.target
                );
                if (binding.symbol == lux::script::InvalidScriptSymbolId || !is_valid_target)
                    return lux::cxx::unexpected(EScriptSystemDescriptionError::INVALID_MOUNT);
                if (!binding_index.insert(binding).second)
                    return lux::cxx::unexpected(EScriptSystemDescriptionError::DUPLICATE_BINDING);
            }

            if (mount_ids_.contains(mount.id.value))
                return lux::cxx::unexpected(EScriptSystemDescriptionError::DUPLICATE_MOUNT_ID);

            std::optional<lux::world::WorldObjectId> entity_object;
            if (const auto* entity = std::get_if<EntityScriptMount>(&mount.scope))
            {
                entity_object = entity->object;
                if (entity_objects_.contains(*entity_object))
                    return lux::cxx::unexpected(EScriptSystemDescriptionError::INVALID_MOUNT);
            }

            const auto mount_id = mount.id.value;
            mounts_.push_back(std::move(mount));
            try
            {
                mount_ids_.insert(mount_id);
                if (entity_object)
                    entity_objects_.insert(*entity_object);
            }
            catch (const std::length_error&)
            {
                mount_ids_.erase(mount_id);
                if (entity_object)
                    entity_objects_.erase(*entity_object);
                mounts_.pop_back();
                return lux::cxx::unexpected(EScriptSystemDescriptionError::ALLOCATION_FAILURE);
            }
            catch (const std::bad_alloc&)
            {
                mount_ids_.erase(mount_id);
                if (entity_object)
                    entity_objects_.erase(*entity_object);
                mounts_.pop_back();
                return lux::cxx::unexpected(EScriptSystemDescriptionError::ALLOCATION_FAILURE);
            }
            return {};
        }
        catch (const std::length_error&)
        {
            return lux::cxx::unexpected(EScriptSystemDescriptionError::ALLOCATION_FAILURE);
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(EScriptSystemDescriptionError::ALLOCATION_FAILURE);
        }
    }

    lux::cxx::expected<ScriptSystemDescription, EScriptSystemDescriptionError>
    ScriptSystemDescriptionBuilder::build(
        const SimulationDescription& simulation
    ) && noexcept
    {
        for (const auto& mount : mounts_)
        {
            const bool simulation_scope =
                std::holds_alternative<SimulationScriptMount>(mount.scope);
            for (const auto& binding : mount.bindings)
            {
                const auto validation = std::visit(
                    [&](const auto& target) noexcept
                    {
                        using Target = std::remove_cvref_t<decltype(target)>;
                        if constexpr (std::is_same_v<Target, HookScriptTarget>)
                        {
                            return simulation.findHookPoint(
                                    target.system,
                                    target.hook)
                                ? EScriptSystemDescriptionError::INVALID_MOUNT
                                : EScriptSystemDescriptionError::TARGET_NOT_FOUND;
                        }
                        else
                        {
                            const auto event = simulation.findEvent(
                                target.system,
                                target.event
                            );
                            if (!event)
                            {
                                return EScriptSystemDescriptionError::
                                    TARGET_NOT_FOUND;
                            }
                            if (simulation_scope && event.route() ==
                                EEventRoute::ENTITY_TARGETED)
                            {
                                return EScriptSystemDescriptionError::
                                    SCOPE_MISMATCH;
                            }
                            return EScriptSystemDescriptionError::INVALID_MOUNT;
                        }
                    },
                    binding.target
                );
                if (validation != EScriptSystemDescriptionError::INVALID_MOUNT)
                    return lux::cxx::unexpected(validation);
            }
        }
        ScriptSystemDescription result;
        result.mounts_ = std::move(mounts_);
        return result;
    }
}
