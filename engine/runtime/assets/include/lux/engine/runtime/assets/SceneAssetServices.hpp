#pragma once

#include <lux/engine/runtime/assets/AssetLoadService.hpp>

namespace lux::asset
{
    class AssetManager;
}

namespace lux::asset_runtime
{
    /// Narrow scene-assembly capability published by runtime::scene.
    ///
    /// Packs may resolve authored asset ids and inspect the runtime ledger,
    /// but they do not receive AsyncRuntime, a scheduler, or a generic task
    /// submission surface.  The service is assembly-time data and is never
    /// looked up from a per-frame hot path: installed systems retain the
    /// concrete references they need.
    struct SceneAssetServices final
    {
        lux::asset::AssetManager& manager;
        AssetClient               loads;
    };
}
