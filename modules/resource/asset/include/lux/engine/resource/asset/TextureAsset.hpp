#pragma once
#include <lux/engine/description/Texture.hpp>
#include <lux/engine/resource/asset/visibility.h>
#include "Asset.hpp"
#include "AssetSerDeser.hpp"

namespace lux::asset
{
	class LUX_ASSET_PUBLIC TextureAsset : public TAsset<lux::rdesc::Texture>
    {
        friend class LuxAssetManager;
		friend class TextureCodec;
    public:
        static constexpr EAssetType asset_type{EAssetType::TEXTURE};

        explicit TextureAsset(std::unique_ptr<AssetInfo> info);

        TextureAsset(const TextureAsset& other) = delete;

        TextureAsset& operator=(const TextureAsset& other) = delete;

        TextureAsset(TextureAsset&&) noexcept;

        TextureAsset& operator=(TextureAsset&&) noexcept;

        ~TextureAsset() override;
    };
} // namespace lux::asset
