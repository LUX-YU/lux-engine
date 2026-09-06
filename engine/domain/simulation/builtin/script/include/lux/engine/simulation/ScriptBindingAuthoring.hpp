#pragma once
#include <lux/engine/function/script/artifact/ScriptArtifact.hpp>
#include <lux/engine/function/script/ScriptBindingHint.hpp>
#include <lux/engine/simulation/ScriptSystemDescription.hpp>
#include <lux/engine/simulation/script_system/visibility.h>
#include <vector>

namespace lux::simulation::script
{
    enum class EScriptBindingCompatibility : std::uint8_t
    {
        COMPATIBLE,
        LIFECYCLE_ONLY,
        RETURN_NOT_SUPPORTED,
        SIGNATURE_MISMATCH,
        SCOPE_MISMATCH,
        NOT_SCRIPT_CAPABLE
    };
    struct ScriptBindingCandidate final
    {
        lux::simulation::script::ScriptBindingTarget target;
        EScriptBindingCompatibility compatibility{};
        lux::script::ScriptSymbolId symbol{};
    };
    enum class EScriptBindingAuthoringError : std::uint8_t
    {
        SYMBOL_NOT_FOUND,
        TARGET_NOT_FOUND,
        INCOMPATIBLE,
        AMBIGUOUS_DEFAULT,
        NO_SELECTION,
        ALLOCATION_FAILURE
    };
    using ScriptBindingCandidates =
        lux::cxx::expected<std::vector<ScriptBindingCandidate>, EScriptBindingAuthoringError>;
    [[nodiscard]] LUX_ENGINE_SIMULATION_SCRIPT_PUBLIC ScriptBindingCandidates
    listScriptBindingCandidates(const lux::script::ScriptArtifact& artifact, lux::script::ScriptSymbolId symbol,
                                const lux::simulation::SimulationDescription& simulation, bool entity_scope) noexcept;

    // Explicit selection wins. Suggestions are authoring inputs only, never runtime auto-binding policy.
    [[nodiscard]] LUX_ENGINE_SIMULATION_SCRIPT_PUBLIC lux::cxx::expected<
        lux::simulation::script::ScriptBindingDescription, EScriptBindingAuthoringError>
    selectScriptBinding(lux::script::ScriptSymbolId symbol, std::span<const ScriptBindingCandidate> candidates,
                        const lux::simulation::script::ScriptBindingTarget* explicit_target,
                        std::span<const lux::simulation::script::ScriptBindingTarget> suggestions) noexcept;

    [[nodiscard]] LUX_ENGINE_SIMULATION_SCRIPT_PUBLIC lux::cxx::expected<
        ScriptBindingDescription, EScriptBindingAuthoringError>
    selectScriptBindingFromHints(const lux::script::ScriptArtifact& artifact, lux::script::ScriptSymbolId symbol,
        const SimulationDescription& simulation, bool entity_scope, const ScriptBindingTarget* explicit_target,
        std::span<const lux::script::ScriptBindingHint> hints) noexcept;
} // namespace lux::simulation::script
