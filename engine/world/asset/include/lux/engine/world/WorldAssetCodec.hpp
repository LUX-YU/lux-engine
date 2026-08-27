#pragma once

#include <lux/engine/resource/asset/AssetCodecSet.hpp>
#include <lux/engine/world/asset/visibility.h>

#include <memory>
#include <cstdint>
#include <string_view>

namespace lux::world
{
    inline constexpr std::string_view WorldAssetCanonicalName{"lux.world.description"};
    inline constexpr std::uint32_t WorldAssetPrimaryMagic{0x4457584CU};

    [[nodiscard]] LUX_ENGINE_WORLD_ASSET_PUBLIC
    lux::asset::AssetCodecDescriptor worldAssetCodecDescriptor(
        std::shared_ptr<const void> code_lifetime
    );
}
