#include <lux/engine/authoring/script/ScriptAuthoring.hpp>

#include <algorithm>
#include <new>
#include <type_traits>

namespace lux::authoring::script
{
    namespace
    {
        [[nodiscard]] const lux::rdesc::ScriptFunction* findFunction(
            const lux::rdesc::Script& asset,
            lux::script::ScriptSymbolId symbol
        ) noexcept
        {
            const auto found = std::find_if(
                asset.exports.begin(),
                asset.exports.end(),
                [symbol](const auto& function) noexcept
                {
                    return function.symbol_id == symbol;
                });
            return found == asset.exports.end()
                ? nullptr
                : std::addressof(*found);
        }

        [[nodiscard]] bool sameType(
            const lux::rdesc::ScriptValueType& script_type,
            lux::semantic::Type endpoint_type
        ) noexcept
        {
            return script_type.type_id == endpoint_type.type_id &&
                script_type.canonical_name == endpoint_type.canonical_name &&
                static_cast<std::uint8_t>(script_type.pass) ==
                    static_cast<std::uint8_t>(endpoint_type.pass);
        }

        [[nodiscard]] EScriptAuthoringError validateBinding(
            const lux::simulation::SimulationDescription& simulation,
            const lux::rdesc::ScriptFunction& function,
            const lux::simulation::script::ScriptMountDescription& mount,
            const lux::simulation::script::ScriptBindingTarget& target
        ) noexcept
        {
            using namespace lux::simulation;
            using namespace lux::simulation::script;
            if (const auto* hook = std::get_if<HookScriptTarget>(&target))
            {
                const auto endpoint = simulation.findHookPoint(
                    hook->system,
                    hook->hook);
                if (!endpoint)
                    return EScriptAuthoringError::MISSING_TARGET;
                return compatibleHookBinding(function, endpoint)
                    ? EScriptAuthoringError::SUCCESS
                    : EScriptAuthoringError::SIGNATURE_MISMATCH;
            }
            const auto& event_target = std::get<EventScriptTarget>(target);
            const auto endpoint = simulation.findEvent(
                event_target.system,
                event_target.event);
            if (!endpoint)
                return EScriptAuthoringError::MISSING_TARGET;
            if (std::holds_alternative<SimulationScriptMount>(mount.scope) &&
                endpoint.route() == EEventRoute::ENTITY_TARGETED)
            {
                return EScriptAuthoringError::SCOPE_MISMATCH;
            }
            return compatibleEventBinding(function, endpoint)
                ? EScriptAuthoringError::SUCCESS
                : EScriptAuthoringError::SIGNATURE_MISMATCH;
        }
    }

    bool compatibleHookBinding(
        const lux::rdesc::ScriptFunction& function,
        lux::simulation::SimulationHookPointView hook
    ) noexcept
    {
        if (!hook || !function.returns.empty() ||
            function.args.size() != hook.parameterCount())
        {
            return false;
        }
        for (std::size_t index{}; index < function.args.size(); ++index)
        {
            if (!sameType(function.args[index], hook.parameterAt(index)))
                return false;
        }
        return true;
    }

    bool compatibleEventBinding(
        const lux::rdesc::ScriptFunction& function,
        lux::simulation::SimulationEventView event
    ) noexcept
    {
        return event && function.returns.empty() &&
            function.args.size() == 1U &&
            function.args[0].type_id == event.payloadType() &&
            function.args[0].canonical_name == event.payloadSchemaName() &&
            function.args[0].pass ==
                lux::script::EScriptPassMode::CONST_REF;
    }

    std::vector<lux::simulation::script::HookScriptTarget>
    compatibleHookTargets(
        const lux::simulation::SimulationDescription& simulation,
        const lux::rdesc::ScriptFunction& function
    )
    {
        std::vector<lux::simulation::script::HookScriptTarget> result;
        for (std::size_t system_index{};
             system_index < simulation.systemCount(); ++system_index)
        {
            const auto system = simulation.systemAt(system_index);
            for (std::size_t hook_index{};
                 hook_index < system.hookPointCount(); ++hook_index)
            {
                const auto hook = system.hookPointAt(hook_index);
                if (compatibleHookBinding(function, hook))
                    result.push_back({system.instanceId(), hook.id()});
            }
        }
        return result;
    }

