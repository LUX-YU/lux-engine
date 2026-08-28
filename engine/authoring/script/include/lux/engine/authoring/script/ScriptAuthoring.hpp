#pragma once

#include <lux/engine/authoring/script/visibility.h>
#include <lux/engine/description/Script.hpp>
#include <lux/engine/simulation/SimulationDescription.hpp>
#include <lux/engine/simulation/systems/ScriptSystemDescription.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace lux::authoring::script
{
    enum class EScriptAuthoringError : std::uint8_t
    {
        SUCCESS,
        MISSING_SYMBOL,
        MISSING_TARGET,
        SCOPE_MISMATCH,
        SIGNATURE_MISMATCH,
        DUPLICATE_BINDING,
        INVALID_INDEX,
        ALLOCATION_FAILURE,
    };

    struct ScriptBindingDiagnostic final
    {
        EScriptAuthoringError error{EScriptAuthoringError::SUCCESS};
        std::size_t binding_index{};
        lux::script::ScriptSymbolId symbol{};
    };

    [[nodiscard]] LUX_ENGINE_AUTHORING_SCRIPT_PUBLIC
    bool compatibleHookBinding(
        const lux::rdesc::ScriptFunction& function,
        lux::simulation::SimulationHookPointView hook
    ) noexcept;

    [[nodiscard]] LUX_ENGINE_AUTHORING_SCRIPT_PUBLIC
    bool compatibleEventBinding(
        const lux::rdesc::ScriptFunction& function,
        lux::simulation::SimulationEventView event
    ) noexcept;

    [[nodiscard]] LUX_ENGINE_AUTHORING_SCRIPT_PUBLIC
    std::vector<lux::simulation::script::HookScriptTarget>
    compatibleHookTargets(
        const lux::simulation::SimulationDescription& simulation,
        const lux::rdesc::ScriptFunction& function
    );

    [[nodiscard]] LUX_ENGINE_AUTHORING_SCRIPT_PUBLIC
    std::vector<lux::simulation::script::EventScriptTarget>
    compatibleEventTargets(
        const lux::simulation::SimulationDescription& simulation,
        const lux::rdesc::ScriptFunction& function,
        bool simulation_scope
    );

    [[nodiscard]] LUX_ENGINE_AUTHORING_SCRIPT_PUBLIC
    EScriptAuthoringError addBinding(
        const lux::simulation::SimulationDescription& simulation,
        const lux::rdesc::Script& asset,
        lux::simulation::script::ScriptMountDescription& mount,
        lux::simulation::script::ScriptBindingDescription binding
    ) noexcept;

    [[nodiscard]] LUX_ENGINE_AUTHORING_SCRIPT_PUBLIC
    EScriptAuthoringError eraseBinding(
        lux::simulation::script::ScriptMountDescription& mount,
        std::size_t binding_index
    ) noexcept;

    [[nodiscard]] LUX_ENGINE_AUTHORING_SCRIPT_PUBLIC
    EScriptAuthoringError moveBinding(
        lux::simulation::script::ScriptMountDescription& mount,
        std::size_t from,
        std::size_t to
    ) noexcept;

    [[nodiscard]] LUX_ENGINE_AUTHORING_SCRIPT_PUBLIC
    std::vector<ScriptBindingDiagnostic> diagnoseBindings(
        const lux::simulation::SimulationDescription& simulation,
        const lux::rdesc::Script& asset,
        const lux::simulation::script::ScriptMountDescription& mount
    );
}
