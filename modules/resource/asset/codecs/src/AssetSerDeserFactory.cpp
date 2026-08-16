#include <lux/engine/resource/asset/AssetCodecCatalog.hpp>

#include <lux/engine/resource/asset/AnimationClipSerDeser.hpp>
#include <lux/engine/resource/asset/MaterialInstanceSerDeser.hpp>
#include <lux/engine/resource/asset/MaterialSerDeser.hpp>
#include <lux/engine/resource/asset/MeshSerDeser.hpp>
#include <lux/engine/resource/asset/ModelCodec.hpp>
#include <lux/engine/resource/asset/ScriptSerDeser.hpp>
#include <lux/engine/resource/asset/ShaderSerDeser.hpp>
#include <lux/engine/resource/asset/SkeletonSerDeser.hpp>
#include <lux/engine/resource/asset/TextureAtlasSerDeser.hpp>
#include <lux/engine/resource/asset/TextureCodec.hpp>

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
                &decodeTexture,
                &createShell<TextureAsset>
            ));
            descriptors.push_back(descriptor<MaterialAsset, MaterialSerDeser>(
                EAssetType::MATERIAL,
                &decodeData<MaterialAsset, MaterialSerDeser>,
                &createShell<MaterialAsset>
            ));
            descriptors.push_back(descriptor<
                MaterialInstanceAsset,
                MaterialInstanceSerDeser>(
                    EAssetType::MATERIAL_INSTANCE,
                    &decodeData<
                        MaterialInstanceAsset,
                        MaterialInstanceSerDeser>,
                    &createShell<MaterialInstanceAsset>
            ));
            descriptors.push_back(descriptor<MeshAsset, MeshSerDeser>(
                EAssetType::MESH,
                &decodeData<MeshAsset, MeshSerDeser>,
                &createShell<MeshAsset>
            ));
            descriptors.push_back(descriptor<ModelAsset, ModelCodec>(
                EAssetType::MODEL
            ));
            descriptors.push_back(descriptor<ShaderAsset, ShaderSerDeser>(
                EAssetType::SHADER
            ));
            descriptors.push_back(descriptor<ScriptAsset, ScriptSerDeser>(
                EAssetType::SCRIPT
            ));
            descriptors.push_back(descriptor<SkeletonAsset, SkeletonSerDeser>(
                EAssetType::SKELETON,
                &decodeData<SkeletonAsset, SkeletonSerDeser>,
                &createShell<SkeletonAsset>
            ));
            descriptors.push_back(descriptor<
                AnimationClipAsset,
                AnimationClipSerDeser>(
                    EAssetType::ANIMATION_CLIP,
                    &decodeData<AnimationClipAsset, AnimationClipSerDeser>,
                    &createShell<AnimationClipAsset>
            ));
            descriptors.push_back(descriptor<
                TextureAtlasAsset,
                TextureAtlasSerDeser>(
                    EAssetType::TEXTURE_ATLAS,
                    &decodeData<TextureAtlasAsset, TextureAtlasSerDeser>,
                    &createShell<TextureAtlasAsset>
            ));
            descriptors.push_back(descriptor<
                FlipbookClipAsset,
                FlipbookClipSerDeser>(
                    EAssetType::FLIPBOOK_CLIP,
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
