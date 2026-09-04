#pragma once

#include <lux/engine/core/semantic/SemanticType.hpp>

#include <cstdint>
#include <string>
#include <tuple>

namespace lux::script
{
    enum class EScriptEventRoute : std::uint8_t
    {
        SIMULATION_BROADCAST,
        ENTITY_TARGETED,
    };

    struct ScriptEventPayloadDescription final
    {
        std::string canonical_name;
        lux::semantic::TypeId type_id{};
        std::uint8_t abi_kind{};
        std::uint32_t size{};
        std::uint32_t alignment{};

        friend bool operator==(
            const ScriptEventPayloadDescription&,
            const ScriptEventPayloadDescription&
        ) noexcept = default;
    };

    struct ScriptEventSourceDescription final
    {
        std::string system_name;
        std::string event_name;
        std::uint64_t system_id{};
        std::uint64_t event_id{};
        EScriptEventRoute route{EScriptEventRoute::SIMULATION_BROADCAST};
        ScriptEventPayloadDescription payload;
        std::uint64_t payload_schema_hash{};
        std::uint32_t payload_schema_version{};

        [[nodiscard]] bool valid() const noexcept
        {
            return !system_name.empty() && !event_name.empty() && system_id != 0U && event_id != 0U &&
                route <= EScriptEventRoute::ENTITY_TARGETED && !payload.canonical_name.empty() &&
                payload.type_id == lux::semantic::typeId(payload.canonical_name) && payload.abi_kind != 0U &&
                payload.size != 0U && payload.alignment != 0U &&
                (payload.alignment & (payload.alignment - 1U)) == 0U && payload_schema_hash != 0U &&
                payload_schema_version != 0U;
        }

        friend bool operator==(
            const ScriptEventSourceDescription&,
            const ScriptEventSourceDescription&
        ) noexcept = default;
    };

    struct ScriptEventSourceLess final
    {
        [[nodiscard]] bool operator()(
            const ScriptEventSourceDescription& left,
            const ScriptEventSourceDescription& right
        ) const noexcept
        {
            return std::tie(left.system_id, left.event_id, left.system_name, left.event_name) <
                std::tie(right.system_id, right.event_id, right.system_name, right.event_name);
        }
    };
}
