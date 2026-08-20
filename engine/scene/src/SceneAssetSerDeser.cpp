#include <lux/engine/scene/SceneAssetSerDeser.hpp>

#include "SceneDescriptionCodec.hpp"

#include <lux/cxx/compile_time/type_info.hpp>

#include <cstring>
#include <fstream>
#include <istream>
#include <limits>
#include <utility>

namespace lux::scene
{
    namespace
    {
        [[nodiscard]] SceneCodecFailure failure(ESceneCodecError error, std::string detail)
        {
            return SceneCodecFailure{error, std::move(detail)};
        }

        [[nodiscard]] lux::asset::EAssetError assetError(ESceneCodecError error) noexcept
        {
            using lux::asset::EAssetError;
            switch (error)
            {
            case ESceneCodecError::BAD_MAGIC:
            case ESceneCodecError::OUTER_INNER_ID_MISMATCH:
                return EAssetError::WRONG_FILE_HEADER;
            case ESceneCodecError::UNSUPPORTED_VERSION:
                return EAssetError::UNSUPPORTED_VERSION;
            case ESceneCodecError::TRUNCATED:
            case ESceneCodecError::LIMIT_EXCEEDED:
                return EAssetError::ABNORMAL_FILE_SIZE;
            default:
                return EAssetError::ASSET_DESERIALIZE_FAIL;
            }
        }

        [[nodiscard]] SceneCodecResult<std::vector<std::byte>> encodeImage(
            const lux::asset::AssetInfo& info,
            const SceneDescription& description,
            const SceneCodecLimits& limits) noexcept
        {
            if (info.id.is_nil() || description.id.is_nil() ||
                info.id != description.id || info.type != kSceneAssetType)
            {
                return lux::cxx::unexpected(failure(
                    ESceneCodecError::OUTER_INNER_ID_MISMATCH,
                    "Scene AssetInfo and SceneDescription identity differ"));
            }

            auto data = detail::encodeSceneDescriptionBytes(
                description,
                limits);
            if (!data)
                return lux::cxx::unexpected(data.error());
            if (data->size() >
                std::numeric_limits<std::size_t>::max() -
                    sizeof(lux::asset::AssetFileHeader))
            {
                return lux::cxx::unexpected(failure(
                    ESceneCodecError::LIMIT_EXCEEDED,
                    "Scene Asset image size overflows"));
            }

            const lux::asset::AssetFileHeader header{
                .magic_number = kSceneAssetMagic,
                .version      = lux::asset::current_asset_version,
                .info_offset  = sizeof(lux::asset::AssetFileHeader),
                .info_size    = 0u,
                .data_offset  = sizeof(lux::asset::AssetFileHeader),
                .data_size    = data->size(),
                .info         = info
            };
            std::vector<std::byte> image(sizeof(header) + data->size());
            std::memcpy(image.data(), &header, sizeof(header));
            std::memcpy(
                image.data() + sizeof(header),
                data->data(),
                data->size()
            );
            return image;
        }

        [[nodiscard]] lux::cxx::expected<std::vector<std::byte>, lux::asset::EAssetError>
        readAll(std::istream& stream)
        {
            stream.clear();
            stream.seekg(0, std::ios::end);
            const auto end = stream.tellg();
            if (end <= 0)
            {
                return lux::cxx::unexpected(lux::asset::EAssetError::ABNORMAL_FILE_SIZE);
            }
            stream.seekg(0, std::ios::beg);
            std::vector<std::byte> bytes(static_cast<std::size_t>(end));
            stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            if (stream.gcount() != static_cast<std::streamsize>(bytes.size()))
            {
                return lux::cxx::unexpected(lux::asset::EAssetError::READ_FILE_FAIL);
            }
            return bytes;
        }

        [[nodiscard]] std::unique_ptr<lux::asset::AssetSerDeser>
        createCodec(lux::asset::EAssetType type, std::shared_ptr<lux::asset::AssetManager> manager)
        {
            return type == kSceneAssetType
                ? std::make_unique<SceneAssetSerDeser>(std::move(manager))
                : nullptr;
        }

