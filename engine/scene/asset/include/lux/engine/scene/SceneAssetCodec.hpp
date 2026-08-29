#pragma once

#include <lux/engine/resource/asset/AssetCodecSet.hpp>
#include <lux/engine/scene/SceneDescription.hpp>
#include <lux/engine/scene/asset/visibility.h>

#include <cstdint>
#include <memory>
#include <string_view>

namespace lux::scene
{
    inline constexpr std::string_view SceneAssetCanonicalName{"lux.scene.description"};
    inline constexpr std::uint32_t SceneAssetPrimaryMagic{0x4443534cU};

    [[nodiscard]] LUX_ENGINE_SCENE_ASSET_PUBLIC lux::asset::AssetCodecDescriptor
    sceneAssetCodecDescriptor(std::shared_ptr<const void> code_lifetime);
} // namespace lux::scene
