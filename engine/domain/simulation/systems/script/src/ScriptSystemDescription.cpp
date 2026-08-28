#include <lux/engine/simulation/systems/ScriptSystemDescription.hpp>

#include <algorithm>
#include <new>
#include <type_traits>

namespace lux::simulation::script
{
    const ScriptMountDescription* ScriptSystemDescription::findMount(
        ScriptMountId id
    ) const noexcept
    {
        const auto found = std::find_if(
            mounts_.begin(),
            mounts_.end(),
            [id](const auto& mount) noexcept { return mount.id == id; }
        );
        return found == mounts_.end() ? nullptr : std::addressof(*found);
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
        for (std::size_t index{}; index < mount.bindings.size(); ++index)
        {
            const auto& binding = mount.bindings[index];
            if (binding.symbol == lux::script::InvalidScriptSymbolId ||
                !std::visit(
                    [](const auto& target) noexcept
                    {
                        using Target = std::remove_cvref_t<decltype(target)>;
                        if constexpr (std::is_same_v<Target, HookScriptTarget>)
                            return target.system.valid() && target.hook.valid();
                        else
                            return target.system.valid() && target.event.valid();
                    },
                    binding.target))
            {
                return lux::cxx::unexpected(
                    EScriptSystemDescriptionError::INVALID_MOUNT);
            }
            if (std::find(
                    mount.bindings.begin(),
                    mount.bindings.begin() + index,
                    binding) != mount.bindings.begin() + index)
            {
                return lux::cxx::unexpected(
                    EScriptSystemDescriptionError::DUPLICATE_BINDING);
            }
        }
        if (std::any_of(
                mounts_.begin(),
                mounts_.end(),
                [&](const auto& existing) noexcept
                {
                    return existing.id == mount.id;
                }))
        {
            return lux::cxx::unexpected(
                EScriptSystemDescriptionError::DUPLICATE_MOUNT_ID);
        }
        if (const auto* entity = std::get_if<EntityScriptMount>(&mount.scope);
            entity != nullptr && std::any_of(
                mounts_.begin(),
                mounts_.end(),
                [&](const auto& existing) noexcept
                {
                    const auto* existing_entity =
                        std::get_if<EntityScriptMount>(&existing.scope);
                    return existing_entity &&
                        existing_entity->object == entity->object;
                }))
        {
            return lux::cxx::unexpected(
                EScriptSystemDescriptionError::INVALID_MOUNT);
        }
        try
        {
            mounts_.push_back(std::move(mount));
            return {};
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(
                EScriptSystemDescriptionError::ALLOCATION_FAILURE);
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
