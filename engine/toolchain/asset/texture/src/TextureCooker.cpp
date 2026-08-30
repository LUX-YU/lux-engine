#include <lux/engine/toolchain/asset/texture/TextureCooker.hpp>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_FAILURE_USERMSG
#include <stb_image.h>

#include <bc7enc.h>
#include <rgbcx.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <new>
#include <utility>
#include <vector>

namespace lux::toolchain
{
    namespace
    {
        using lux::rdesc::ETextureAssetFlags;
        using lux::rdesc::ETexturePixelFormat;
        using lux::rdesc::TextureInfo;
        using lux::rdesc::TextureMipRange;

        [[nodiscard]] TextureCookFailure failure(ETextureCookError code, std::size_t offset = 0U) noexcept
        {
            return TextureCookFailure{code, offset};
        }

        [[nodiscard]] bool isRawFormat(ETexturePixelFormat format) noexcept
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

        [[nodiscard]] bool isSupportedCompressedFormat(ETexturePixelFormat format) noexcept
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

        [[nodiscard]] std::size_t blockBytes(ETexturePixelFormat format) noexcept
        {
            return format == ETexturePixelFormat::BC1_SRGB ? 8U : 16U;
        }

        [[nodiscard]] std::int32_t channels(ETexturePixelFormat format) noexcept
        {
            if (format == ETexturePixelFormat::R8_UNORM)
                return 1;
            if (format == ETexturePixelFormat::RG8_UNORM || format == ETexturePixelFormat::BC5_UNORM)
                return 2;
            return 4;
        }

        [[nodiscard]] bool validConfiguration(const TextureCookConfiguration& configuration) noexcept
        {
            const bool valid_format = isRawFormat(configuration.output_format) ||
                isSupportedCompressedFormat(configuration.output_format);
            const bool valid_color_space = configuration.color_space >= lux::rdesc::ETextureColorSpace::SRGB &&
                configuration.color_space <= lux::rdesc::ETextureColorSpace::DATA;
            return valid_format && valid_color_space;
        }

        [[nodiscard]] bool checkedImageBytes(int width, int height, std::size_t& result) noexcept
        {
            if (width <= 0 || height <= 0)
                return false;
            const auto unsigned_width = static_cast<std::size_t>(width);
            const auto unsigned_height = static_cast<std::size_t>(height);
            if (unsigned_height > (std::numeric_limits<std::size_t>::max)() / unsigned_width)
                return false;
            const auto pixels = unsigned_width * unsigned_height;
            if (pixels > (std::numeric_limits<std::size_t>::max)() / 4U)
                return false;
            result = pixels * 4U;
            return true;
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
            destination_width = (std::max)(1, source_width / 2);
            destination_height = (std::max)(1, source_height / 2);
            destination.resize(
                static_cast<std::size_t>(destination_width) *
                static_cast<std::size_t>(destination_height) * 4U
            );
            for (int y = 0; y < destination_height; ++y)
            {
                for (int x = 0; x < destination_width; ++x)
                {
                    std::array<std::uint32_t, 4U> sum{};
                    for (int kernel_y = 0; kernel_y < 2; ++kernel_y)
                    {
                        for (int kernel_x = 0; kernel_x < 2; ++kernel_x)
                        {
                            const int source_x = (std::min)(source_width - 1, x * 2 + kernel_x);
                            const int source_y = (std::min)(source_height - 1, y * 2 + kernel_y);
                            const auto source_index = (
                                static_cast<std::size_t>(source_y) * static_cast<std::size_t>(source_width) +
                                static_cast<std::size_t>(source_x)
                            ) * 4U;
                            for (std::size_t channel = 0U; channel < sum.size(); ++channel)
                                sum[channel] += source[source_index + channel];
                        }
                    }
                    const auto destination_index = (
                        static_cast<std::size_t>(y) * static_cast<std::size_t>(destination_width) +
                        static_cast<std::size_t>(x)
                    ) * 4U;
                    for (std::size_t channel = 0U; channel < sum.size(); ++channel)
                        destination[destination_index + channel] = static_cast<std::uint8_t>(sum[channel] / 4U);
                }
            }
        }

        struct BcEncoderState final
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

