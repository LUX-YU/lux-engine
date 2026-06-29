#include <lux/engine/asset/AssetSerDeser.hpp>

#include <lux/engine/asset/TextureSerDeser.hpp>
#include <lux/engine/asset/MaterialSerDeser.hpp>
#include <lux/engine/asset/MaterialInstanceSerDeser.hpp>
#include <lux/engine/asset/MeshSerDeser.hpp>
#include <lux/engine/asset/ModelSerDeser.hpp>
#include <lux/engine/asset/ShaderSerDeser.hpp>
#include <lux/engine/asset/ScriptSerDeser.hpp>
#include <lux/engine/asset/SkeletonSerDeser.hpp>
#include <lux/engine/asset/AnimationClipSerDeser.hpp>
#include <lux/engine/asset/AssetHeaderProbe.hpp>   // assetTypeOfMagic

#include <cstdint>
#include <cstring>
#include <utility>
#include <array>
#include <fstream>
#include <filesystem>

namespace lux::asset
{
    std::unique_ptr<AssetSerDeser>
    createSerDeserFor(EAssetType type, std::shared_ptr<AssetManager> manager)
    {
        switch (type)
        {
        case EAssetType::TEXTURE:
            return std::make_unique<TextureSerDeser>(std::move(manager));
        case EAssetType::MATERIAL:
            return std::make_unique<MaterialSerDeser>(std::move(manager));
        case EAssetType::MATERIAL_INSTANCE:
            return std::make_unique<MaterialInstanceSerDeser>(std::move(manager));
        case EAssetType::MESH:
            return std::make_unique<MeshSerDeser>(std::move(manager));
        case EAssetType::MODEL:
            return std::make_unique<ModelSerDeser>(std::move(manager));
        case EAssetType::SHADER:
            return std::make_unique<ShaderSerDeser>(std::move(manager));
        case EAssetType::SCRIPT:
            return std::make_unique<ScriptSerDeser>(std::move(manager));
        case EAssetType::SKELETON:
            return std::make_unique<SkeletonSerDeser>(std::move(manager));
        case EAssetType::ANIMATION_CLIP:
            return std::make_unique<AnimationClipSerDeser>(std::move(manager));
        // FONT / SOUND have no serialized form yet; UNKNOWN never will.
        default:
            return nullptr;
        }
    }

    // ─── Lazy loading glue (见 .internal/async-asset-redesign-2026-06-17.md §1.5) ───

    namespace
    {
        // 把某个 XxxSerDeser::decodeData 的纯数据结果包成"注入到该类型空壳"的擦除器。
        // 失败 → 透传错误。AssetT = 具体资产类(其 setData 吃 asset_data_t)。
        template <class AssetT, class DecodeFn>
        lux::cxx::expected<AssetDataInjector, EAssetError>
        wrapInjector(DecodeFn&& decode, const void* bytes, std::size_t len)
        {
            auto decoded = decode(bytes, len);
            if (!decoded.has_value())
                return lux::cxx::unexpected(decoded.error());
            return AssetDataInjector{
                [data = std::move(decoded.value())](LuxAsset& shell) mutable
                {
                    if (auto* a = shell.as<AssetT>())
                        a->setData(std::move(data));
                }
            };
        }
    } // namespace

    lux::cxx::expected<AssetDataInjector, EAssetError>
    decodeAssetData(const void* bytes, std::size_t len)
    {
        if (bytes == nullptr || len < sizeof(std::uint32_t))
            return lux::cxx::unexpected(EAssetError::ABNORMAL_FILE_SIZE);

        std::uint32_t magic = 0;
        std::memcpy(&magic, bytes, sizeof(magic));
        switch (assetTypeOfMagic(magic))
        {
        case EAssetType::MESH:
            return wrapInjector<MeshAsset>(&MeshSerDeser::decodeData, bytes, len);
        case EAssetType::TEXTURE:
            return wrapInjector<TextureAsset>(&TextureSerDeser::decodeData, bytes, len);
        case EAssetType::MATERIAL:
            return wrapInjector<MaterialAsset>(&MaterialSerDeser::decodeData, bytes, len);
        case EAssetType::MATERIAL_INSTANCE:
            return wrapInjector<MaterialInstanceAsset>(&MaterialInstanceSerDeser::decodeData, bytes, len);
        case EAssetType::SKELETON:
            return wrapInjector<SkeletonAsset>(&SkeletonSerDeser::decodeData, bytes, len);
        case EAssetType::ANIMATION_CLIP:
            return wrapInjector<AnimationClipAsset>(&AnimationClipSerDeser::decodeData, bytes, len);
        // 不走懒注入的类型(调用方 eager 全载):
        //   MODEL  —— 清单/UUID 图,非单一 asset_data_t;
        //   SCRIPT —— 纯数据在 info、payload 另在 data 段;
        //   SHADER —— 除 TAsset<Shader> 数据外还带 ShaderAssetInfo(不在 data 段,
        //             无法仅凭 data-段注入复原),故与 MODEL/SCRIPT 同列。
        default:
            return lux::cxx::unexpected(EAssetError::UNSUPPORTED);
        }
    }

