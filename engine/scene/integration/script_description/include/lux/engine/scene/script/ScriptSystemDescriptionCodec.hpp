#pragma once

#include <lux/engine/simulation/SimulationDescriptionBuilder.hpp>
#include <lux/engine/scene/script/ScriptSystemDescription.hpp>
#include <lux/engine/scene/script_description/visibility.h>

#include <cstddef>
#include <span>
#include <string_view>
#include <vector>

namespace lux::scene::script
{
    using lux::simulation::SimulationDataSchemaId;
    using lux::simulation::SimulationDescription;
    using lux::simulation::SimulationDescriptionBuilder;
    using lux::simulation::script::ScriptMountId;
    using lux::simulation::script::HookScriptTarget;
    using lux::simulation::script::EventScriptTarget;
    using lux::simulation::script::ScriptBindingTarget;
    using lux::simulation::script::ScriptBindingDescription;

    inline constexpr std::string_view ScriptSystemDataCanonicalName{"lux.simulation.script"};
    inline constexpr std::uint32_t ScriptSystemWireMagic{0x5353584CU};

    struct ScriptSystemCodecLimits final
    {
        std::size_t max_input_bytes{};
        std::size_t max_decoded_bytes{};
        std::size_t max_encoded_bytes{};
    };

    [[nodiscard]] LUX_ENGINE_SCENE_SCRIPT_DESCRIPTION_PUBLIC
    SimulationDataSchemaId scriptSystemDataSchemaId();

    [[nodiscard]] LUX_ENGINE_SCENE_SCRIPT_DESCRIPTION_PUBLIC
    lux::cxx::expected<std::vector<std::byte>, EScriptSystemDescriptionError>
    encodeScriptSystemDescription(
        const ScriptSystemDescription& description,
        ScriptSystemCodecLimits limits
    ) noexcept;

    [[nodiscard]] LUX_ENGINE_SCENE_SCRIPT_DESCRIPTION_PUBLIC
    lux::cxx::expected<ScriptSystemDescription, EScriptSystemDescriptionError>
    decodeScriptSystemDescription(
        std::span<const std::byte> bytes,
        const SimulationDescription& simulation,
        ScriptSystemCodecLimits limits
    ) noexcept;

    [[nodiscard]] LUX_ENGINE_SCENE_SCRIPT_DESCRIPTION_PUBLIC
    lux::cxx::expected<void, EScriptSystemDescriptionError>
    addScriptSystemData(
        SimulationDescriptionBuilder& builder,
        const ScriptSystemDescription& description,
        ScriptSystemCodecLimits limits
    ) noexcept;
}
