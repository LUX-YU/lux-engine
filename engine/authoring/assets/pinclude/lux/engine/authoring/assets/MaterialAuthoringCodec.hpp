#pragma once

#include <lux/engine/resource/asset/AssetCodecCatalog.hpp>

namespace lux::authoring::detail
{
    /// Editor/toolchain factory. Runtime keeps its strict cooked-v4 decoder;
    /// this manager-less SerDeser additionally migrates legacy material-v3
    /// images into a complete, unregistered MaterialAsset.
    [[nodiscard]] std::unique_ptr<lux::asset::AssetSerDeser>
    createAuthoringMaterialSerDeser(
        lux::asset::EAssetType type,
        std::shared_ptr<lux::asset::AssetManager> manager
    );
} // namespace lux::authoring::detail
