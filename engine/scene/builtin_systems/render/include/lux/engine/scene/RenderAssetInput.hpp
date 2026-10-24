#pragma once

#include <lux/engine/resource/asset/storage/AssetVfs.hpp>

namespace lux::scene
{
    struct RenderAssetInput final
    {
        // Fixed identity domain for this Scene; revision and snapshot are published together on Main.
        std::uint64_t source{};
        std::uint64_t revision{};
        asset::AssetVfsView snapshot;
    };
} // namespace lux::scene