        [[nodiscard]] lux::cxx::expected<lux::asset::AssetDataInjector, lux::asset::EAssetError>
        decodeSceneAsset(lux::cxx::SharedBytes<> image) noexcept
        {
            auto description = SceneAssetSerDeser::decodeData(image.view());
            if (!description){
                return lux::cxx::unexpected(assetError(description.error().error));
            }
            return lux::asset::AssetDataInjector{
                [value = std::move(*description)](lux::asset::LuxAsset& shell) mutable
                {
                    if (auto* scene = shell.as<SceneAsset>())
                        scene->setData(std::move(value));
                }
            };
        }

        [[nodiscard]] std::unique_ptr<lux::asset::LuxAsset>
        createShell(std::unique_ptr<lux::asset::AssetInfo> info) noexcept
        {
            if (!info || info->type != kSceneAssetType || info->id.is_nil())
                return nullptr;
            return std::make_unique<SceneAsset>(std::move(info));
        }

        [[nodiscard]] lux::cxx::expected<std::unique_ptr<lux::asset::LuxAsset>, lux::asset::EAssetError>
        createLegacyShell(std::span<const std::byte> image) noexcept
        {
            auto description = SceneAssetSerDeser::decodeData(image);
            if (!description)
                return lux::cxx::unexpected(assetError(description.error().error));
            auto info = std::make_unique<lux::asset::AssetInfo>();
            info->id = (*description)->id;
            info->type = kSceneAssetType;
            return std::unique_ptr<lux::asset::LuxAsset>{
                std::make_unique<SceneAsset>(std::move(info))
            };
        }
    } // namespace

    SceneAssetSerDeser::SceneAssetSerDeser(
        std::shared_ptr<lux::asset::AssetManager> manager)
        : TAssetSerDeser<std::monostate>(std::move(manager))
    {}

    SceneCodecResult<std::vector<std::byte>> SceneAssetSerDeser::encodeData(
        const lux::asset::asset_id_t& id,
        const SceneDescription& description,
        const SceneCodecLimits& limits) noexcept
    {
        lux::asset::AssetInfo info{};
        info.id = id;
        info.type = kSceneAssetType;
        return encodeImage(info, description, limits);
    }

    SceneCodecResult<std::unique_ptr<SceneDescription>>
    SceneAssetSerDeser::decodeData(
        std::span<const std::byte> image,
        const SceneCodecLimits& limits) noexcept
    {
        if (image.size() < sizeof(std::uint32_t))
        {
            return lux::cxx::unexpected(failure(
                ESceneCodecError::TRUNCATED,
                "Scene image is truncated"));
        }

        std::uint32_t magic{};
        std::memcpy(&magic, image.data(), sizeof(magic));
        std::span<const std::byte> data;
        lux::asset::asset_id_t outer_id{};
        if (magic == kSceneDescriptionMagic)
        {
            data = image;
        }
        else if (magic == kSceneAssetMagic)
        {
            if (image.size() < sizeof(lux::asset::AssetFileHeader))
            {
                return lux::cxx::unexpected(failure(
                    ESceneCodecError::TRUNCATED,
                    "Scene Asset header is truncated"));
            }
            lux::asset::AssetFileHeader header{};
            std::memcpy(&header, image.data(), sizeof(header));
            if (header.version != lux::asset::current_asset_version)
            {
                return lux::cxx::unexpected(failure(
                    ESceneCodecError::UNSUPPORTED_VERSION,
                    "unsupported Scene Asset header version"));
            }
            if (header.info.type != kSceneAssetType ||
                header.info.id.is_nil() ||
                header.info_offset != sizeof(header) ||
                header.info_size != 0u ||
                header.data_offset != sizeof(header) ||
                header.data_size > limits.maximum_manifest_bytes ||
                header.data_size != image.size() - sizeof(header))
            {
                return lux::cxx::unexpected(failure(
                    ESceneCodecError::INVALID_ARGUMENT,
                    "invalid Scene Asset header or bounds"));
            }
            outer_id = header.info.id;
            data = image.subspan(
                sizeof(header),
                static_cast<std::size_t>(header.data_size));
        }
        else
        {
            return lux::cxx::unexpected(failure(
                ESceneCodecError::BAD_MAGIC,
                "Scene Asset magic mismatch"));
        }

        auto description = detail::decodeSceneDescriptionBytes(data, limits);
        if (!description)
            return lux::cxx::unexpected(description.error());
        if (!outer_id.is_nil() && outer_id != description->id)
        {
            return lux::cxx::unexpected(failure(
                ESceneCodecError::OUTER_INNER_ID_MISMATCH,
                "outer Scene Asset id differs from LXSC id"));
        }
        return std::make_unique<SceneDescription>(std::move(*description));
    }

