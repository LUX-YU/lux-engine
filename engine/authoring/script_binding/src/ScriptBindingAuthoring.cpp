#include <lux/engine/authoring/ScriptBindingAuthoring.hpp>

#include <algorithm>
#include <array>
#include <new>
#include <type_traits>

namespace lux::authoring
{
    namespace
    {
        [[nodiscard]] const lux::rdesc::ScriptFunction* findFunction(
            const lux::rdesc::Script& script,
            lux::script::ScriptSymbolId symbol
        ) noexcept
        {
            const auto found = std::find_if(
                script.exports.begin(),
                script.exports.end(),
                [symbol](const auto& function) noexcept
                {
                    return function.symbol_id == symbol;
                }
            );
            return found == script.exports.end()
                ? nullptr
                : std::addressof(*found);
        }

        [[nodiscard]] EScriptBindingAuthoringError mapCompatibility(
            lux::simulation::EScriptBindingCompatibility value
        ) noexcept
        {
            using Source = lux::simulation::EScriptBindingCompatibility;
            switch (value)
            {
            case Source::COMPATIBLE:
                return EScriptBindingAuthoringError::SUCCESS;
            case Source::TARGET_NOT_FOUND:
                return EScriptBindingAuthoringError::MISSING_TARGET;
            case Source::TARGET_AMBIGUOUS:
                return EScriptBindingAuthoringError::AMBIGUOUS_TARGET;
            case Source::TARGET_TYPE_MISMATCH:
                return EScriptBindingAuthoringError::TARGET_TYPE_MISMATCH;
            case Source::SCOPE_MISMATCH:
                return EScriptBindingAuthoringError::SCOPE_MISMATCH;
            case Source::CARDINALITY_MISMATCH:
                return EScriptBindingAuthoringError::CARDINALITY_MISMATCH;
            case Source::INVALID_FUNCTION:
            case Source::SIGNATURE_MISMATCH:
                return EScriptBindingAuthoringError::SIGNATURE_MISMATCH;
            }
            return EScriptBindingAuthoringError::SIGNATURE_MISMATCH;
        }

        void copyHookSignature(
            lux::simulation::SimulationHookPointView hook,
            ScriptBindingTargetCatalogEntry& entry
        )
        {
            entry.parameters.reserve(hook.parameterCount());
            entry.returns.reserve(hook.returnCount());
            for (std::size_t index{}; index < hook.parameterCount(); ++index)
                entry.parameters.push_back(hook.parameterAt(index));
            for (std::size_t index{}; index < hook.returnCount(); ++index)
                entry.returns.push_back(hook.returnAt(index));
        }
    }

    std::vector<ScriptBindingTargetCatalogEntry> makeScriptBindingTargetCatalog(
        const lux::simulation::SimulationDescription& simulation
    )
    {
        using namespace lux::simulation;
        std::vector<ScriptBindingTargetCatalogEntry> result;
        for (std::size_t system_index{};
             system_index < simulation.systemCount(); ++system_index)
        {
            const auto system = simulation.systemAt(system_index);
            for (std::size_t hook_index{};
                 hook_index < system.hookPointCount(); ++hook_index)
            {
                const auto hook = system.hookPointAt(hook_index);
                ScriptBindingTargetCatalogEntry global;
                global.model = lux::rdesc::EScriptModel::GLOBAL_MODULE;
                global.target = SystemHookBindingTarget{
                    system.type(),
                    std::string{system.instanceName()},
                    std::string{hook.name()}};
                global.cardinality = hook.cardinality();
                copyHookSignature(hook, global);
                result.push_back(global);
                if (hook.cardinality() == ESystemHookCardinality::MULTI)
                {
                    global.model = lux::rdesc::EScriptModel::ENTITY_BEHAVIOR;
                    result.push_back(std::move(global));
                }
            }
            for (std::size_t event_index{};
                 event_index < system.eventCount(); ++event_index)
            {
                const auto event = system.eventAt(event_index);
                ScriptBindingTargetCatalogEntry entry;
                entry.model = event.target() == ESystemEventTarget::GLOBAL
                    ? lux::rdesc::EScriptModel::GLOBAL_MODULE
                    : lux::rdesc::EScriptModel::ENTITY_BEHAVIOR;
                entry.target = SystemEventBindingTarget{
                    system.type(),
                    std::string{system.instanceName()},
                    std::string{event.name()}};
                if (!event.payloadSchemaName().empty())
                {
                    entry.parameters.push_back({
                        event.payloadSchemaHash(),
                        event.payloadSchemaName(),
                        lux::script::EScriptPassMode::CONST_REF});
                }
                result.push_back(std::move(entry));
            }
        }

        for (const auto point : {
                 EBehaviorLifecyclePoint::CONSTRUCT,
                 EBehaviorLifecyclePoint::START,
                 EBehaviorLifecyclePoint::STOP})
        {
            ScriptBindingTargetCatalogEntry entry;
            entry.model = lux::rdesc::EScriptModel::ENTITY_BEHAVIOR;
            entry.target = BehaviorLifecycleBindingTarget{point};
            if (point == EBehaviorLifecyclePoint::STOP)
            {
                entry.parameters.push_back({
                    lux::script::scriptSemanticTypeId(
                        BehaviorStopReasonCanonicalName),
                    BehaviorStopReasonCanonicalName,
                    lux::script::EScriptPassMode::VALUE});
            }
            result.push_back(std::move(entry));
        }
        return result;
    }

