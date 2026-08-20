#include <lux/engine/toolchain/asset/texture/TextureImporter.hpp>

#include <lux/engine/resource/asset/detail/AssetManagerImpl.hpp>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <bc7enc.h>
#include <rgbcx.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <utility>
#include <vector>

namespace
{
    using lux::rdesc::ETextureAssetFlags;
    using lux::rdesc::ETexturePixelFormat;
    using lux::rdesc::TextureInfo;
    using lux::rdesc::TextureMipRange;
    using lux::rdesc::kTextureMaxMipCount;

    [[nodiscard]] bool supportsRawOutputFormat(
        ETexturePixelFormat format
    ) noexcept
    {
        switch (format)
        {
        case ETexturePixelFormat::RGBA8_UNORM:
        case ETexturePixelFormat::RGBA8_SRGB:
        case ETexturePixelFormat::RG8_UNORM:
        case ETexturePixelFormat::R8_UNORM:
            return true;
        default:
            return false;
        }
    }

    [[nodiscard]] bool supportsCompressedEncoding(
        ETexturePixelFormat format
    ) noexcept
    {
        switch (format)
        {
        case ETexturePixelFormat::BC1_SRGB:
        case ETexturePixelFormat::BC3_SRGB:
        case ETexturePixelFormat::BC5_UNORM:
        case ETexturePixelFormat::BC7_SRGB:
            return true;
        default:
            return false;
        }
    }

    [[nodiscard]] std::size_t blockBytesForCompressed(
        ETexturePixelFormat format
    ) noexcept
    {
        switch (format)
        {
        case ETexturePixelFormat::BC1_SRGB:
            return 8;
        case ETexturePixelFormat::BC3_SRGB:
        case ETexturePixelFormat::BC5_UNORM:
        case ETexturePixelFormat::BC7_SRGB:
            return 16;
        default:
            return 0;
        }
    }

    [[nodiscard]] std::uint32_t compressedChannelCount(
        ETexturePixelFormat format
    ) noexcept
    {
        return format == ETexturePixelFormat::BC5_UNORM ? 2u : 4u;
    }

    void clearMipRanges(
        std::array<TextureMipRange, kTextureMaxMipCount>& ranges
    ) noexcept
    {
        for (auto& range : ranges)
            range = {};
    }

    void assignSingleMip(
        TextureInfo& info,
        std::size_t payload_size
    ) noexcept
    {
        info.mip_count = 1;
        clearMipRanges(info.mip_ranges);
        info.mip_ranges[0] = TextureMipRange{
            .offset = 0,
            .size = static_cast<std::uint64_t>(payload_size),
            .width = static_cast<std::uint32_t>(
                std::max(info.width, 0)
            ),
            .height = static_cast<std::uint32_t>(
                std::max(info.height, 0)
            )
        };
    }

    void downsampleRgbaBox(
        const std::vector<std::uint8_t>& source,
        int source_width,
        int source_height,
        std::vector<std::uint8_t>& destination,
        int& destination_width,
        int& destination_height
    )
    {
        destination_width = std::max(1, source_width / 2);
        destination_height = std::max(1, source_height / 2);
        destination.resize(
            static_cast<std::size_t>(destination_width) *
            static_cast<std::size_t>(destination_height) * 4u
        );

        for (int y = 0; y < destination_height; ++y)
        {
            for (int x = 0; x < destination_width; ++x)
            {
                std::uint32_t sum[4]{0, 0, 0, 0};
                for (int kernel_y = 0; kernel_y < 2; ++kernel_y)
                {
                    for (int kernel_x = 0; kernel_x < 2; ++kernel_x)
                    {
                        const int source_x = std::min(
                            source_width - 1,
                            x * 2 + kernel_x
                        );
                        const int source_y = std::min(
                            source_height - 1,
                            y * 2 + kernel_y
                        );
                        const auto source_index =
                            (static_cast<std::size_t>(source_y) *
                                static_cast<std::size_t>(source_width) +
                                static_cast<std::size_t>(source_x)) * 4u;
                        for (std::size_t channel = 0; channel < 4; ++channel)
                            sum[channel] += source[source_index + channel];
                    }
                }

                const auto destination_index =
                    (static_cast<std::size_t>(y) *
                        static_cast<std::size_t>(destination_width) +
                        static_cast<std::size_t>(x)) * 4u;
                for (std::size_t channel = 0; channel < 4; ++channel)
                {
                    destination[destination_index + channel] =
                        static_cast<std::uint8_t>(sum[channel] / 4u);
                }
            }
        }
    }