    lux::cxx::expected<
        std::unique_ptr<lux::asset::LuxAsset>,
        lux::asset::EAssetError>
    SceneAssetSerDeser::fromLuxAssetStream(std::istream& stream)
    {
        auto image = readAll(stream);
        if (!image)
            return lux::cxx::unexpected(image.error());
        auto description = decodeData(*image);
        if (!description)
            return lux::cxx::unexpected(assetError(description.error().error));

        auto info = std::make_unique<lux::asset::AssetInfo>();
        std::uint32_t magic{};
        std::memcpy(&magic, image->data(), sizeof(magic));
        if (magic == kSceneAssetMagic)
        {
            lux::asset::AssetFileHeader header{};
            std::memcpy(&header, image->data(), sizeof(header));
            *info = header.info;
        }
        else
        {
            info->id = (*description)->id;
            info->type = kSceneAssetType;
        }
        return std::unique_ptr<lux::asset::LuxAsset>{
            std::make_unique<SceneAsset>(
                std::move(info),
                std::move(*description))};
    }

    lux::asset::EAssetError SceneAssetSerDeser::exportAsLuxAssetStream(
        const lux::asset::LuxAsset& asset,
        std::ofstream& stream)
    {
        const auto* scene = asset.as<SceneAsset>();
        if (scene == nullptr)
            return lux::asset::EAssetError::FILE_TYPE_ERROR;
        if (!scene->hasData())
            return lux::asset::EAssetError::ASSET_NO_DATA;
        auto image = encodeImage(*scene->info(), *scene->data(), {});
        if (!image)
            return assetError(image.error().error);
        stream.write(
            reinterpret_cast<const char*>(image->data()),
            static_cast<std::streamsize>(image->size()));
        return stream.good()
            ? lux::asset::EAssetError::SUCCESS
            : lux::asset::EAssetError::WRITE_FILE_FAIL;
    }

    lux::asset::AssetCodecDescriptor sceneAssetCodecDescriptor()
    {
        return lux::asset::AssetCodecDescriptor{
            kSceneAssetType,
            lux::cxx::type_hash<SceneAsset>(),
            std::string{lux::cxx::type_name<SceneAsset>()},
            lux::asset::EAssetShippingClass::RUNTIME,
            &createCodec,
            &decodeSceneAsset,
            &createShell,
            kSceneAssetMagic,
            kSceneDescriptionMagic,
            &createLegacyShell,
            {}};
    }

    lux::cxx::expected<
        std::shared_ptr<const lux::asset::AssetCodecCatalog>,
        lux::asset::EAssetCodecCatalogError>
    makeSceneAssetCodecCatalog(
        const lux::asset::AssetCodecCatalog& base) noexcept
    {
        std::vector<lux::asset::AssetCodecDescriptor> descriptors{
            base.descriptors().begin(), base.descriptors().end()};
        descriptors.push_back(sceneAssetCodecDescriptor());
        auto catalog = lux::asset::AssetCodecCatalog::build(
            std::move(descriptors));
        if (!catalog)
            return lux::cxx::unexpected(catalog.error());
        return std::make_shared<const lux::asset::AssetCodecCatalog>(
            std::move(*catalog));
    }
} // namespace lux::scene
