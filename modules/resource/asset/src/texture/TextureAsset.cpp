#include <lux/engine/resource/asset/texture/TextureAsset.hpp>

#include <lux/engine/resource/asset/CookedAssetImage.hpp>
#include <lux/engine/resource/asset/detail/CookedAssetWriter.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <new>
#include <utility>

namespace lux::asset
{
    namespace
    {
        using lux::rdesc::ETextureAssetFlags;
        using lux::rdesc::ETextureColorSpace;
        using lux::rdesc::ETexturePixelFormat;
        using lux::rdesc::TextureAssetInfo;
        using lux::rdesc::TextureInfo;

        struct FormatLayout final
        {
            std::uint32_t block_width{};
            std::uint32_t block_height{};
            std::uint32_t block_bytes{};
            std::int32_t channels{};
            bool compressed{};
        };

        [[nodiscard]] AssetDecodeFailure decodeFailure(
            EAssetDecodeError code,
            std::size_t offset = 0U
        ) noexcept
        {
            return AssetDecodeFailure{code, offset};
        }

        [[nodiscard]] AssetEncodeFailure encodeFailure(
            EAssetEncodeError code,
            std::size_t offset = 0U
        ) noexcept
        {
            return AssetEncodeFailure{code, offset};
        }

        [[nodiscard]] bool formatLayout(ETexturePixelFormat format, FormatLayout& result) noexcept
        {
            switch (format)
            {
            case ETexturePixelFormat::RGBA8_UNORM:
            case ETexturePixelFormat::RGBA8_SRGB:
                result = {1U, 1U, 4U, 4, false};
                return true;
            case ETexturePixelFormat::RG8_UNORM:
                result = {1U, 1U, 2U, 2, false};
                return true;
            case ETexturePixelFormat::R8_UNORM:
                result = {1U, 1U, 1U, 1, false};
                return true;
            case ETexturePixelFormat::RGBA16_SFLOAT:
                result = {1U, 1U, 8U, 4, false};
                return true;
            case ETexturePixelFormat::BC1_SRGB:
            case ETexturePixelFormat::ETC2_RGB8_UNORM:
            case ETexturePixelFormat::ETC2_RGB8_SRGB:
                result = {4U, 4U, 8U, 4, true};
                return true;
            case ETexturePixelFormat::BC3_SRGB:
            case ETexturePixelFormat::BC7_SRGB:
            case ETexturePixelFormat::ETC2_RGBA8_UNORM:
            case ETexturePixelFormat::ETC2_RGBA8_SRGB:
                result = {4U, 4U, 16U, 4, true};
                return true;
            case ETexturePixelFormat::BC5_UNORM:
                result = {4U, 4U, 16U, 2, true};
                return true;
            case ETexturePixelFormat::ASTC_4x4_UNORM:
            case ETexturePixelFormat::ASTC_4x4_SRGB:
                result = {4U, 4U, 16U, 4, true};
                return true;
            case ETexturePixelFormat::ASTC_6x6_UNORM:
            case ETexturePixelFormat::ASTC_6x6_SRGB:
                result = {6U, 6U, 16U, 4, true};
                return true;
            default:
                return false;
            }
        }

        [[nodiscard]] bool checkedMultiply(std::uint64_t value, std::uint64_t multiplier, std::uint64_t& result) noexcept
        {
            if (value != 0U && multiplier > (std::numeric_limits<std::uint64_t>::max)() / value)
                return false;
            result = value * multiplier;
            return true;
        }