    std::vector<std::size_t> compatibleScriptBindingTargets(
        const lux::simulation::SimulationDescription& simulation,
        const lux::rdesc::Script& script,
        lux::script::ScriptSymbolId symbol,
        const std::vector<ScriptBindingTargetCatalogEntry>& catalog
    )
    {
        std::vector<std::size_t> result;
        const auto* function = findFunction(script, symbol);
        if (!function)
            return result;
        for (std::size_t index{}; index < catalog.size(); ++index)
        {
            if (catalog[index].model == script.model &&
                lux::simulation::evaluateScriptBindingCompatibility(
                    simulation,
                    script.model,
                    *function,
                    catalog[index].target
                ) == lux::simulation::EScriptBindingCompatibility::COMPATIBLE)
            {
                result.push_back(index);
            }
        }
        return result;
    }

    EScriptBindingAuthoringError addScriptBinding(
        const lux::simulation::SimulationDescription& simulation,
        const lux::rdesc::Script& script,
        lux::simulation::ScriptMountDescription& mount,
        lux::simulation::ScriptBindingDescription binding
    ) noexcept
    {
        const auto* function = findFunction(script, binding.function);
        if (!function)
            return EScriptBindingAuthoringError::MISSING_SYMBOL;
        if (std::find(mount.bindings.begin(), mount.bindings.end(), binding) !=
            mount.bindings.end())
        {
            return EScriptBindingAuthoringError::DUPLICATE_BINDING;
        }
        const auto compatible = mapCompatibility(
            lux::simulation::evaluateScriptBindingCompatibility(
                simulation,
                script.model,
                *function,
                binding.target
            )
        );
        if (compatible != EScriptBindingAuthoringError::SUCCESS)
            return compatible;
        try
        {
            mount.bindings.push_back(std::move(binding));
            return EScriptBindingAuthoringError::SUCCESS;
        }
        catch (const std::bad_alloc&)
        {
            return EScriptBindingAuthoringError::ALLOCATION_FAILURE;
        }
    }

    EScriptBindingAuthoringError eraseScriptBinding(
        lux::simulation::ScriptMountDescription& mount,
        std::size_t binding_index
    ) noexcept
    {
        if (binding_index >= mount.bindings.size())
            return EScriptBindingAuthoringError::INVALID_INDEX;
        mount.bindings.erase(mount.bindings.begin() + binding_index);
        return EScriptBindingAuthoringError::SUCCESS;
    }

    EScriptBindingAuthoringError moveScriptBinding(
        lux::simulation::ScriptMountDescription& mount,
        std::size_t from,
        std::size_t to
    ) noexcept
    {
        if (from >= mount.bindings.size() || to >= mount.bindings.size())
            return EScriptBindingAuthoringError::INVALID_INDEX;
        if (from == to)
            return EScriptBindingAuthoringError::SUCCESS;
        auto value = std::move(mount.bindings[from]);
        mount.bindings.erase(mount.bindings.begin() + from);
        mount.bindings.insert(mount.bindings.begin() + to, std::move(value));
        return EScriptBindingAuthoringError::SUCCESS;
    }

    std::vector<ScriptBindingDiagnostic> diagnoseScriptBindings(
        const lux::simulation::SimulationDescription& simulation,
        const lux::rdesc::Script& script,
        const lux::simulation::ScriptMountDescription& mount
    )
    {
        std::vector<ScriptBindingDiagnostic> result;
        for (std::size_t index{}; index < mount.bindings.size(); ++index)
        {
            const auto& binding = mount.bindings[index];
            const auto* function = findFunction(script, binding.function);
            const auto error = function
                ? mapCompatibility(
                    lux::simulation::evaluateScriptBindingCompatibility(
                        simulation,
                        script.model,
                        *function,
                        binding.target
                    ))
                : EScriptBindingAuthoringError::MISSING_SYMBOL;
            if (error != EScriptBindingAuthoringError::SUCCESS)
                result.push_back({error, index, binding.function});
        }
        return result;
    }
}