        [[nodiscard]] bool encodeCompressedMip(
            ETexturePixelFormat format,
            const std::vector<std::uint8_t>& rgba,
            int width,
            int height,
            std::vector<std::byte>& output
        )
        {
            const auto& state = bcEncoderState();
            const std::uint32_t blocks_x = (static_cast<std::uint32_t>(width) + 3U) / 4U;
            const std::uint32_t blocks_y = (static_cast<std::uint32_t>(height) + 3U) / 4U;
            const std::size_t block_bytes = blockBytes(format);
            const auto block_count = static_cast<std::size_t>(blocks_x) * blocks_y;
            if (block_count > (std::numeric_limits<std::size_t>::max)() / block_bytes)
                return false;
            output.resize(block_count * block_bytes);

            std::array<std::uint8_t, 64U> block_rgba{};
            std::size_t output_offset{};
            for (std::uint32_t block_y = 0U; block_y < blocks_y; ++block_y)
            {
                for (std::uint32_t block_x = 0U; block_x < blocks_x; ++block_x)
                {
                    for (std::uint32_t y = 0U; y < 4U; ++y)
                    {
                        for (std::uint32_t x = 0U; x < 4U; ++x)
                        {
                            const int source_x = (std::min)(
                                width - 1,
                                static_cast<int>(block_x * 4U + x)
                            );
                            const int source_y = (std::min)(
                                height - 1,
                                static_cast<int>(block_y * 4U + y)
                            );
                            const auto source_index = (
                                static_cast<std::size_t>(source_y) * static_cast<std::size_t>(width) +
                                static_cast<std::size_t>(source_x)
                            ) * 4U;
                            const auto block_index = (static_cast<std::size_t>(y) * 4U + x) * 4U;
                            std::memcpy(block_rgba.data() + block_index, rgba.data() + source_index, 4U);
                        }
                    }

                    auto* destination = reinterpret_cast<unsigned char*>(output.data() + output_offset);
                    switch (format)
                    {
                    case ETexturePixelFormat::BC1_SRGB:
                        rgbcx::encode_bc1(10, destination, block_rgba.data(), false, false);
                        break;
                    case ETexturePixelFormat::BC3_SRGB:
                        rgbcx::encode_bc3(10, destination, block_rgba.data());
                        break;
                    case ETexturePixelFormat::BC5_UNORM:
                        rgbcx::encode_bc5(destination, block_rgba.data(), 0, 1, 4);
                        break;
                    case ETexturePixelFormat::BC7_SRGB:
                        bc7enc_compress_block(destination, block_rgba.data(), &state.bc7_parameters);
                        break;
                    default:
                        return false;
                    }
                    output_offset += block_bytes;
                }
            }
            return true;
        }

        [[nodiscard]] bool encodeRawMip(
            ETexturePixelFormat format,
            const std::vector<std::uint8_t>& rgba,
            std::vector<std::byte>& output
        )
        {
            const std::size_t pixel_count = rgba.size() / 4U;
            const std::size_t channel_count = static_cast<std::size_t>(channels(format));
            if (pixel_count > (std::numeric_limits<std::size_t>::max)() / channel_count)
                return false;
            output.resize(pixel_count * channel_count);
            for (std::size_t pixel = 0U; pixel < pixel_count; ++pixel)
            {
                for (std::size_t channel = 0U; channel < channel_count; ++channel)
                    output[pixel * channel_count + channel] = static_cast<std::byte>(rgba[pixel * 4U + channel]);
            }
            return true;
        }