    struct BcEncoderState
    {
        bc7enc_compress_block_params bc7_parameters{};

        BcEncoderState() noexcept
        {
            rgbcx::init();
            bc7enc_compress_block_init();
            bc7enc_compress_block_params_init(&bc7_parameters);
        }
    };

    [[nodiscard]] const BcEncoderState& bcEncoderState() noexcept
    {
        static const BcEncoderState state{};
        return state;
    }

    [[nodiscard]] bool encodeCompressedMipLevel(
        ETexturePixelFormat format,
        const std::vector<std::uint8_t>& rgba,
        int width,
        int height,
        std::vector<std::byte>& output
    )
    {
        // rgbcx requires an explicit one-time initialization for BC1/BC3/BC5
        // as well as BC7.  Keeping this reference outside the format switch
        // prevents Release builds from hiding the missing initialization after
        // the Debug assertion is compiled out.
        const auto& encoder_state = bcEncoderState();
        const std::size_t block_bytes = blockBytesForCompressed(format);
        const auto block_width =
            (static_cast<std::uint32_t>(width) + 3u) / 4u;
        const auto block_height =
            (static_cast<std::uint32_t>(height) + 3u) / 4u;
        output.resize(
            static_cast<std::size_t>(block_width) * block_height * block_bytes
        );

        std::uint8_t block_rgba[64]{};
        std::size_t output_offset = 0;
        for (std::uint32_t block_y = 0; block_y < block_height; ++block_y)
        {
            for (std::uint32_t block_x = 0; block_x < block_width; ++block_x)
            {
                for (std::uint32_t pixel_y = 0; pixel_y < 4; ++pixel_y)
                {
                    for (std::uint32_t pixel_x = 0; pixel_x < 4; ++pixel_x)
                    {
                        const int source_x = std::min(
                            width - 1,
                            static_cast<int>(block_x * 4u + pixel_x)
                        );
                        const int source_y = std::min(
                            height - 1,
                            static_cast<int>(block_y * 4u + pixel_y)
                        );
                        const auto source_index =
                            (static_cast<std::size_t>(source_y) *
                                static_cast<std::size_t>(width) +
                                static_cast<std::size_t>(source_x)) * 4u;
                        const auto block_index =
                            (static_cast<std::size_t>(pixel_y) * 4u + pixel_x) *
                            4u;
                        std::memcpy(
                            block_rgba + block_index,
                            rgba.data() + source_index,
                            4u
                        );
                    }
                }

                auto* destination = reinterpret_cast<unsigned char*>(
                    output.data() + output_offset
                );
                switch (format)
                {
                case ETexturePixelFormat::BC1_SRGB:
                    rgbcx::encode_bc1(
                        10,
                        destination,
                        block_rgba,
                        false,
                        false
                    );
                    break;
                case ETexturePixelFormat::BC3_SRGB:
                    rgbcx::encode_bc3(10, destination, block_rgba);
                    break;
                case ETexturePixelFormat::BC5_UNORM:
                    rgbcx::encode_bc5(destination, block_rgba, 0, 1, 4);
                    break;
                case ETexturePixelFormat::BC7_SRGB:
                    bc7enc_compress_block(
                        destination,
                        block_rgba,
                        &encoder_state.bc7_parameters
                    );
                    break;
                default:
                    return false;
                }
                output_offset += block_bytes;
            }
        }
        return true;
    }

