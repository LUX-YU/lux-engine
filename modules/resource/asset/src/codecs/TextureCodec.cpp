#include <lux/engine/resource/asset/codecs/TextureCodec.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
    using lux::rdesc::TextureInfo;
    using lux::rdesc::TextureAssetInfo;
    using lux::rdesc::TextureMipRange;
    using lux::rdesc::kTextureMaxMipCount;

    [[nodiscard]] bool validateMipRanges(
        const TextureInfo& info,
        std::size_t payload_size
    ) noexcept
    {
        if (info.mip_count == 0 || info.mip_count > kTextureMaxMipCount)
            return false;

        const auto payload = static_cast<std::uint64_t>(payload_size);
        for (std::uint32_t index = 0; index < info.mip_count; ++index)
        {
            const TextureMipRange& mip = info.mip_ranges[index];
            if (mip.width == 0 || mip.height == 0 || mip.size == 0)
                return false;
            if (mip.offset > payload || mip.size > payload - mip.offset)
                return false;
        }
        return true;
    }

    [[nodiscard]] lux::cxx::expected<
        std::unique_ptr<lux::rdesc::Texture>,
        lux::asset::EAssetError
    > decodeTextureFromImage(
        lux::cxx::SharedBytes<> file,
        const lux::asset::AssetFileHeader& header,
        const lux::rdesc::TextureAssetInfo& disk_info
    ) noexcept
    {
        using lux::asset::EAssetError;

        if (
            header.data_offset > file.size() ||
            header.data_size > file.size() - header.data_offset
        )
        {
            return lux::cxx::unexpected(
                EAssetError::ABNORMAL_FILE_SIZE
            );
        }

        TextureInfo info{};
        info.width        = disk_info.width;
        info.height       = disk_info.height;
        info.channel      = disk_info.channel;
        info.layers       = disk_info.layers;
        info.mip_count    = disk_info.mip_count;
        info.pixel_format = static_cast<lux::rdesc::ETexturePixelFormat>(
            disk_info.pixel_format
        );
        info.color_space = static_cast<lux::rdesc::ETextureColorSpace>(
            disk_info.color_space
        );
        info.flags      = disk_info.flags;
        info.mip_ranges = disk_info.mip_ranges;

        const auto data_size = static_cast<std::size_t>(header.data_size);
        if (!validateMipRanges(info, data_size))
            return lux::cxx::unexpected(EAssetError::WRONG_FILE_HEADER);

        auto pixels = file.subspan(
            static_cast<std::size_t>(header.data_offset),
            data_size
        );
        if (pixels.size() != data_size)
            return lux::cxx::unexpected(EAssetError::ABNORMAL_FILE_SIZE);

        auto texture = lux::rdesc::Texture::fromShared(
            std::move(info),
            std::move(pixels)
        );
        if (!texture)
            return lux::cxx::unexpected(EAssetError::WRONG_FILE_HEADER);
        return std::make_unique<lux::rdesc::Texture>(std::move(*texture));
    }
} // namespace

namespace lux::asset
{
    TextureCodec::TextureCodec(std::shared_ptr<AssetManager> manager)
        : AssetSerDeser(std::move(manager))
    {
    }

    TextureCodec::~TextureCodec() = default;

    lux::cxx::expected<std::unique_ptr<lux::rdesc::Texture>, EAssetError>
    TextureCodec::decodeData(
        const void* bytes,
        std::size_t length
    ) noexcept
    {
        if (bytes == nullptr || length == 0)
            return lux::cxx::unexpected(EAssetError::ABNORMAL_FILE_SIZE);
        return decodeData(
            lux::cxx::SharedBytes<>::copyOf(
                std::span<const std::byte>{
                    static_cast<const std::byte*>(bytes),
                    length
                }
            )
        );
    }

    lux::cxx::expected<std::unique_ptr<lux::rdesc::Texture>, EAssetError>
    TextureCodec::decodeData(lux::cxx::SharedBytes<> file) noexcept
    {
        if (file.empty())
            return lux::cxx::unexpected(EAssetError::ABNORMAL_FILE_SIZE);

        AssetFileHeader header{};
        TextureAssetInfo texture_info{};
        const EAssetError error =
            loadHeader<TextureAssetInfo, EAssetType::TEXTURE>(
                file.view(),
                header,
                texture_info
            );
        if (error != EAssetError::SUCCESS)
            return lux::cxx::unexpected(error);
        return decodeTextureFromImage(
            std::move(file),
            header,
            texture_info
        );
    }

