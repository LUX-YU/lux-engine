#pragma once

#include <lux/engine/resource/asset/Asset.hpp>
#include <lux/engine/scene/SceneDescription.hpp>

#include <cstdint>
#include <memory>

namespace lux::scene
{
    inline constexpr auto kSceneAssetType = static_cast<lux::asset::EAssetType>(15u);
    inline constexpr std::uint32_t kSceneAssetMagic = 0x0130914Du;

    class SceneAsset final : public lux::asset::TAsset<SceneDescription>
    {
    public:
        static inline constexpr lux::asset::EAssetType asset_type =
            kSceneAssetType;

        explicit SceneAsset(
            std::unique_ptr<lux::asset::AssetInfo> info,
            std::unique_ptr<SceneDescription> description = nullptr)
            : TAsset(std::move(info), std::move(description))
        {}
    };
} // namespace lux::scene
