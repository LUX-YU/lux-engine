#pragma once

#include <lux/engine/description/Script.hpp>
#include <lux/engine/resource/asset/AssetCodecSet.hpp>
#include <lux/engine/resource/asset/script/visibility.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

namespace lux::asset
{
    inline constexpr std::string_view ScriptAssetCanonicalName{
        "lux.script.asset"};
    inline constexpr std::uint32_t ScriptAssetPrimaryMagic{0x4153584CU};

    struct ScriptAssetContent final
    {
        lux::rdesc::Script description;
        std::vector<std::byte> payload;
    };

    [[nodiscard]] LUX_RESOURCE_SCRIPT_ASSET_PUBLIC AssetCodecDescriptor
    scriptAssetCodecDescriptor(std::shared_ptr<const void> code_lifetime);
}
