#pragma once

#include <lux/engine/core/async/OperationPort.hpp>
#include <lux/engine/resource/asset/storage/AssetProvider.hpp>
#include <lux/engine/resource/asset/storage/AssetVfs.hpp>

namespace lux::process::asset_loading
{
    struct ReadAssetImage final
    {
        using Value = lux::asset::AssetBlob;
        using Error = lux::asset::EAssetStorageError;

        lux::asset::AssetId id;
        std::size_t max_bytes{SIZE_MAX};
        // Optional captured VFS publication for a multi-asset transaction. Empty uses the endpoint's source.
        lux::asset::AssetVfsView snapshot;
    };

    using AssetReadPort = lux::async::TOperationPort<ReadAssetImage>;
} // namespace lux::process::asset_loading
