#pragma once
/**
 * @file TextureAtlasAssets.hpp
 * @brief Asset wrappers for the two traditional-2D metadata resources:
 *        TextureAtlasAsset (TEXTURE_ATLAS) and FlipbookClipAsset
 *        (FLIPBOOK_CLIP). A2-00 / A2-01.
 *
 * Both follow the AnimationClip "pure metadata" pattern: the encoded
 * description lives in the INFO section, data_size == 0. The two asset
 * classes share this header (and one SerDeser header / one cpp) because they
 * are structural twins always consumed together by the 2D kit — a deliberate
 * deviation from the one-type-per-file convention to keep the file count down.
 *
 * On-disk layout is the concern of @ref TextureAtlasSerDeser.hpp.
 */

#include <lux/engine/description/TextureAtlas.hpp>
#include "Asset.hpp"

namespace lux::asset
{
    /// Convert an rdesc opaque asset reference to/from the asset id type.
    [[nodiscard]] LUX_ASSET_PUBLIC asset_id_t          assetIdFromOpaque(const lux::rdesc::OpaqueAssetId& raw) noexcept;
    [[nodiscard]] LUX_ASSET_PUBLIC lux::rdesc::OpaqueAssetId opaqueFromAssetId(const asset_id_t& id) noexcept;

    struct TextureAtlasLoadConfig{};
    struct FlipbookClipLoadConfig{};

    class LUX_ASSET_PUBLIC TextureAtlasAsset
        : public TAsset<lux::rdesc::TextureAtlas>
    {
        friend class TextureAtlasSerDeser;
    public:
        static constexpr EAssetType asset_type{ EAssetType::TEXTURE_ATLAS };

        explicit TextureAtlasAsset(std::unique_ptr<AssetInfo>   info,
                                  std::unique_ptr<lux::rdesc::TextureAtlas> atlas = nullptr);
    };

    class LUX_ASSET_PUBLIC FlipbookClipAsset
        : public TAsset<lux::rdesc::FlipbookClip>
    {
        friend class FlipbookClipSerDeser;
    public:
        static constexpr EAssetType asset_type{ EAssetType::FLIPBOOK_CLIP };

        explicit FlipbookClipAsset(std::unique_ptr<AssetInfo>      info,
                                     std::unique_ptr<lux::rdesc::FlipbookClip> clip = nullptr);
    };
}
