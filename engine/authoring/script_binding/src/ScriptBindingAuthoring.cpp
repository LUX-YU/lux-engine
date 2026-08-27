#include <lux/engine/authoring/ScriptBindingAuthoring.hpp>

#include <algorithm>
#include <array>
#include <iterator>
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
            {
                const auto type = hook.parameterAt(index);
                entry.parameters.push_back({
                    std::string{type.canonical_name},
                    type.type_id,
                    type.pass});
            }
            for (std::size_t index{}; index < hook.returnCount(); ++index)
            {
                const auto type = hook.returnAt(index);
                entry.returns.push_back({
                    std::string{type.canonical_name},
                    type.type_id,
                    type.pass});
            }
        }

        [[nodiscard]] lux::simulation::SimulationHookPointView resolveHook(
            const lux::simulation::SimulationDescription& simulation,
            const lux::simulation::SystemHookBindingTarget& target,
            std::string_view& instance
        ) noexcept
        {
            lux::simulation::SimulationSystemView system;
            if (!target.system_instance.empty())
            {
                system = simulation.findSystem(target.system_instance);
                if (!system || system.type() != target.system_type)
                    return {};
            }
            else
            {
                for (std::size_t index{};
                     index < simulation.systemCount(); ++index)
                {
                    const auto candidate = simulation.systemAt(index);
                    if (candidate.type() != target.system_type)
                        continue;
                    if (system)
                        return {};
                    system = candidate;
                }
            }
            if (!system)
                return {};
            instance = system.instanceName();
            return system.findHookPoint(target.hook);
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
                        std::string{event.payloadSchemaName()},
                        event.payloadSchemaHash(),
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
                    std::string{BehaviorStopReasonCanonicalName},
                    lux::script::scriptSemanticTypeId(
                        BehaviorStopReasonCanonicalName),
                    lux::script::EScriptPassMode::VALUE});
            }
            result.push_back(std::move(entry));
        }
        return result;
    }

    std::vector<std::size_t> compatibleScriptBindingTargets(
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
                lux::simulation::evaluateScriptBindingSignatureCompatibility(
                    script.model,
                    *function,
                    catalog[index].model,
                    catalog[index].cardinality,
                    catalog[index].parameters,
                    catalog[index].returns
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
        if (const auto* hook = std::get_if<
                lux::simulation::SystemHookBindingTarget>(
                std::addressof(binding.target)))
        {
            std::string_view target_instance;
            const auto target_hook = resolveHook(
                simulation,
                *hook,
                target_instance
            );
            if (target_hook && target_hook.cardinality() ==
                lux::simulation::ESystemHookCardinality::SINGLE)
            {
                for (const auto& existing : mount.bindings)
                {
                    const auto* existing_hook = std::get_if<
                        lux::simulation::SystemHookBindingTarget>(
                        std::addressof(existing.target));
                    if (!existing_hook || existing_hook->hook != hook->hook)
                        continue;
                    std::string_view existing_instance;
                    const auto resolved = resolveHook(
                        simulation,
                        *existing_hook,
                        existing_instance
                    );
                    if (resolved && existing_instance == target_instance)
                    {
                        return EScriptBindingAuthoringError::
                            SINGLE_HOOK_MULTIPLE_HANDLERS;
                    }
                }
            }
        }
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

    std::vector<ScriptBindingDiagnostic> diagnoseScriptBindingComposition(
        const lux::simulation::SimulationDescription& simulation,
        std::span<const ScriptBindingCompositionEntry> mounts
    )
    {
        std::vector<ScriptBindingDiagnostic> result;
        struct SingleTarget final
        {
            std::string_view system_instance;
            std::string_view hook;
        };
        std::vector<SingleTarget> singles;
        for (std::size_t mount_index{};
             mount_index < mounts.size(); ++mount_index)
        {
            const auto& entry = mounts[mount_index];
            if (!entry.script || !entry.mount)
                continue;
            auto local = diagnoseScriptBindings(
                simulation,
                *entry.script,
                *entry.mount
            );
            for (auto& diagnostic : local)
                diagnostic.mount_index = mount_index;
            result.insert(
                result.end(),
                std::make_move_iterator(local.begin()),
                std::make_move_iterator(local.end())
            );
            for (std::size_t binding_index{};
                 binding_index < entry.mount->bindings.size(); ++binding_index)
            {
                const auto& binding = entry.mount->bindings[binding_index];
                const auto* target = std::get_if<
                    lux::simulation::SystemHookBindingTarget>(
                    std::addressof(binding.target));
                if (!target)
                    continue;
                std::string_view instance;
                const auto hook = resolveHook(simulation, *target, instance);
                if (!hook || hook.cardinality() !=
                    lux::simulation::ESystemHookCardinality::SINGLE)
                {
                    continue;
                }
                const auto duplicate = std::find_if(
                    singles.begin(),
                    singles.end(),
                    [&](const SingleTarget& value) noexcept
                    {
                        return value.system_instance == instance &&
                            value.hook == target->hook;
                    }
                );
                if (duplicate != singles.end())
                {
                    result.push_back({
                        EScriptBindingAuthoringError::
                            SINGLE_HOOK_MULTIPLE_HANDLERS,
                        binding_index,
                        binding.function,
                        mount_index});
                }
                else
                {
                    singles.push_back({instance, target->hook});
                }
            }
        }
        return result;
    }
}
