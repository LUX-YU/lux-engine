#include <lux/engine/simulation/ScriptBindingCompatibility.hpp>

#include <type_traits>

namespace lux::simulation
{
    namespace
    {
        [[nodiscard]] bool sameType(
            const lux::rdesc::ScriptValueType& script_type,
            const lux::script::ScriptSemanticType& target_type
        ) noexcept
        {
            return script_type.type_id == target_type.type_id &&
                script_type.canonical_name == target_type.canonical_name &&
                script_type.pass == target_type.pass;
        }

        [[nodiscard]] bool sameHookSignature(
            const lux::rdesc::ScriptFunction& function,
            const SimulationHookPointView& hook
        ) noexcept
        {
            if (function.args.size() != hook.parameterCount() ||
                function.returns.size() != hook.returnCount())
                return false;
            for (std::size_t index{}; index < function.args.size(); ++index)
            {
                if (!sameType(function.args[index], hook.parameterAt(index)))
                    return false;
            }
            for (std::size_t index{}; index < function.returns.size(); ++index)
            {
                if (!sameType(function.returns[index], hook.returnAt(index)))
                    return false;
            }
            return true;
        }

        [[nodiscard]] bool sameEventSignature(
            const lux::rdesc::ScriptFunction& function,
            const SimulationEventView& event
        ) noexcept
        {
            if (!function.returns.empty())
                return false;
            if (event.payloadSchemaName().empty())
                return function.args.empty();
            return function.args.size() == 1U &&
                function.args.front().canonical_name ==
                    event.payloadSchemaName() &&
                function.args.front().type_id == event.payloadSchemaHash() &&
                function.args.front().pass ==
                    lux::script::EScriptPassMode::CONST_REF;
        }

        [[nodiscard]] bool sameLifecycleSignature(
            const lux::rdesc::ScriptFunction& function,
            EBehaviorLifecyclePoint point
        ) noexcept
        {
            if (!function.returns.empty())
                return false;
            if (point != EBehaviorLifecyclePoint::STOP)
                return function.args.empty();
            return function.args.size() == 1U &&
                function.args.front().canonical_name ==
                    BehaviorStopReasonCanonicalName &&
                function.args.front().type_id ==
                    lux::script::scriptSemanticTypeId(
                        BehaviorStopReasonCanonicalName) &&
                function.args.front().pass ==
                    lux::script::EScriptPassMode::VALUE;
        }

        [[nodiscard]] EScriptBindingCompatibility resolveSystem(
            const SimulationDescription& simulation,
            const SystemTypeId& type,
            std::string_view instance,
            SimulationSystemView& result
        ) noexcept
        {
            if (!instance.empty())
            {
                result = simulation.findSystem(instance);
                if (!result)
                    return EScriptBindingCompatibility::TARGET_NOT_FOUND;
                return result.type() == type
                    ? EScriptBindingCompatibility::COMPATIBLE
                    : EScriptBindingCompatibility::TARGET_TYPE_MISMATCH;
            }
            for (std::size_t index{}; index < simulation.systemCount(); ++index)
            {
                const auto candidate = simulation.systemAt(index);
                if (candidate.type() != type)
                    continue;
                if (result)
                    return EScriptBindingCompatibility::TARGET_AMBIGUOUS;
                result = candidate;
            }
            return result
                ? EScriptBindingCompatibility::COMPATIBLE
                : EScriptBindingCompatibility::TARGET_NOT_FOUND;
        }
    }

