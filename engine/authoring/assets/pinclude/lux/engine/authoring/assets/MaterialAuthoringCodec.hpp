#pragma once

#include <lux/engine/resource/asset/AssetCodecCatalog.hpp>

namespace lux::authoring::detail
{
    /// Editor/toolchain material decoder. Runtime keeps its strict cooked-v4
    /// decoder; this authoring seam additionally migrates legacy v3 material
    /// images and restores auxiliary authoring payloads onto lazy-loaded shells.
    [[nodiscard]] lux::cxx::expected<
        lux::asset::AssetDataInjector,
        lux::asset::EAssetError>
    decodeAuthoringMaterial(lux::cxx::SharedBytes<> image) noexcept;
} // namespace lux::authoring::detail
