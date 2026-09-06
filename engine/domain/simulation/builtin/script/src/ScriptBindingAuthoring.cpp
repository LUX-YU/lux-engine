#include <lux/engine/simulation/ScriptBindingAuthoring.hpp>
#include <lux/engine/simulation/scripting/ScriptSignatureCompatibility.hpp>

#include <algorithm>
#include <new>
#include <optional>

namespace lux::simulation::script
{
    ScriptBindingCandidates listScriptBindingCandidates(const lux::script::ScriptArtifact& artifact,
                                                        lux::script::ScriptSymbolId symbol,
                                                        const lux::simulation::SimulationDescription& simulation,
                                                        bool entity_scope) noexcept
    {
        using namespace lux::simulation;
        using namespace lux::simulation::script;
        const auto* function = artifact.findExport(symbol);
        if (function == nullptr)
            return lux::cxx::unexpected(EScriptBindingAuthoringError::SYMBOL_NOT_FOUND);
        const auto& lifecycle = artifact.description().lifecycle;
        const bool is_lifecycle = lifecycle.begin_play == symbol || lifecycle.end_play == symbol;
        try
        {
            std::vector<ScriptBindingCandidate> result;
            for (std::size_t system_index{}; system_index < simulation.systemCount(); ++system_index)
            {
                const auto system = simulation.systemAt(system_index);
                for (std::size_t hook_index{}; hook_index < system.hookPointCount(); ++hook_index)
                {
                    const auto hook = system.hookPointAt(hook_index);
                    auto compatibility = EScriptBindingCompatibility::COMPATIBLE;
                    if (is_lifecycle)
                        compatibility = EScriptBindingCompatibility::LIFECYCLE_ONLY;
                    else if (!hook.scriptCapable())
                        compatibility = EScriptBindingCompatibility::NOT_SCRIPT_CAPABLE;
                    else if (!function->returns.empty())
                        compatibility = EScriptBindingCompatibility::RETURN_NOT_SUPPORTED;
                    else if (!sameScriptHookSignature(
                                 *function, hook.parameterCount(),
                                 [hook](std::size_t index) noexcept { return hook.parameterAt(index); }))
                        compatibility = EScriptBindingCompatibility::SIGNATURE_MISMATCH;
                    result.push_back({HookScriptTarget{system.instanceId(), hook.id()}, compatibility, symbol});
                }
                for (std::size_t event_index{}; event_index < system.eventCount(); ++event_index)
                {
                    const auto event = system.eventAt(event_index);
                    auto compatibility = EScriptBindingCompatibility::COMPATIBLE;
                    if (is_lifecycle)
                        compatibility = EScriptBindingCompatibility::LIFECYCLE_ONLY;
                    else if (!event.dispatchHook().scriptCapable())
                        compatibility = EScriptBindingCompatibility::NOT_SCRIPT_CAPABLE;
                    else if (event.route() == EEventRoute::ENTITY_TARGETED && !entity_scope)
                        compatibility = EScriptBindingCompatibility::SCOPE_MISMATCH;
                    else if (!function->returns.empty())
                        compatibility = EScriptBindingCompatibility::RETURN_NOT_SUPPORTED;
                    else if (!sameScriptEventSignature(*function, {event.payloadType(), event.payloadSchemaName(),
                                                                   lux::semantic::EValuePass::CONST_REF}))
                        compatibility = EScriptBindingCompatibility::SIGNATURE_MISMATCH;
                    result.push_back({EventScriptTarget{system.instanceId(), event.id()}, compatibility, symbol});
                }
            }
            return result;
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(EScriptBindingAuthoringError::ALLOCATION_FAILURE);
        }
    }

