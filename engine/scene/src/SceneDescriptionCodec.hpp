#pragma once

#include <lux/engine/scene/SceneAssetSerDeser.hpp>

namespace lux::scene::detail
{
    [[nodiscard]] SceneCodecResult<std::vector<std::byte>>
    encodeSceneDescriptionBytes(
        const SceneDescription& description,
        const SceneCodecLimits& limits) noexcept;

    [[nodiscard]] SceneCodecResult<SceneDescription>
    decodeSceneDescriptionBytes(
        std::span<const std::byte> bytes,
        const SceneCodecLimits& limits) noexcept;
} // namespace lux::scene::detail