        [[nodiscard]] bool validateTexture(const TextureInfo& info, std::size_t payload_size) noexcept
        {
            FormatLayout layout{};
            const auto all_flags = lux::rdesc::toUnderlying(ETextureAssetFlags::COMPRESSED) |
                lux::rdesc::toUnderlying(ETextureAssetFlags::PREMULTIPLIED_ALPHA) |
                lux::rdesc::toUnderlying(ETextureAssetFlags::NO_MIPS);
            const bool invalid_basic = info.width <= 0 || info.height <= 0 || info.layers == 0U ||
                info.mip_count == 0U || info.mip_count > lux::rdesc::kTextureMaxMipCount ||
                info.color_space < ETextureColorSpace::SRGB || info.color_space > ETextureColorSpace::DATA ||
                (info.flags & ~all_flags) != 0U || !formatLayout(info.pixel_format, layout);
            if (invalid_basic)
                return false;
            const bool compression_mismatch = lux::rdesc::hasTextureFlag(
                info.flags,
                ETextureAssetFlags::COMPRESSED
            ) != layout.compressed;
            const bool no_mips_mismatch = lux::rdesc::hasTextureFlag(
                info.flags,
                ETextureAssetFlags::NO_MIPS
            ) && info.mip_count != 1U;
            if (compression_mismatch || no_mips_mismatch || info.channel != layout.channels)
                return false;

            std::uint64_t expected_offset{};
            for (std::uint32_t level = 0U; level < info.mip_count; ++level)
            {
                const std::uint32_t width = (std::max)(1U, static_cast<std::uint32_t>(info.width) >> level);
                const std::uint32_t height = (std::max)(1U, static_cast<std::uint32_t>(info.height) >> level);
                const std::uint64_t blocks_x = (width + layout.block_width - 1U) / layout.block_width;
                const std::uint64_t blocks_y = (height + layout.block_height - 1U) / layout.block_height;
                std::uint64_t expected_size{};
                if (!checkedMultiply(blocks_x, blocks_y, expected_size) ||
                    !checkedMultiply(expected_size, layout.block_bytes, expected_size) ||
                    !checkedMultiply(expected_size, info.layers, expected_size))
                {
                    return false;
                }
                const auto& mip = info.mip_ranges[level];
                if (mip.offset != expected_offset || mip.size != expected_size ||
                    mip.width != width || mip.height != height)
                {
                    return false;
                }
                if (expected_size > (std::numeric_limits<std::uint64_t>::max)() - expected_offset)
                    return false;
                expected_offset += expected_size;
            }
            return expected_offset == payload_size;
        }

        [[nodiscard]] TextureInfo toRuntimeInfo(const TextureAssetInfo& disk) noexcept
        {
            TextureInfo info{};
            info.width = disk.width;
            info.height = disk.height;
            info.channel = disk.channel;
            info.layers = disk.layers;
            info.mip_count = disk.mip_count;
            info.pixel_format = static_cast<ETexturePixelFormat>(disk.pixel_format);
            info.color_space = static_cast<ETextureColorSpace>(disk.color_space);
            info.flags = disk.flags;
            info.mip_ranges = disk.mip_ranges;
            return info;
        }

        [[nodiscard]] TextureAssetInfo toDiskInfo(const TextureInfo& info) noexcept
        {
            TextureAssetInfo disk{};
            disk.width = info.width;
            disk.height = info.height;
            disk.channel = info.channel;
            disk.layers = info.layers;
            disk.mip_count = info.mip_count;
            disk.pixel_format = static_cast<std::uint32_t>(info.pixel_format);
            disk.color_space = static_cast<std::uint32_t>(info.color_space);
            disk.flags = info.flags;
            disk.mip_ranges = info.mip_ranges;
            return disk;
        }
    } // namespace

    TextureAsset::TextureAsset(
        AssetInfo info,
        std::shared_ptr<const lux::rdesc::Texture> data,
        std::vector<AssetAuxiliaryPayload> auxiliary
    ) noexcept
        : TAsset(std::move(info), std::move(data), std::move(auxiliary))
    {
    }

    lux::cxx::expected<std::shared_ptr<const TextureAsset>, AssetDecodeFailure> TextureAsset::create(
        AssetInfo info,
        std::shared_ptr<const lux::rdesc::Texture> data,
        std::vector<AssetAuxiliaryPayload> auxiliary
    ) noexcept
    {
        if (info.id.isNull() || !data || !validateTexture(data->info(), data->size()))
            return lux::cxx::unexpected(decodeFailure(EAssetDecodeError::INVALID_PAYLOAD));
        info.type = asset_type;
        try
        {
            std::sort(
                auxiliary.begin(),
                auxiliary.end(),
                [](const AssetAuxiliaryPayload& left, const AssetAuxiliaryPayload& right) noexcept {
                    return left.tag < right.tag;
                }
            );
            for (std::size_t index = 0U; index < auxiliary.size(); ++index)
            {
                const bool invalid = auxiliary[index].tag == 0U || auxiliary[index].bytes.empty();
                const bool duplicate = index != 0U && auxiliary[index - 1U].tag == auxiliary[index].tag;
                if (invalid || duplicate)
                    return lux::cxx::unexpected(decodeFailure(EAssetDecodeError::INVALID_PAYLOAD, index));
            }
            return std::shared_ptr<const TextureAsset>(
                new TextureAsset(std::move(info), std::move(data), std::move(auxiliary))
            );
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(decodeFailure(EAssetDecodeError::ALLOCATION_FAILURE));
        }
        catch (...)
        {
            return lux::cxx::unexpected(decodeFailure(EAssetDecodeError::INVALID_PAYLOAD));
        }
    }

