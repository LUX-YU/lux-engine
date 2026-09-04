#pragma once

#include <lux/engine/function/script/ScriptEvent.hpp>

#include <lux/cxx/compile_time/expected.hpp>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <new>
#include <span>
#include <string_view>

namespace lux::toolchain::lua
{
    enum class EScriptEventSchemaWriteError : std::uint8_t
    {
        INVALID_SOURCE,
        OUTPUT_FAILURE,
        ALLOCATION_FAILURE,
    };

    [[nodiscard]] inline lux::cxx::expected<void, EScriptEventSchemaWriteError>
    writeScriptEventSchemaManifest(
        const std::filesystem::path& output,
        std::span<const lux::script::ScriptEventSourceDescription> sources
    ) noexcept
    {
        const auto valid_character = [](char value) noexcept {
            return value >= 0x20 && value != '"' && value != '\\';
        };
        const auto valid_text = [&](std::string_view value) noexcept {
            return !value.empty() && std::ranges::all_of(value, valid_character);
        };
        if (!std::ranges::is_sorted(sources, lux::script::ScriptEventSourceLess{}))
            return lux::cxx::unexpected(EScriptEventSchemaWriteError::INVALID_SOURCE);
        for (std::size_t index{}; index < sources.size(); ++index)
        {
            const auto& source = sources[index];
            const bool invalid_text = !valid_text(source.system_name) || !valid_text(source.event_name) ||
                !valid_text(source.payload.canonical_name);
            const bool duplicate = index != 0U &&
                sources[index - 1U].system_id == source.system_id &&
                sources[index - 1U].event_id == source.event_id;
            if (!source.valid() || invalid_text || duplicate)
                return lux::cxx::unexpected(EScriptEventSchemaWriteError::INVALID_SOURCE);
        }

        try
        {
            std::ofstream stream(output, std::ios::binary | std::ios::trunc);
            if (!stream)
                return lux::cxx::unexpected(EScriptEventSchemaWriteError::OUTPUT_FAILURE);
            stream << "{\n  \"schema\": \"lux-script-event\",\n  \"version\": 1,\n  \"events\": [";
            for (std::size_t index{}; index < sources.size(); ++index)
            {
                const auto& source = sources[index];
                stream << (index == 0U ? "\n" : ",\n")
                       << "    {\n"
                       << "      \"system_name\": \"" << source.system_name << "\",\n"
                       << "      \"event_name\": \"" << source.event_name << "\",\n"
                       << "      \"system_id\": " << source.system_id << ",\n"
                       << "      \"event_id\": " << source.event_id << ",\n"
                       << "      \"route\": \""
                       << (source.route == lux::script::EScriptEventRoute::SIMULATION_BROADCAST
                               ? "simulation_broadcast"
                               : "entity_targeted")
                       << "\",\n"
                       << "      \"payload\": {\n"
                       << "        \"canonical_name\": \"" << source.payload.canonical_name << "\",\n"
                       << "        \"type_id\": " << source.payload.type_id << ",\n"
                       << "        \"abi_kind\": " << static_cast<std::uint32_t>(source.payload.abi_kind) << ",\n"
                       << "        \"size\": " << source.payload.size << ",\n"
                       << "        \"alignment\": " << source.payload.alignment << "\n"
                       << "      },\n"
                       << "      \"payload_schema_hash\": " << source.payload_schema_hash << ",\n"
                       << "      \"payload_schema_version\": " << source.payload_schema_version << "\n"
                       << "    }";
            }
            stream << "\n  ]\n}\n";
            if (!stream)
                return lux::cxx::unexpected(EScriptEventSchemaWriteError::OUTPUT_FAILURE);
            return {};
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(EScriptEventSchemaWriteError::ALLOCATION_FAILURE);
        }
        catch (...)
        {
            return lux::cxx::unexpected(EScriptEventSchemaWriteError::OUTPUT_FAILURE);
        }
    }
}
