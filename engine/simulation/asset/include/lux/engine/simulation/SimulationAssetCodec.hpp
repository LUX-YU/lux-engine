#pragma once

#include <lux/engine/resource/asset/AssetCodecSet.hpp>
#include <lux/engine/simulation/asset/visibility.h>

#include <memory>
#include <cstdint>
#include <string_view>

namespace lux::simulation
{
    inline constexpr std::string_view SimulationAssetCanonicalName{
        "lux.simulation.description"};
    inline constexpr std::uint32_t SimulationAssetPrimaryMagic{0x4453584CU};

    [[nodiscard]] LUX_ENGINE_SIMULATION_ASSET_PUBLIC
    lux::asset::AssetCodecDescriptor simulationAssetCodecDescriptor(
        std::shared_ptr<const void> code_lifetime
    );
}
