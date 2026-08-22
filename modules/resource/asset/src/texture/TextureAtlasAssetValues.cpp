#include <lux/engine/resource/asset/texture/TextureAtlasAssets.hpp>

#include <cstring>

namespace lux::asset
{
    TextureAtlasAsset::TextureAtlasAsset(
        std::unique_ptr<AssetInfo> info,
        std::unique_ptr<lux::rdesc::TextureAtlas> atlas
    )
        : TAsset<lux::rdesc::TextureAtlas>(std::move(info), std::move(atlas))
    {
    }

    FlipbookClipAsset::FlipbookClipAsset(
        std::unique_ptr<AssetInfo> info,
        std::unique_ptr<lux::rdesc::FlipbookClip> clip
    )
        : TAsset<lux::rdesc::FlipbookClip>(std::move(info), std::move(clip))
    {
    }

    asset_id_t assetIdFromOpaque(
        const lux::rdesc::OpaqueAssetId& raw
    ) noexcept
    {
        return asset_id_t(raw.begin(), raw.end());
    }

    lux::rdesc::OpaqueAssetId opaqueFromAssetId(
        const asset_id_t& id
    ) noexcept
    {
        lux::rdesc::OpaqueAssetId out{};
        const auto bytes = id.as_bytes();
        std::memcpy(out.data(), bytes.data(), out.size());
        return out;
    }
} // namespace lux::asset
