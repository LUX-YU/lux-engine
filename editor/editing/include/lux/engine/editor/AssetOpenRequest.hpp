#pragma once
#include <lux/engine/resource/identity/AssetId.hpp>
namespace lux::editor
{
    struct AssetOpenRequest final
    {
        asset::AssetId asset;
    };
}