    EScriptBindingCompatibility evaluateScriptBindingSignatureCompatibility(
        EScriptAttachmentScope source_scope,
        const lux::rdesc::ScriptFunction& function,
        EScriptAttachmentScope target_scope,
        ESystemHookCardinality cardinality,
        std::span<const lux::rdesc::ScriptValueType> parameters,
        std::span<const lux::rdesc::ScriptValueType> returns
    ) noexcept
    {
        if (function.name.empty() ||
            function.symbol_id == lux::script::InvalidScriptSymbolId)
        {
            return EScriptBindingCompatibility::INVALID_FUNCTION;
        }
        if (source_scope != target_scope)
            return EScriptBindingCompatibility::SCOPE_MISMATCH;
        if (source_scope == EScriptAttachmentScope::ENTITY &&
            cardinality == ESystemHookCardinality::SINGLE)
        {
            return EScriptBindingCompatibility::CARDINALITY_MISMATCH;
        }
        if (function.args.size() != parameters.size() ||
            function.returns.size() != returns.size())
        {
            return EScriptBindingCompatibility::SIGNATURE_MISMATCH;
        }
        for (std::size_t index{}; index < parameters.size(); ++index)
        {
            if (function.args[index] != parameters[index])
                return EScriptBindingCompatibility::SIGNATURE_MISMATCH;
        }
        for (std::size_t index{}; index < returns.size(); ++index)
        {
            if (function.returns[index] != returns[index])
                return EScriptBindingCompatibility::SIGNATURE_MISMATCH;
        }
        return EScriptBindingCompatibility::COMPATIBLE;
    }

    EScriptBindingCompatibility evaluateScriptBindingCompatibility(
        const SimulationDescription& simulation,
        EScriptAttachmentScope scope,
        const lux::rdesc::ScriptFunction& function,
        const ScriptBindingTarget& target
    ) noexcept
    {
        if (function.name.empty() ||
            function.symbol_id == lux::script::InvalidScriptSymbolId)
            return EScriptBindingCompatibility::INVALID_FUNCTION;
        const bool entity = scope == EScriptAttachmentScope::ENTITY;
        if (!entity && scope != EScriptAttachmentScope::SIMULATION)
            return EScriptBindingCompatibility::SCOPE_MISMATCH;
        return std::visit(
            [&](const auto& concrete) noexcept
            {
                using Target = std::remove_cvref_t<decltype(concrete)>;
                if constexpr (std::is_same_v<Target, SystemHookBindingTarget>)
                {
                    SimulationSystemView system;
                    const auto resolved = resolveSystem(
                        simulation,
                        concrete.system_type,
                        concrete.system_instance,
                        system
                    );
                    if (resolved != EScriptBindingCompatibility::COMPATIBLE)
                        return resolved;
                    const auto hook = system.findHookPoint(concrete.hook);
                    if (!hook)
                        return EScriptBindingCompatibility::TARGET_NOT_FOUND;
                    if (entity && hook.cardinality() !=
                        ESystemHookCardinality::MULTI)
                    {
                        return EScriptBindingCompatibility::CARDINALITY_MISMATCH;
                    }
                    return sameHookSignature(function, hook)
                        ? EScriptBindingCompatibility::COMPATIBLE
                        : EScriptBindingCompatibility::SIGNATURE_MISMATCH;
                }
                else if constexpr (
                    std::is_same_v<Target, SystemEventBindingTarget>)
                {
                    SimulationSystemView system;
                    const auto resolved = resolveSystem(
                        simulation,
                        concrete.system_type,
                        concrete.system_instance,
                        system
                    );
                    if (resolved != EScriptBindingCompatibility::COMPATIBLE)
                        return resolved;
                    const auto event = system.findEvent(concrete.event);
                    if (!event)
                        return EScriptBindingCompatibility::TARGET_NOT_FOUND;
                    if (entity != (event.target() ==
                        ESystemEventTarget::ENTITY_TARGETED))
                    {
                        return EScriptBindingCompatibility::SCOPE_MISMATCH;
                    }
                    return sameEventSignature(function, event)
                        ? EScriptBindingCompatibility::COMPATIBLE
                        : EScriptBindingCompatibility::SIGNATURE_MISMATCH;
                }
                else
                {
                    if (!entity)
                        return EScriptBindingCompatibility::SCOPE_MISMATCH;
                    return sameLifecycleSignature(function, concrete.point)
                        ? EScriptBindingCompatibility::COMPATIBLE
                        : EScriptBindingCompatibility::SIGNATURE_MISMATCH;
                }
            },
            target
        );
    }
}
