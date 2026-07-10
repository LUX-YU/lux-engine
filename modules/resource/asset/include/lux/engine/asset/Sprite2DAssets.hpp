#pragma once
/**
 * @file Sprite2DAssets.hpp
 * @brief Asset wrappers for the two traditional-2D metadata resources:
 *        SpriteAtlasAsset (SPRITE_ATLAS) and SpriteAnimClipAsset
 *        (SPRITE_ANIM_CLIP). A2-00 / A2-01.
 *
 * Both follow the AnimationClip "pure metadata" pattern: the encoded
 * description lives in the INFO section, data_size == 0. The two asset
 * classes share this header (and one SerDeser header / one cpp) because they
 * are structural twins always consumed together by the 2D kit — a deliberate
 * deviation from the one-type-per-file convention to keep the file count down.
 *
 * On-disk layout is the concern of @ref Sprite2DSerDeser.hpp.
 */

#include <lux/engine/description/Sprite2D.hpp>
#include "Asset.hpp"

namespace lux::asset
{
    /// Re-exported for convenience under the lux::asset namespace.
    using lux::rdesc::SpriteAtlas;
    using lux::rdesc::SpriteAtlasFrame;
    using lux::rdesc::SpriteAnimClip;
    using lux::rdesc::SpriteAnimFrame;
    using lux::rdesc::SpriteAnimEvent;

    /// Convert an rdesc opaque asset reference to/from the asset id type.
    [[nodiscard]] LUX_RESOURCE_PUBLIC asset_id_t          assetIdFromOpaque(const lux::rdesc::OpaqueAssetId& raw) noexcept;
    [[nodiscard]] LUX_RESOURCE_PUBLIC lux::rdesc::OpaqueAssetId opaqueFromAssetId(const asset_id_t& id) noexcept;

    struct SpriteAtlasLoadConfig{};
    struct SpriteAnimClipLoadConfig{};

    class LUX_RESOURCE_PUBLIC SpriteAtlasAsset : public TAsset<SpriteAtlas>
    {
        friend class SpriteAtlasSerDeser;
    public:
        static constexpr EAssetType asset_type{ EAssetType::SPRITE_ATLAS };

        explicit SpriteAtlasAsset(std::unique_ptr<AssetInfo>   info,
                                  std::unique_ptr<SpriteAtlas> atlas = nullptr);
    };

    class LUX_RESOURCE_PUBLIC SpriteAnimClipAsset : public TAsset<SpriteAnimClip>
    {
        friend class SpriteAnimClipSerDeser;
    public:
        static constexpr EAssetType asset_type{ EAssetType::SPRITE_ANIM_CLIP };

        explicit SpriteAnimClipAsset(std::unique_ptr<AssetInfo>      info,
                                     std::unique_ptr<SpriteAnimClip> clip = nullptr);
    };
}
