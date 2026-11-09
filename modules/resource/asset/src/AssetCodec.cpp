#include <lux/engine/resource/asset/AssetCodec.hpp>

#include <lux/engine/resource/asset/animation/AnimationClipAsset.hpp>
#include <lux/engine/resource/asset/animation/SkeletonAsset.hpp>
#include <lux/engine/resource/asset/material/MaterialAssets.hpp>
#include <lux/engine/resource/asset/mesh/MeshAsset.hpp>
#include <lux/engine/resource/asset/model/ModelAsset.hpp>
#include <lux/engine/resource/asset/shader/ShaderAsset.hpp>
#include <lux/engine/resource/asset/texture/TextureAsset.hpp>
#include <lux/engine/resource/asset/texture/TextureAtlasAssets.hpp>

namespace lux::asset
{
    namespace
    {
        template <class T> class BuiltinAssetCodec final : public AssetCodec
        {
        public:
            [[nodiscard]] AssetTypeId type() const noexcept override { return T::asset_type; }
            [[nodiscard]] std::uint32_t magic() const noexcept override { return T::primary_magic; }

            [[nodiscard]] cxx::type_info dataType() const noexcept override
            {
                return cxx::make_basic_type_info<typename T::data_type>();
            }

            [[nodiscard]] cxx::expected<std::shared_ptr<const Asset>, AssetDecodeFailure>
            decode(AssetId id, cxx::SharedBytes<> image, const AssetDecodeLimits& limits) const noexcept override
            {
                auto decoded = TAssetSerDeser<T>::decode(id, std::move(image), limits);
                if (!decoded)
                {
                    return cxx::unexpected(decoded.error());
                }
                return std::move(*decoded);
            }

            [[nodiscard]] cxx::expected<std::vector<std::byte>, AssetEncodeFailure> encode(
                const AssetInfo& info,
                const void* data,
                std::span<const AssetAuxiliaryPayload> auxiliary,
                const AssetEncodeLimits& limits
            ) const noexcept override
            {
                return TAssetSerDeser<T>::encode(
                    info, *static_cast<const typename T::data_type*>(data), auxiliary, limits
                );
            }

            [[nodiscard]] const void* data(const Asset& asset) const noexcept override
            {
                return &static_cast<const T&>(asset).data();
            }
        };
    }

    std::vector<std::unique_ptr<AssetCodec>> createAssetCodecs()
    {
        std::vector<std::unique_ptr<AssetCodec>> codecs;
        codecs.reserve(10);
        codecs.push_back(std::make_unique<BuiltinAssetCodec<MeshAsset>>());
        codecs.push_back(std::make_unique<BuiltinAssetCodec<ModelAsset>>());
        codecs.push_back(std::make_unique<BuiltinAssetCodec<MaterialAsset>>());
        codecs.push_back(std::make_unique<BuiltinAssetCodec<MaterialInstanceAsset>>());
        codecs.push_back(std::make_unique<BuiltinAssetCodec<TextureAsset>>());
        codecs.push_back(std::make_unique<BuiltinAssetCodec<TextureAtlasAsset>>());
        codecs.push_back(std::make_unique<BuiltinAssetCodec<FlipbookClipAsset>>());
        codecs.push_back(std::make_unique<BuiltinAssetCodec<ShaderAsset>>());
        codecs.push_back(std::make_unique<BuiltinAssetCodec<SkeletonAsset>>());
        codecs.push_back(std::make_unique<BuiltinAssetCodec<AnimationClipAsset>>());
        return codecs;
    }
}
