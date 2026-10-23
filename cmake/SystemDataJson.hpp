#pragma once

#include <lux/engine/system/SystemDataContract.hpp>
#include <nlohmann/json.hpp>

// Build-time projection shared by plugin declaration writers; runtime uses the typed declaration.
inline nlohmann::json systemDataJson(std::span<const lux::system::SystemDataContract> contracts)
{
    nlohmann::json result = nlohmann::json::array();
    for (const auto& value : contracts)
    {
        result.push_back(
            {{"id", value.name},
             {"version", value.version},
             {"kind", value.kind == lux::system::ESystemDataKind::SCHEMA ? "schema" : "prepared_data"},
             {"phase", static_cast<std::uint8_t>(value.phase)}}
        );
    }
    return result;
}
