#pragma once

#include <lux/engine/authoring/flowforge/visibility.h>
#include <lux/engine/function/script/ScriptSemantic.hpp>
#include <lux/engine/simulation/SimulationDescription.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace lux::flowforge
{
    enum class ETypedEntryKind : std::uint8_t
    {
        HOOK,
        EVENT,
    };

    struct TypedEntryNode final
    {
        ETypedEntryKind kind{ETypedEntryKind::HOOK};
        std::string system_type;
        std::string system_instance;
        std::string member;
        lux::simulation::ESystemHookCardinality cardinality{
            lux::simulation::ESystemHookCardinality::MULTI};
        lux::simulation::ESystemEventTarget event_target{
            lux::simulation::ESystemEventTarget::GLOBAL};
        std::vector<lux::script::ScriptSemanticType> parameters;
        std::vector<lux::script::ScriptSemanticType> returns;
    };

    [[nodiscard]] LUX_ENGINE_AUTHORING_FLOWFORGE_PUBLIC
    std::vector<TypedEntryNode> makeTypedEntryCatalog(
        const lux::simulation::SimulationDescription& description
    );
}