    lux::cxx::expected<lux::simulation::script::ScriptBindingDescription, EScriptBindingAuthoringError>
    selectScriptBinding(lux::script::ScriptSymbolId symbol, std::span<const ScriptBindingCandidate> candidates,
                        const lux::simulation::script::ScriptBindingTarget* explicit_target,
                        std::span<const lux::simulation::script::ScriptBindingTarget> suggestions) noexcept
    {
        using namespace lux::simulation::script;
        if (std::ranges::none_of(candidates, [symbol](const auto& value) noexcept { return value.symbol == symbol; }))
            return lux::cxx::unexpected(EScriptBindingAuthoringError::SYMBOL_NOT_FOUND);
        std::optional<ScriptBindingTarget> selected;
        const auto inspect = [&](const ScriptBindingTarget& target) -> std::optional<EScriptBindingAuthoringError> {
            const auto found = std::ranges::find(candidates, target, &ScriptBindingCandidate::target);
            if (found == candidates.end())
                return EScriptBindingAuthoringError::TARGET_NOT_FOUND;
            if (found->symbol != symbol)
                return EScriptBindingAuthoringError::SYMBOL_NOT_FOUND;
            if (found->compatibility != EScriptBindingCompatibility::COMPATIBLE)
                return EScriptBindingAuthoringError::INCOMPATIBLE;
            if (selected && *selected != target)
                return EScriptBindingAuthoringError::AMBIGUOUS_DEFAULT;
            selected = target;
            return {};
        };
        if (explicit_target != nullptr)
        {
            if (const auto error = inspect(*explicit_target))
                return lux::cxx::unexpected(*error);
        }
        else
        {
            for (const auto& target : suggestions)
                if (const auto error = inspect(target))
                    return lux::cxx::unexpected(*error);
        }
        if (!selected)
            return lux::cxx::unexpected(EScriptBindingAuthoringError::NO_SELECTION);
        return ScriptBindingDescription{symbol, *selected};
    }
    lux::cxx::expected<ScriptBindingDescription, EScriptBindingAuthoringError> selectScriptBindingFromHints(
        const lux::script::ScriptArtifact& artifact, lux::script::ScriptSymbolId symbol,
        const SimulationDescription& simulation, bool entity_scope, const ScriptBindingTarget* explicit_target,
        std::span<const lux::script::ScriptBindingHint> hints) noexcept
    {
        auto candidates = listScriptBindingCandidates(artifact, symbol, simulation, entity_scope);
        if (!candidates) return lux::cxx::unexpected(candidates.error());
        // A retained user choice does not become invalid because a source suggestion changed or disappeared.
        if (explicit_target != nullptr) return selectScriptBinding(symbol, *candidates, explicit_target, {});
        try
        {
            std::vector<ScriptBindingTarget> targets;
            for (const auto& hint : hints)
            {
                if (hint.symbol != symbol) continue;
                const std::string_view qualified = hint.target.qualified_name;
                const auto separator = qualified.find('.');
                const bool invalid_name = separator == std::string_view::npos || separator == 0U ||
                    separator + 1U == qualified.size() || qualified.find('.', separator + 1U) != std::string_view::npos;
                if (invalid_name) return lux::cxx::unexpected(EScriptBindingAuthoringError::TARGET_NOT_FOUND);
                const auto system_name = qualified.substr(0U, separator);
                const auto point_name = qualified.substr(separator + 1U);
                const auto before = targets.size();
                for (std::size_t index{}; index < simulation.systemCount(); ++index)
                {
                    const auto system = simulation.systemAt(index);
                    if (system.instanceName() != system_name) continue;
                    if (hint.target.kind == lux::script::EScriptBindingHintKind::HOOK)
                    {
                        for (std::size_t point{}; point < system.hookPointCount(); ++point)
                        {
                            const auto hook = system.hookPointAt(point);
                            if (hook.name() == point_name)
                                targets.emplace_back(HookScriptTarget{system.instanceId(), hook.id()});
                        }
                    }
                    else if (hint.target.kind == lux::script::EScriptBindingHintKind::EVENT)
                    {
                        for (std::size_t point{}; point < system.eventCount(); ++point)
                        {
                            const auto event = system.eventAt(point);
                            if (event.name() == point_name)
                                targets.emplace_back(EventScriptTarget{system.instanceId(), event.id()});
                        }
                    }
                }
                if (targets.size() == before)
                    return lux::cxx::unexpected(EScriptBindingAuthoringError::TARGET_NOT_FOUND);
            }
            return selectScriptBinding(symbol, *candidates, nullptr, targets);
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(EScriptBindingAuthoringError::ALLOCATION_FAILURE);
        }
    }
} // namespace lux::simulation::script
