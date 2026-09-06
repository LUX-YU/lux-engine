#include <lux/engine/scene/script/ScriptRuntimeAssembly.hpp>

#include <algorithm>
#include <limits>
#include <new>

namespace lux::scene::script
{
    lux::cxx::expected<simulation::script::ScriptRuntimeCapacityPlan, simulation::script::EScriptSystemError>
    planScriptRuntimeCapacity(const ScriptSystemDescription& description) noexcept
    {
        using namespace simulation::script;
        try
        {
            ScriptRuntimeCapacityPlan result;
            result.mount_capacity = description.mounts().size();
            for (const auto& mount : description.mounts())
            {
                if (!mount.enabled)
                    continue;
                ++result.enabled_mount_capacity;
                if (mount.bindings.size() > std::numeric_limits<std::size_t>::max() - result.binding_capacity)
                    return lux::cxx::unexpected(EScriptSystemError::CAPACITY_EXCEEDED);
                result.binding_capacity += mount.bindings.size();
                for (const auto& binding : mount.bindings)
                {
                    const auto found = std::find_if(result.endpoint_capacities.begin(),
                        result.endpoint_capacities.end(),
                        [&](const auto& entry) noexcept { return entry.target == binding.target; });
                    if (found == result.endpoint_capacities.end())
                        result.endpoint_capacities.push_back({binding.target, 1U});
                    else
                        ++found->handler_capacity;
                }
            }
            if (result.mount_capacity > (std::numeric_limits<std::size_t>::max() - result.binding_capacity) / 2U)
                return lux::cxx::unexpected(EScriptSystemError::CAPACITY_EXCEEDED);
            result.method_capacity = result.binding_capacity + 2U * result.mount_capacity;
            return result;
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(EScriptSystemError::ALLOCATION_FAILURE);
        }
    }

    lux::cxx::expected<std::vector<simulation::script::ScriptRuntimeMount>, simulation::script::EScriptSystemError>
    resolveScriptRuntimeMounts(
        const ScriptSystemDescription& description,
        WorldObjectResolver resolver,
        const simulation::ecs::Registry& registry
    ) noexcept
    {
        using namespace simulation::script;
        try
        {
            std::vector<ScriptRuntimeMount> result;
            result.reserve(description.mounts().size());
            for (const auto& mount : description.mounts())
            {
                if (!mount.enabled)
                    continue;
                ScriptInstanceScope scope{SimulationScriptScope{}};
                if (const auto* object = std::get_if<EntityScriptMount>(&mount.scope))
                {
                    simulation::ecs::Entity entity{simulation::ecs::NullEntity};
                    if (resolver.resolve == nullptr || !resolver.resolve(resolver.context, object->object, entity) ||
                        entity == simulation::ecs::NullEntity || !registry.valid(entity))
                        continue;
                    scope = EntityScriptScope{entity};
                }
                const auto index = static_cast<std::uint32_t>(&mount - description.mounts().data());
                result.push_back({mount.id, mount.asset, scope, mount.bindings, index});
            }
            return result;
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(EScriptSystemError::ALLOCATION_FAILURE);
        }
    }
}
