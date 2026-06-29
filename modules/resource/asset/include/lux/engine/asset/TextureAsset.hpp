#pragma once
#include <lux/engine/description/Texture.hpp>
#include <lux/engine/resource/visibility.h>
#include "Asset.hpp"
#include "AssetSerDeser.hpp"

namespace lux::asset
{
    // ===== Backward compatibility: re-export description types =====
    using lux::rdesc::TextureInfo;
    using lux::rdesc::TextureAssetInfo;
    using lux::rdesc::Texture;

	class LUX_RESOURCE_PUBLIC TextureAsset : public TAsset<Texture>
    {
        friend class LuxAssetManager;
		friend class TextureSerDeser;
		friend class ModelSerDeser;
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
