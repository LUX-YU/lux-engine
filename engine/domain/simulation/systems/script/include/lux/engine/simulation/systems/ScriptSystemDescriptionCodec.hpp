#pragma once

#include <lux/engine/simulation/SimulationDescriptionBuilder.hpp>
#include <lux/engine/simulation/systems/ScriptSystemDescription.hpp>
#include <lux/engine/simulation/systems/script/visibility.h>

#include <cstddef>
#include <span>
#include <string_view>
#include <vector>

namespace lux::simulation::script
{
    inline constexpr std::string_view ScriptSystemDataCanonicalName{"lux.simulation.script"};
    inline constexpr std::uint32_t ScriptSystemWireMagic{0x5353584CU};

    struct ScriptSystemCodecLimits final
    {
        std::size_t max_input_bytes{};
        std::size_t max_decoded_bytes{};
        std::size_t max_encoded_bytes{};
    };

    [[nodiscard]] LUX_ENGINE_SIMULATION_SCRIPT_PUBLIC
    SimulationDataSchemaId scriptSystemDataSchemaId();

    template<typename T> using ScriptSystemResult = lux::cxx::expected<T, EScriptSystemDescriptionError>;

    [[nodiscard]] LUX_ENGINE_SIMULATION_SCRIPT_PUBLIC ScriptSystemResult<std::vector<std::byte>>
    encodeScriptSystemDescription(
        const ScriptSystemDescription& description,
        ScriptSystemCodecLimits limits
    ) noexcept;

    [[nodiscard]] LUX_ENGINE_SIMULATION_SCRIPT_PUBLIC ScriptSystemResult<ScriptSystemDescription>
    decodeScriptSystemDescription(
        std::span<const std::byte> bytes,
        const SimulationDescription& simulation,
        ScriptSystemCodecLimits limits
    ) noexcept;

    [[nodiscard]] LUX_ENGINE_SIMULATION_SCRIPT_PUBLIC ScriptSystemResult<void>
    addScriptSystemData(
        SimulationDescriptionBuilder& builder,
        const ScriptSystemDescription& description,
        ScriptSystemCodecLimits limits
    ) noexcept;
}