    lux::cxx::expected<std::vector<std::byte>, EAssetError>
    TextureCodec::encodeData(
        const asset_id_t& id,
        const lux::rdesc::Texture& texture) noexcept
    {
        const auto& info = texture.info();
        if (id.is_nil() || texture.data() == nullptr || texture.size() == 0u ||
            info.width <= 0 || info.height <= 0 || info.channel <= 0 ||
            !validateMipRanges(info, texture.size()))
        {
            return lux::cxx::unexpected(EAssetError::WRONG_FILE_HEADER);
        }

        TextureAssetInfo disk_info{};
        disk_info.width = info.width;
        disk_info.height = info.height;
        disk_info.channel = info.channel;
        disk_info.layers = info.layers;
        disk_info.mip_count = info.mip_count;
        disk_info.pixel_format = static_cast<std::uint32_t>(
            info.pixel_format);
        disk_info.color_space = static_cast<std::uint32_t>(
            info.color_space);
        disk_info.flags = info.flags;
        disk_info.mip_ranges = info.mip_ranges;

        AssetInfo asset_info{};
        asset_info.id = id;
        asset_info.type = EAssetType::TEXTURE;
        constexpr std::string_view name{"Generated Texture"};
        std::memcpy(
            asset_info.display_name,
            name.data(),
            name.size());
        auto image = makeHeader<TextureAssetInfo, EAssetType::TEXTURE>(
            asset_info,
            disk_info,
            texture.size());
        const auto* first = static_cast<const std::byte*>(texture.data());
        image.insert(image.end(), first, first + texture.size());

        const auto verified = decodeData(image.data(), image.size());
        if (!verified)
            return lux::cxx::unexpected(verified.error());
        return image;
    }

    lux::cxx::expected<std::unique_ptr<LuxAsset>, EAssetError>
    TextureCodec::fromLuxAssetStream(std::istream& stream)
    {
        stream.seekg(0, std::ios::end);
        const auto end = stream.tellg();
        if (end <= 0)
            return lux::cxx::unexpected(EAssetError::ABNORMAL_FILE_SIZE);
        const auto file_size = static_cast<std::size_t>(end);
        stream.seekg(0, std::ios::beg);

        auto owner = std::make_shared<std::vector<std::byte>>(file_size);
        stream.read(
            reinterpret_cast<char*>(owner->data()),
            static_cast<std::streamsize>(file_size)
        );
        if (!stream)
            return lux::cxx::unexpected(EAssetError::ASSET_DESERIALIZE_FAIL);

        AssetFileHeader header{};
        TextureAssetInfo texture_info{};
        const EAssetError error =
            loadHeader<TextureAssetInfo, EAssetType::TEXTURE>(
                *owner,
                header,
                texture_info
            );
        if (error != EAssetError::SUCCESS)
            return lux::cxx::unexpected(error);

        auto shared_file = lux::cxx::SharedBytes<>::fromOwner(
            owner,
            std::span<const std::byte>{owner->data(), owner->size()}
        );
        if (shared_file.empty())
            return lux::cxx::unexpected(EAssetError::OUT_OF_MEMORY);

        auto texture = decodeTextureFromImage(
            std::move(shared_file),
            header,
            texture_info
        );
        if (!texture)
            return lux::cxx::unexpected(texture.error());

        auto asset = std::make_unique<TextureAsset>(
            std::make_unique<AssetInfo>(header.info)
        );
        asset->setData(std::move(*texture));
        return asset;
    }

    EAssetError TextureCodec::exportAsLuxAssetStream(
        const LuxAsset& asset,
        std::ofstream& stream
    )
    {
        const auto* texture_asset = asset.as<TextureAsset>();
        if (texture_asset == nullptr || texture_asset->data() == nullptr)
            return EAssetError::UNKNOWN_ERROR;

        const auto* texture = texture_asset->data();
        const auto& info = texture->info();
        if (!validateMipRanges(info, texture->size()))
            return EAssetError::WRONG_FILE_HEADER;

        TextureAssetInfo disk_info{};
        disk_info.width        = info.width;
        disk_info.height       = info.height;
        disk_info.channel      = info.channel;
        disk_info.layers       = info.layers;
        disk_info.mip_count    = info.mip_count;
        disk_info.pixel_format = static_cast<std::uint32_t>(
            info.pixel_format
        );
        disk_info.color_space = static_cast<std::uint32_t>(
            info.color_space
        );
        disk_info.flags      = info.flags;
        disk_info.mip_ranges = info.mip_ranges;

        auto header = makeHeader<TextureAssetInfo, EAssetType::TEXTURE>(
            *texture_asset->info(),
            disk_info,
            texture->size()
        );
        stream.write(
            reinterpret_cast<const char*>(header.data()),
            static_cast<std::streamsize>(header.size())
        );
        stream.write(
            reinterpret_cast<const char*>(texture->data()),
            static_cast<std::streamsize>(texture->size())
        );
        stream.flush();
        return stream ? EAssetError::SUCCESS : EAssetError::WRITE_FILE_FAIL;
    }
} // namespace lux::asset
