#pragma once
#include <lux/engine/process/asset_loading/AssetLoadSender.hpp>
#include <lux/engine/process/asset_loading/visibility.h>

namespace lux::process::asset_loading
{
    struct MemoryAssetImage final
    {
        asset::AssetId id;
        asset::AssetBlob image;
    };
    // Both memory images and fallback are fixed for this port's lifetime. Decoding is scheduled by loadAsset.
    [[nodiscard]] LUX_PROCESS_ASSET_LOADING_PUBLIC lux::cxx::expected<AssetReadPort, lux::async::ESubmitError>
    makeAssetReadOverlay(std::vector<MemoryAssetImage>, AssetReadPort fallback) noexcept;
}
