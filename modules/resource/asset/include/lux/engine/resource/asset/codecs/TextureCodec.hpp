#pragma once
#include <lux/engine/resource/asset/visibility.h>

#include <lux/engine/resource/asset/AssetSerDeser.hpp>
#include <lux/engine/resource/asset/TextureAsset.hpp>
#include <lux/cxx/memory/SharedBytes.hpp>
#include <lux/engine/description/Texture.hpp>

#include <vector>

namespace lux::asset
{
    /// Runtime codec for already-cooked texture assets.  It never decodes an
    /// authoring image and therefore has no stb or BC encoder dependency.
    class LUX_ASSET_PUBLIC TextureCodec : public AssetSerDeser
    {
    public:
        explicit TextureCodec(std::shared_ptr<AssetManager> manager);
        ~TextureCodec() override;

        [[nodiscard]] static lux::cxx::expected<std::unique_ptr<lux::rdesc::Texture>, EAssetError>
        decodeData(const void* bytes, std::size_t length) noexcept;

        [[nodiscard]] static lux::cxx::expected<std::unique_ptr<lux::rdesc::Texture>, EAssetError>
        decodeData(lux::cxx::SharedBytes<> bytes) noexcept;

        /// Encode deterministic Runtime texture bytes without an AssetManager.
        /// Toolchain-generated textures use this path so generated World
        /// environment assets and imported assets share one canonical codec.
        [[nodiscard]] static lux::cxx::expected<
            std::vector<std::byte>, EAssetError>
        encodeData(
            const asset_id_t& id,
            const lux::rdesc::Texture& texture) noexcept;

    protected:
        EAssetError exportAsLuxAssetStream(
            const LuxAsset& asset,
            std::ofstream& stream
        ) override;

        lux::cxx::expected<std::unique_ptr<LuxAsset>, EAssetError>
        fromLuxAssetStream(std::istream& stream) override;
    };
} // namespace lux::asset
