#pragma once

#include <lux/engine/authoring/script_binding/visibility.h>
#include <lux/engine/simulation/ScriptBindingCompatibility.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace lux::authoring
{
    struct ScriptBindingTargetCatalogEntry final
    {
        lux::rdesc::EScriptModel model{lux::rdesc::EScriptModel::GLOBAL_MODULE};
        lux::simulation::ScriptBindingTarget target;
        lux::simulation::ESystemHookCardinality cardinality{lux::simulation::ESystemHookCardinality::MULTI};
        std::vector<lux::rdesc::ScriptValueType> parameters;
        std::vector<lux::rdesc::ScriptValueType> returns;
    };

    enum class EScriptBindingAuthoringError : std::uint8_t
    {
        SUCCESS,
        MISSING_SYMBOL,
        MISSING_TARGET,
        AMBIGUOUS_TARGET,
        TARGET_TYPE_MISMATCH,
        SCOPE_MISMATCH,
        CARDINALITY_MISMATCH,
        SIGNATURE_MISMATCH,
        DUPLICATE_BINDING,
        SINGLE_HOOK_MULTIPLE_HANDLERS,
        INVALID_INDEX,
        ALLOCATION_FAILURE,
    };

    struct ScriptBindingDiagnostic final
    {
        EScriptBindingAuthoringError error{EScriptBindingAuthoringError::SUCCESS};
        std::size_t binding_index{};
        lux::script::ScriptSymbolId symbol{};
        std::size_t mount_index{};
    };

    struct ScriptBindingCompositionEntry final
    {
        const lux::rdesc::Script* script{};
        const lux::simulation::ScriptMountDescription* mount{};
    };

    [[nodiscard]] LUX_ENGINE_AUTHORING_SCRIPT_BINDING_PUBLIC std::vector<ScriptBindingTargetCatalogEntry>
    makeScriptBindingTargetCatalog(const lux::simulation::SimulationDescription& simulation);

    [[nodiscard]] LUX_ENGINE_AUTHORING_SCRIPT_BINDING_PUBLIC std::vector<std::size_t> compatibleScriptBindingTargets(
        const lux::rdesc::Script& script,
        lux::script::ScriptSymbolId symbol,
        const std::vector<ScriptBindingTargetCatalogEntry>& catalog
    );

    [[nodiscard]] LUX_ENGINE_AUTHORING_SCRIPT_BINDING_PUBLIC EScriptBindingAuthoringError addScriptBinding(
        const lux::simulation::SimulationDescription& simulation,
        const lux::rdesc::Script& script,
        lux::simulation::ScriptMountDescription& mount,
        lux::simulation::ScriptBindingDescription binding
    ) noexcept;

    [[nodiscard]] LUX_ENGINE_AUTHORING_SCRIPT_BINDING_PUBLIC EScriptBindingAuthoringError
    eraseScriptBinding(lux::simulation::ScriptMountDescription& mount, std::size_t binding_index) noexcept;

    [[nodiscard]] LUX_ENGINE_AUTHORING_SCRIPT_BINDING_PUBLIC EScriptBindingAuthoringError
    moveScriptBinding(lux::simulation::ScriptMountDescription& mount, std::size_t from, std::size_t to) noexcept;

    [[nodiscard]] LUX_ENGINE_AUTHORING_SCRIPT_BINDING_PUBLIC std::vector<ScriptBindingDiagnostic>
    diagnoseScriptBindings(
        const lux::simulation::SimulationDescription& simulation,
        const lux::rdesc::Script& script,
        const lux::simulation::ScriptMountDescription& mount
    );

    [[nodiscard]] LUX_ENGINE_AUTHORING_SCRIPT_BINDING_PUBLIC std::vector<ScriptBindingDiagnostic>
    diagnoseScriptBindingComposition(
        const lux::simulation::SimulationDescription& simulation,
        std::span<const ScriptBindingCompositionEntry> mounts
    );
}