    [[nodiscard]] bool buildTexturePayloadFromRgba(
        const std::uint8_t* rgba_pixels,
        int width,
        int height,
        const lux::toolchain::TextureImportConfig& config,
        TextureInfo& output_info,
        std::vector<std::byte>& output_payload
    )
    {
        if (rgba_pixels == nullptr || width <= 0 || height <= 0)
            return false;

        output_info = {};
        output_info.width = width;
        output_info.height = height;
        output_info.layers = 1;
        output_info.pixel_format = config.output_format;
        output_info.color_space = config.color_space;
        output_info.flags = config.flags;

        if (lux::rdesc::isCompressedFormat(config.output_format))
        {
            if (!supportsCompressedEncoding(config.output_format))
                return false;

            std::vector<std::uint8_t> current_rgba(
                rgba_pixels,
                rgba_pixels + static_cast<std::size_t>(width) *
                    static_cast<std::size_t>(height) * 4u
            );
            clearMipRanges(output_info.mip_ranges);

            int mip_width = width;
            int mip_height = height;
            std::uint32_t mip_count = 0;
            while (mip_count < kTextureMaxMipCount)
            {
                std::vector<std::byte> level_payload;
                if (!encodeCompressedMipLevel(
                    config.output_format,
                    current_rgba,
                    mip_width,
                    mip_height,
                    level_payload
                ))
                {
                    return false;
                }

                const auto offset = static_cast<std::uint64_t>(
                    output_payload.size()
                );
                output_payload.insert(
                    output_payload.end(),
                    level_payload.begin(),
                    level_payload.end()
                );
                output_info.mip_ranges[mip_count] = TextureMipRange{
                    .offset = offset,
                    .size = static_cast<std::uint64_t>(level_payload.size()),
                    .width = static_cast<std::uint32_t>(mip_width),
                    .height = static_cast<std::uint32_t>(mip_height)
                };

                ++mip_count;
                if (mip_width == 1 && mip_height == 1)
                    break;

                std::vector<std::uint8_t> next_rgba;
                int next_width = 1;
                int next_height = 1;
                downsampleRgbaBox(
                    current_rgba,
                    mip_width,
                    mip_height,
                    next_rgba,
                    next_width,
                    next_height
                );
                current_rgba.swap(next_rgba);
                mip_width = next_width;
                mip_height = next_height;
            }

            output_info.channel = static_cast<int>(
                compressedChannelCount(config.output_format)
            );
            output_info.mip_count = mip_count;
            output_info.flags |= lux::rdesc::toUnderlying(
                ETextureAssetFlags::COMPRESSED
            );
            return true;
        }

        if (!supportsRawOutputFormat(config.output_format))
            return false;

        const auto pixel_count = static_cast<std::size_t>(width) *
            static_cast<std::size_t>(height);
        switch (config.output_format)
        {
        case ETexturePixelFormat::RGBA8_UNORM:
        case ETexturePixelFormat::RGBA8_SRGB:
            output_info.channel = 4;
            output_payload.resize(pixel_count * 4u);
            std::memcpy(
                output_payload.data(),
                rgba_pixels,
                output_payload.size()
            );
            break;
        case ETexturePixelFormat::RG8_UNORM:
            output_info.channel = 2;
            output_payload.resize(pixel_count * 2u);
            for (std::size_t index = 0; index < pixel_count; ++index)
            {
                output_payload[index * 2u] = static_cast<std::byte>(
                    rgba_pixels[index * 4u]
                );
                output_payload[index * 2u + 1u] = static_cast<std::byte>(
                    rgba_pixels[index * 4u + 1u]
                );
            }
            break;
        case ETexturePixelFormat::R8_UNORM:
            output_info.channel = 1;
            output_payload.resize(pixel_count);
            for (std::size_t index = 0; index < pixel_count; ++index)
            {
                output_payload[index] = static_cast<std::byte>(
                    rgba_pixels[index * 4u]
                );
            }
            break;
        default:
            return false;
        }

        assignSingleMip(output_info, output_payload.size());
        return true;
    }

    [[nodiscard]] lux::cxx::expected<
        std::unique_ptr<lux::rdesc::Texture>,
        lux::asset::EAssetError
    > decodeAuthoringImage(
        const stbi_uc* memory,
        int length,
        const lux::toolchain::TextureImportConfig& config
    )
    {
        int width = 0;
        int height = 0;
        int source_channels = 0;
        stbi_set_flip_vertically_on_load(config.flip_vertically ? 1 : 0);
        stbi_uc* image = stbi_load_from_memory(
            memory,
            length,
            &width,
            &height,
            &source_channels,
            STBI_rgb_alpha
        );
        if (image == nullptr)
        {
            return lux::cxx::unexpected(
                lux::asset::EAssetError::UNKNOWN_ERROR
            );
        }

        TextureInfo texture_info{};
        std::vector<std::byte> payload;
        const bool built = buildTexturePayloadFromRgba(
            image,
            width,
            height,
            config,
            texture_info,
            payload
        );
        stbi_image_free(image);
        if (!built)
        {
            return lux::cxx::unexpected(
                lux::asset::EAssetError::UNSUPPORTED
            );
        }

        auto storage = std::make_shared<std::vector<std::byte>>(
            std::move(payload)
        );
        auto bytes = lux::cxx::SharedBytes<>::fromOwner(
            storage,
            std::span<const std::byte>{storage->data(), storage->size()}
        );
        if (bytes.empty())
        {
            return lux::cxx::unexpected(
                lux::asset::EAssetError::OUT_OF_MEMORY
            );
        }
        auto texture = lux::rdesc::Texture::fromShared(
            std::move(texture_info),
            std::move(bytes)
        );
        if (!texture)
        {
            return lux::cxx::unexpected(
                lux::asset::EAssetError::WRONG_FILE_HEADER
            );
        }
        return std::make_unique<lux::rdesc::Texture>(std::move(*texture));
    }
} // namespace