    std::unique_ptr<LuxAsset>
    makeAssetShell(std::unique_ptr<AssetInfo> info)
    {
        if (!info)
            return nullptr;
        switch (info->type)
        {
        case EAssetType::MESH:              return std::make_unique<MeshAsset>(std::move(info));
        case EAssetType::TEXTURE:           return std::make_unique<TextureAsset>(std::move(info));
        case EAssetType::MATERIAL:          return std::make_unique<MaterialAsset>(std::move(info));
        case EAssetType::MATERIAL_INSTANCE: return std::make_unique<MaterialInstanceAsset>(std::move(info));
        case EAssetType::SKELETON:          return std::make_unique<SkeletonAsset>(std::move(info));
        case EAssetType::ANIMATION_CLIP:    return std::make_unique<AnimationClipAsset>(std::move(info));
        // 不做懒空壳(调用方 eager 全载):
        //   SHADER —— 其 ShaderAssetInfo 不在 data 段,无法仅由 AssetInfo 造壳;
        //   MODEL / SCRIPT —— 非单一 asset_data_t 型;FONT / SOUND / UNKNOWN —— 无序列化型。
        default:                            return nullptr;
        }
    }

    lux::cxx::expected<std::unique_ptr<LuxAsset>, EAssetError>
    makeShellFromFile(const std::filesystem::path& path)
    {
        std::ifstream f(path, std::ios::binary);
        if (!f)
            return lux::cxx::unexpected(EAssetError::FILE_OPEN_FAIL);

        // 只读够最大已知头(v2 头 ≥ v1 头,因 AssetInfo 增大);不载 data 段。
        std::array<std::byte, sizeof(AssetFileHeader)> buf{};
        f.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
        const std::streamsize got = f.gcount();

        // {magic,version} 前缀跨版本布局稳定 —— 先据它识别格式再选具体头布局。
        constexpr std::size_t kPrefix = sizeof(std::uint32_t) + sizeof(asset_version_t);
        if (got < static_cast<std::streamsize>(kPrefix))
            return lux::cxx::unexpected(EAssetError::ABNORMAL_FILE_SIZE);

        std::uint32_t   magic{};
        asset_version_t version{};
        std::memcpy(&magic,   buf.data(),                  sizeof(magic));
        std::memcpy(&version, buf.data() + sizeof(magic),  sizeof(version));

        AssetInfo info{};
        if (version == current_asset_version)
        {
            if (got < static_cast<std::streamsize>(sizeof(AssetFileHeader)))
                return lux::cxx::unexpected(EAssetError::ABNORMAL_FILE_SIZE);
            AssetFileHeader header{};
            std::memcpy(&header, buf.data(), sizeof(header));
            info = header.info;
        }
        else if (version == asset_version_v1)
        {
            if (got < static_cast<std::streamsize>(sizeof(compat::AssetFileHeaderV1)))
                return lux::cxx::unexpected(EAssetError::ABNORMAL_FILE_SIZE);
            compat::AssetFileHeaderV1 v1{};
            std::memcpy(&v1, buf.data(), sizeof(v1));
            info = compat::upgradeAssetInfo(v1.info);   // 单一升级真源,非裸 memcpy
        }
        else
        {
            return lux::cxx::unexpected(EAssetError::UNSUPPORTED_VERSION);
        }

        auto shell = makeAssetShell(std::make_unique<AssetInfo>(info));
        if (!shell)
            return lux::cxx::unexpected(EAssetError::UNSUPPORTED);   // 非懒类型 → 调用方 eager
        return shell;
    }
}
