#pragma once

#include <lux/engine/resource/asset/AssetId.hpp>

namespace lux::scene
{
    struct SceneDescription final
    {
        asset::AssetId world;
        asset::AssetId simulation;
    };
} // namespace lux::scene