namespace lux::toolchain
{
    TextureImporter::TextureImporter(
        std::shared_ptr<lux::asset::AssetManager> manager
    )
        : TextureCodec(std::move(manager))
    {
    }

    TextureImporter::~TextureImporter() = default;

    lux::cxx::expected<
        lux::asset::AssetIDPair,
        lux::asset::EAssetError
    > TextureImporter::importFromFile(
        const std::filesystem::path& external_path
    )
    {
        int width = 0;
        int height = 0;
        int source_channels = 0;
        stbi_set_flip_vertically_on_load(config_.flip_vertically ? 1 : 0);
        stbi_uc* image = stbi_load(
            external_path.string().c_str(),
            &width,
            &height,
            &source_channels,
            STBI_rgb_alpha
        );
        if (image == nullptr)
        {
            return lux::cxx::unexpected(
                lux::asset::EAssetError::UNKNOWN_ERROR
            );
        }

        TextureInfo texture_info{};
        std::vector<std::byte> payload;
        const bool built = buildTexturePayloadFromRgba(
            image,
            width,
            height,
            config_,
            texture_info,
            payload
        );
        stbi_image_free(image);
        if (!built)
        {
            return lux::cxx::unexpected(
                lux::asset::EAssetError::UNSUPPORTED
            );
        }

        auto storage = std::make_shared<std::vector<std::byte>>(
            std::move(payload)
        );
        auto bytes = lux::cxx::SharedBytes<>::fromOwner(
            storage,
            std::span<const std::byte>{storage->data(), storage->size()}
        );
        if (bytes.empty())
        {
            return lux::cxx::unexpected(
                lux::asset::EAssetError::OUT_OF_MEMORY
            );
        }
        auto texture = lux::rdesc::Texture::fromShared(
            std::move(texture_info),
            std::move(bytes)
        );
        if (!texture)
        {
            return lux::cxx::unexpected(
                lux::asset::EAssetError::WRONG_FILE_HEADER
            );
        }

        auto asset = manager().createAssetSeeded<lux::asset::TextureAsset>(
            config_.deterministic_seed,
            std::make_unique<lux::rdesc::Texture>(std::move(*texture))
        );
        const auto id = asset->id();
        auto* pointer = asset.get();
        if (!submitAsset(std::move(asset)))
        {
            return lux::cxx::unexpected(
                lux::asset::EAssetError::ASSET_ALREADY_EXIST
            );
        }
        return lux::asset::AssetIDPair{pointer, id};
    }

    lux::cxx::expected<
        std::unique_ptr<lux::asset::LuxAsset>,
        lux::asset::EAssetError
    > TextureImporter::fromMemory(
        const void* memory,
        std::size_t length
    )
    {
        if (
            memory == nullptr || length == 0 ||
            length > static_cast<std::size_t>((std::numeric_limits<int>::max)())
        )
        {
            return lux::cxx::unexpected(
                lux::asset::EAssetError::ABNORMAL_FILE_SIZE
            );
        }

        auto texture = decodeAuthoringImage(
            static_cast<const stbi_uc*>(memory),
            static_cast<int>(length),
            config_
        );
        if (!texture)
            return lux::cxx::unexpected(texture.error());

        auto asset = std::make_unique<lux::asset::TextureAsset>(
            manager().createAssetInfo(
                lux::asset::EAssetType::TEXTURE,
                config_.deterministic_seed
            )
        );
        asset->setData(std::move(*texture));
        return asset;
    }
} // namespace lux::toolchain
