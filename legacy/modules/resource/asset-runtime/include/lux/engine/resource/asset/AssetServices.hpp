#pragma once

#include <lux/engine/resource/asset/AssetLoadPort.hpp>

namespace lux::asset
{
    class AssetManager;

    /// Assembly-time references retained by ECS asset-resolution Systems.
    struct AssetServices final
    {
        AssetManager& manager;
        lux::asset_runtime::AssetClient loads;
    };
}
