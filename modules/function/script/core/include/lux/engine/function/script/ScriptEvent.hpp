#pragma once

#include <lux/engine/core/semantic/SemanticType.hpp>

#include <cstdint>
#include <string>
#include <string_view>
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

    friend bool operator==(const ScriptEventPayloadDescription &,
                           const ScriptEventPayloadDescription &) noexcept = default;
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
    std::uint64_t delivery_hook_id{};
    std::uint64_t delivery_schema_hash{};
    std::uint32_t delivery_schema_version{};

    [[nodiscard]] bool valid() const noexcept
    {
        return !system_name.empty() && !event_name.empty() && system_id != 0U && event_id != 0U &&
               route <= EScriptEventRoute::ENTITY_TARGETED && !payload.canonical_name.empty() &&
               payload.type_id == lux::semantic::typeId(payload.canonical_name) && payload.abi_kind != 0U &&
               payload.size != 0U && payload.alignment != 0U && (payload.alignment & (payload.alignment - 1U)) == 0U &&
               payload_schema_hash != 0U && payload_schema_version != 0U && delivery_hook_id != 0U &&
               delivery_schema_hash != 0U && delivery_schema_version != 0U;
    }

    friend bool operator==(const ScriptEventSourceDescription &,
                           const ScriptEventSourceDescription &) noexcept = default;
};

// Read-only projection of the same semantic contract for target-compiled constant tables.
struct ScriptEventSourceView final
{
    std::string_view system_name;
    std::string_view event_name;
    std::uint64_t system_id{};
    std::uint64_t event_id{};
    EScriptEventRoute route{};
    lux::semantic::Layout payload;
    std::uint64_t payload_schema_hash{};
    std::uint32_t payload_schema_version{};
    std::uint64_t delivery_hook_id{};
    std::uint64_t delivery_schema_hash{};
    std::uint32_t delivery_schema_version{};

    [[nodiscard]] constexpr bool valid() const noexcept
    {
        return !system_name.empty() && !event_name.empty() && system_id != 0U && event_id != 0U &&
            route <= EScriptEventRoute::ENTITY_TARGETED && payload.type_id != 0U &&
            payload.type_id == lux::semantic::typeId(payload.canonical_name) && payload.size != 0U &&
            payload.alignment != 0U && (payload.alignment & (payload.alignment - 1U)) == 0U &&
            payload_schema_hash != 0U && payload_schema_version != 0U && delivery_hook_id != 0U &&
            delivery_schema_hash != 0U && delivery_schema_version != 0U;
    }

    [[nodiscard]] bool matches(const ScriptEventSourceDescription &source) const noexcept
    {
        return system_name == source.system_name && event_name == source.event_name && system_id == source.system_id &&
               event_id == source.event_id && route == source.route &&
               payload.canonical_name == source.payload.canonical_name && payload.type_id == source.payload.type_id &&
               payload.abi_kind == source.payload.abi_kind && payload.size == source.payload.size &&
               payload.alignment == source.payload.alignment && payload_schema_hash == source.payload_schema_hash &&
               payload_schema_version == source.payload_schema_version && delivery_hook_id == source.delivery_hook_id &&
               delivery_schema_hash == source.delivery_schema_hash &&
               delivery_schema_version == source.delivery_schema_version;
    }

    [[nodiscard]] ScriptEventSourceDescription materialize() const
    {
        return {
            std::string(system_name),
            std::string(event_name),
            system_id,
            event_id,
            route,
            {std::string(payload.canonical_name), payload.type_id, payload.abi_kind, payload.size, payload.alignment},
            payload_schema_hash,
            payload_schema_version,
            delivery_hook_id,
            delivery_schema_hash,
            delivery_schema_version};
    }
};

struct ScriptEventSourceLess final
{
    [[nodiscard]] bool operator()(const ScriptEventSourceDescription &left,
                                  const ScriptEventSourceDescription &right) const noexcept
    {
        return std::tie(left.system_id, left.event_id, left.system_name, left.event_name) <
               std::tie(right.system_id, right.event_id, right.system_name, right.event_name);
    }
};
} // namespace lux::script
