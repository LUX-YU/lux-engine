#include <lux/engine/resource/asset/codecs/AssetCodecCatalog.hpp>

#include <lux/engine/resource/asset/codecs/AnimationClipSerDeser.hpp>
#include <lux/engine/resource/asset/codecs/MaterialInstanceSerDeser.hpp>
#include <lux/engine/resource/asset/codecs/MaterialSerDeser.hpp>
#include <lux/engine/resource/asset/codecs/MeshSerDeser.hpp>
#include <lux/engine/resource/asset/codecs/ModelCodec.hpp>
#include <lux/engine/resource/asset/codecs/ScriptSerDeser.hpp>
#include <lux/engine/resource/asset/codecs/ShaderSerDeser.hpp>
#include <lux/engine/resource/asset/codecs/SkeletonSerDeser.hpp>
#include <lux/engine/resource/asset/codecs/TextureAtlasSerDeser.hpp>
#include <lux/engine/resource/asset/codecs/TextureCodec.hpp>

#include <lux/cxx/compile_time/type_info.hpp>

#include <cstdlib>
#include <utility>

namespace lux::asset
{
    namespace
    {
        template <class Codec>
        std::unique_ptr<AssetSerDeser> createCodec(
            EAssetType,
            std::shared_ptr<AssetManager> manager)
        {
            return std::make_unique<Codec>(std::move(manager));
        }

        template <class Asset, class Codec>
        lux::cxx::expected<AssetDataInjector, EAssetError> decodeData(
            lux::cxx::SharedBytes<> image) noexcept
        {
            auto decoded = Codec::decodeData(image.data(), image.size());
            if (!decoded)
                return lux::cxx::unexpected(decoded.error());
            return AssetDataInjector{
                [data = std::move(*decoded)](LuxAsset& shell) mutable
                {
                    if (auto* asset = shell.as<Asset>())
                        asset->setData(std::move(data));
                }};
        }

        lux::cxx::expected<AssetDataInjector, EAssetError> decodeTexture(
            lux::cxx::SharedBytes<> image) noexcept
        {
            auto decoded = TextureCodec::decodeData(std::move(image));
            if (!decoded)
                return lux::cxx::unexpected(decoded.error());
            return AssetDataInjector{
                [data = std::move(*decoded)](LuxAsset& shell) mutable
                {
                    if (auto* asset = shell.as<TextureAsset>())
                        asset->setData(std::move(data));
                }};
        }

        template <class Asset>
        std::unique_ptr<LuxAsset> createShell(
            std::unique_ptr<AssetInfo> info) noexcept
        {
            return std::make_unique<Asset>(std::move(info));
        }

        template <class Asset, class Codec>
        AssetCodecDescriptor descriptor(
            EAssetType type,
            std::uint32_t primary_magic,
            AssetDataDecodeFn decode = nullptr,
            AssetShellCreateFn shell = nullptr)
        {
            return AssetCodecDescriptor{
                type,
                lux::cxx::type_hash<Asset>(),
                std::string{lux::cxx::type_name<Asset>()},
                EAssetShippingClass::RUNTIME,
                &createCodec<Codec>,
                decode,
                shell,
                primary_magic,
                0u,
                nullptr,
                {}};
        }
    } // namespace

    std::shared_ptr<const AssetCodecCatalog>
    runtimeAssetCodecCatalog() noexcept
    {
        static const auto catalog = []
        {
            std::vector<AssetCodecDescriptor> descriptors;
            descriptors.reserve(11u);
            descriptors.push_back(descriptor<TextureAsset, TextureCodec>(
                EAssetType::TEXTURE,
                asset_magic_number_of<EAssetType::TEXTURE>::value,
                &decodeTexture,
                &createShell<TextureAsset>
            ));
            descriptors.push_back(descriptor<MaterialAsset, MaterialSerDeser>(
                EAssetType::MATERIAL,
                asset_magic_number_of<EAssetType::MATERIAL>::value,
                &decodeData<MaterialAsset, MaterialSerDeser>,
                &createShell<MaterialAsset>
            ));
            descriptors.push_back(descriptor<
                MaterialInstanceAsset,
                MaterialInstanceSerDeser>(
                    EAssetType::MATERIAL_INSTANCE,
                    asset_magic_number_of<EAssetType::MATERIAL_INSTANCE>::value,
                    &decodeData<
                        MaterialInstanceAsset,
                        MaterialInstanceSerDeser>,
                    &createShell<MaterialInstanceAsset>
            ));
            descriptors.push_back(descriptor<MeshAsset, MeshSerDeser>(
                EAssetType::MESH,
                asset_magic_number_of<EAssetType::MESH>::value,
                &decodeData<MeshAsset, MeshSerDeser>,
                &createShell<MeshAsset>
            ));
            descriptors.push_back(descriptor<ModelAsset, ModelCodec>(
                EAssetType::MODEL,
                asset_magic_number_of<EAssetType::MODEL>::value
            ));
            descriptors.push_back(descriptor<ShaderAsset, ShaderSerDeser>(
                EAssetType::SHADER,
                asset_magic_number_of<EAssetType::SHADER>::value
            ));
            descriptors.push_back(descriptor<ScriptAsset, ScriptSerDeser>(
                EAssetType::SCRIPT,
                asset_magic_number_of<EAssetType::SCRIPT>::value
            ));
            descriptors.push_back(descriptor<SkeletonAsset, SkeletonSerDeser>(
                EAssetType::SKELETON,
                asset_magic_number_of<EAssetType::SKELETON>::value,
                &decodeData<SkeletonAsset, SkeletonSerDeser>,
                &createShell<SkeletonAsset>
            ));
            descriptors.push_back(descriptor<
                AnimationClipAsset,
                AnimationClipSerDeser>(
                    EAssetType::ANIMATION_CLIP,
                    asset_magic_number_of<EAssetType::ANIMATION_CLIP>::value,
                    &decodeData<AnimationClipAsset, AnimationClipSerDeser>,
                    &createShell<AnimationClipAsset>
            ));
            descriptors.push_back(descriptor<
                TextureAtlasAsset,
                TextureAtlasSerDeser>(
                    EAssetType::TEXTURE_ATLAS,
                    asset_magic_number_of<EAssetType::TEXTURE_ATLAS>::value,
                    &decodeData<TextureAtlasAsset, TextureAtlasSerDeser>,
                    &createShell<TextureAtlasAsset>
            ));
            descriptors.push_back(descriptor<
                FlipbookClipAsset,
                FlipbookClipSerDeser>(
                    EAssetType::FLIPBOOK_CLIP,
                    asset_magic_number_of<EAssetType::FLIPBOOK_CLIP>::value,
                    &decodeData<FlipbookClipAsset, FlipbookClipSerDeser>,
                    &createShell<FlipbookClipAsset>
            ));

            auto built = AssetCodecCatalog::build(std::move(descriptors));
            if (!built)
                std::abort();
            return std::make_shared<const AssetCodecCatalog>(
                std::move(*built));
        }();
        return catalog;
    }
} // namespace lux::asset