        [[nodiscard]] bool buildTexture(
            const std::uint8_t* rgba,
            int width,
            int height,
            const TextureCookConfiguration& configuration,
            TextureInfo& output_info,
            std::vector<std::byte>& output_payload
        )
        {
            std::size_t source_size{};
            if (rgba == nullptr || !checkedImageBytes(width, height, source_size))
                return false;
            std::vector<std::uint8_t> current(rgba, rgba + source_size);
            output_info = {};
            output_info.width = width;
            output_info.height = height;
            output_info.channel = channels(configuration.output_format);
            output_info.pixel_format = configuration.output_format;
            output_info.color_space = configuration.color_space;
            output_info.layers = 1U;
            output_info.flags = configuration.no_mips
                ? lux::rdesc::toUnderlying(ETextureAssetFlags::NO_MIPS)
                : lux::rdesc::toUnderlying(ETextureAssetFlags::NONE);
            if (isSupportedCompressedFormat(configuration.output_format))
                output_info.flags |= lux::rdesc::toUnderlying(ETextureAssetFlags::COMPRESSED);

            int mip_width = width;
            int mip_height = height;
            while (output_info.mip_count <= lux::rdesc::kTextureMaxMipCount)
            {
                const std::uint32_t level = output_info.mip_count - 1U;
                std::vector<std::byte> level_payload;
                const bool encoded = isRawFormat(configuration.output_format)
                    ? encodeRawMip(configuration.output_format, current, level_payload)
                    : encodeCompressedMip(configuration.output_format, current, mip_width, mip_height, level_payload);
                if (!encoded)
                    return false;
                const std::uint64_t offset = output_payload.size();
                output_payload.insert(output_payload.end(), level_payload.begin(), level_payload.end());
                output_info.mip_ranges[level] = TextureMipRange{
                    offset,
                    static_cast<std::uint64_t>(level_payload.size()),
                    static_cast<std::uint32_t>(mip_width),
                    static_cast<std::uint32_t>(mip_height)
                };
                if (configuration.no_mips || (mip_width == 1 && mip_height == 1))
                    break;
                if (output_info.mip_count == lux::rdesc::kTextureMaxMipCount)
                    return false;
                std::vector<std::uint8_t> next;
                int next_width{};
                int next_height{};
                downsampleRgbaBox(current, mip_width, mip_height, next, next_width, next_height);
                current.swap(next);
                mip_width = next_width;
                mip_height = next_height;
                ++output_info.mip_count;
            }
            return true;
        }
    } // namespace

    lux::cxx::expected<std::shared_ptr<const lux::asset::TextureAsset>, TextureCookFailure> cookTexture(
        lux::asset::AssetInfo metadata,
        lux::cxx::SharedBytes<> authoring_image,
        const TextureCookConfiguration& configuration
    ) noexcept
    {
        if (metadata.id.isNull() || authoring_image.empty())
            return lux::cxx::unexpected(failure(ETextureCookError::INVALID_SOURCE));
        if (!validConfiguration(configuration))
        {
            const bool unsupported = !isRawFormat(configuration.output_format) &&
                !isSupportedCompressedFormat(configuration.output_format);
            return lux::cxx::unexpected(failure(
                unsupported ? ETextureCookError::UNSUPPORTED_FORMAT : ETextureCookError::INVALID_OPTIONS
            ));
        }
        if (authoring_image.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)()))
            return lux::cxx::unexpected(failure(ETextureCookError::RANGE_OVERFLOW));

        try
        {
            int width{};
            int height{};
            int source_channels{};
            std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> decoded{
                stbi_load_from_memory(
                reinterpret_cast<const stbi_uc*>(authoring_image.data()),
                static_cast<int>(authoring_image.size()),
                &width,
                &height,
                &source_channels,
                STBI_rgb_alpha
                ),
                &stbi_image_free
            };
            if (!decoded)
                return lux::cxx::unexpected(failure(ETextureCookError::DECODE_FAILED));

            TextureInfo texture_info{};
            std::vector<std::byte> payload;
            const bool built = buildTexture(
                decoded.get(),
                width,
                height,
                configuration,
                texture_info,
                payload
            );
            if (!built || payload.empty())
                return lux::cxx::unexpected(failure(ETextureCookError::RANGE_OVERFLOW));

            auto owner = std::make_shared<std::vector<std::byte>>(std::move(payload));
            auto shared = lux::cxx::SharedBytes<>::fromOwner(owner, *owner);
            auto texture = lux::rdesc::Texture::fromShared(std::move(texture_info), std::move(shared));
            if (!texture)
                return lux::cxx::unexpected(failure(ETextureCookError::INVALID_COOKED_TEXTURE));
            auto asset = lux::asset::TextureAsset::create(
                std::move(metadata),
                std::make_shared<const lux::rdesc::Texture>(std::move(*texture))
            );
            if (!asset)
                return lux::cxx::unexpected(failure(ETextureCookError::INVALID_COOKED_TEXTURE));
            return *asset;
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(failure(ETextureCookError::ALLOCATION_FAILURE));
        }
        catch (...)
        {
            return lux::cxx::unexpected(failure(ETextureCookError::DECODE_FAILED));
        }
    }
} // namespace lux::toolchain