    std::vector<lux::simulation::script::EventScriptTarget>
    compatibleEventTargets(
        const lux::simulation::SimulationDescription& simulation,
        const lux::rdesc::ScriptFunction& function,
        bool simulation_scope
    )
    {
        std::vector<lux::simulation::script::EventScriptTarget> result;
        for (std::size_t system_index{};
             system_index < simulation.systemCount(); ++system_index)
        {
            const auto system = simulation.systemAt(system_index);
            for (std::size_t event_index{};
                 event_index < system.eventCount(); ++event_index)
            {
                const auto event = system.eventAt(event_index);
                if ((!simulation_scope || event.route() !=
                        lux::simulation::EEventRoute::ENTITY_TARGETED) &&
                    compatibleEventBinding(function, event))
                {
                    result.push_back({system.instanceId(), event.id()});
                }
            }
        }
        return result;
    }

    EScriptAuthoringError addBinding(
        const lux::simulation::SimulationDescription& simulation,
        const lux::rdesc::Script& asset,
        lux::simulation::script::ScriptMountDescription& mount,
        lux::simulation::script::ScriptBindingDescription binding
    ) noexcept
    {
        const auto* function = findFunction(asset, binding.symbol);
        if (!function)
            return EScriptAuthoringError::MISSING_SYMBOL;
        if (std::find(
                mount.bindings.begin(),
                mount.bindings.end(),
                binding) != mount.bindings.end())
        {
            return EScriptAuthoringError::DUPLICATE_BINDING;
        }
        const auto validation = validateBinding(
            simulation,
            *function,
            mount,
            binding.target);
        if (validation != EScriptAuthoringError::SUCCESS)
            return validation;
        try
        {
            mount.bindings.push_back(std::move(binding));
            return EScriptAuthoringError::SUCCESS;
        }
        catch (const std::bad_alloc&)
        {
            return EScriptAuthoringError::ALLOCATION_FAILURE;
        }
    }

    EScriptAuthoringError eraseBinding(
        lux::simulation::script::ScriptMountDescription& mount,
        std::size_t binding_index
    ) noexcept
    {
        if (binding_index >= mount.bindings.size())
            return EScriptAuthoringError::INVALID_INDEX;
        mount.bindings.erase(mount.bindings.begin() + binding_index);
        return EScriptAuthoringError::SUCCESS;
    }

    EScriptAuthoringError moveBinding(
        lux::simulation::script::ScriptMountDescription& mount,
        std::size_t from,
        std::size_t to
    ) noexcept
    {
        if (from >= mount.bindings.size() || to >= mount.bindings.size())
            return EScriptAuthoringError::INVALID_INDEX;
        if (from == to)
            return EScriptAuthoringError::SUCCESS;
        auto binding = std::move(mount.bindings[from]);
        mount.bindings.erase(mount.bindings.begin() + from);
        mount.bindings.insert(
            mount.bindings.begin() + to,
            std::move(binding));
        return EScriptAuthoringError::SUCCESS;
    }

    std::vector<ScriptBindingDiagnostic> diagnoseBindings(
        const lux::simulation::SimulationDescription& simulation,
        const lux::rdesc::Script& asset,
        const lux::simulation::script::ScriptMountDescription& mount
    )
    {
        std::vector<ScriptBindingDiagnostic> result;
        for (std::size_t index{}; index < mount.bindings.size(); ++index)
        {
            const auto& binding = mount.bindings[index];
            const auto* function = findFunction(asset, binding.symbol);
            const auto error = function
                ? validateBinding(simulation, *function, mount, binding.target)
                : EScriptAuthoringError::MISSING_SYMBOL;
            if (error != EScriptAuthoringError::SUCCESS)
                result.push_back({error, index, binding.symbol});
        }
        return result;
    }
}
