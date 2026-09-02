#pragma once

#include <lux/engine/core/semantic/SemanticType.hpp>

#include <cstdint>

namespace lux::simulation::script
{
    enum class EScriptEndPlayReason : std::uint32_t
    {
        ENTITY_DESTROYED,
        OBJECT_UNMATERIALIZED,
        RUNTIME_STOPPED,
        FAULTED,
    };
} // namespace lux::simulation::script

namespace lux::semantic
{
    template <>
    struct TypeTraits<lux::simulation::script::EScriptEndPlayReason> final
    {
        inline static constexpr std::string_view CanonicalName = "lux.simulation.ScriptEndPlayReason";
        inline static constexpr std::uint8_t AbiKind = static_cast<std::uint8_t>(EAbiKind::U32);
        inline static constexpr std::uint32_t Size = sizeof(lux::simulation::script::EScriptEndPlayReason);
        inline static constexpr std::uint32_t Alignment = alignof(lux::simulation::script::EScriptEndPlayReason);
    };
} // namespace lux::semantic