    lux::cxx::expected<std::shared_ptr<const TextureAsset>, AssetDecodeFailure>
    TAssetSerDeser<TextureAsset>::decode(
        AssetId requested,
        lux::cxx::SharedBytes<> cooked_image,
        const AssetDecodeLimits& limits
    ) noexcept
    {
        auto image = inspectCookedAssetImage(requested, std::move(cooked_image), limits);
        if (!image)
            return lux::cxx::unexpected(image.error());
        if (image->magic() != TextureAsset::primary_magic)
            return lux::cxx::unexpected(decodeFailure(EAssetDecodeError::INVALID_MAGIC));
        if (image->metadata().legacy_type_tag != TextureAsset::legacy_type_tag)
            return lux::cxx::unexpected(decodeFailure(EAssetDecodeError::INVALID_TYPE));
        if (image->information().size() != sizeof(TextureAssetInfo))
            return lux::cxx::unexpected(decodeFailure(EAssetDecodeError::INVALID_LAYOUT));
        try
        {
            TextureAssetInfo disk{};
            std::memcpy(&disk, image->information().data(), sizeof(disk));
            auto info = toRuntimeInfo(disk);
            if (!validateTexture(info, image->data().size()))
                return lux::cxx::unexpected(decodeFailure(EAssetDecodeError::INVALID_PAYLOAD));
            auto texture = lux::rdesc::Texture::fromShared(std::move(info), image->data());
            if (!texture)
                return lux::cxx::unexpected(decodeFailure(EAssetDecodeError::INVALID_PAYLOAD));

            std::vector<AssetAuxiliaryPayload> auxiliary(
                image->auxiliaryPayloads().begin(),
                image->auxiliaryPayloads().end()
            );
            AssetInfo asset_info{
                image->metadata().id,
                TextureAsset::asset_type,
                image->metadata().date,
                image->metadata().display_name,
                image->metadata().source_path,
                image->metadata().source_mtime
            };
            return TextureAsset::create(
                std::move(asset_info),
                std::make_shared<const lux::rdesc::Texture>(std::move(*texture)),
                std::move(auxiliary)
            );
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(decodeFailure(EAssetDecodeError::ALLOCATION_FAILURE));
        }
        catch (...)
        {
            return lux::cxx::unexpected(decodeFailure(EAssetDecodeError::INVALID_PAYLOAD));
        }
    }

    lux::cxx::expected<std::vector<std::byte>, AssetEncodeFailure> TAssetSerDeser<TextureAsset>::encode(
        const TextureAsset& asset,
        const AssetEncodeLimits& limits
    ) noexcept
    {
        const auto& texture = asset.data();
        if (asset.id().isNull() || asset.type() != TextureAsset::asset_type ||
            !validateTexture(texture.info(), texture.size()))
        {
            return lux::cxx::unexpected(encodeFailure(EAssetEncodeError::INVALID_ASSET));
        }
        const TextureAssetInfo disk = toDiskInfo(texture.info());
        const auto information = std::span<const std::byte>{
            reinterpret_cast<const std::byte*>(&disk),
            sizeof(disk)
        };
        return detail::encodeCookedAssetImage(
            detail::CookedAssetWriteRequest{
                TextureAsset::primary_magic,
                TextureAsset::legacy_type_tag,
                asset.info(),
                information,
                texture.pixels().view(),
                asset.auxiliaryPayloads()
            },
            limits
        );
    }
} // namespace lux::asset
