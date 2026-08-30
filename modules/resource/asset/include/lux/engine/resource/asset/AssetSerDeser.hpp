#pragma once

#include <cstddef>
#include <cstdint>

namespace lux::asset
{
    enum class EAssetCodecError : std::uint8_t
    {
        CODEC_FAILURE,
        OUT_OF_MEMORY,
    };

    inline constexpr std::uint32_t kNoLegacyAssetTypeTag = 0xFFFFFFFFU;

    enum class EAssetDecodeError : std::uint8_t
    {
        INVALID_ASSET_ID,
        ASSET_ID_MISMATCH,
        TRUNCATED,
        UNSUPPORTED_VERSION,
        INVALID_LAYOUT,
        INVALID_MAGIC,
        INVALID_TYPE,
        INVALID_PAYLOAD,
        LIMIT_EXCEEDED,
        ALLOCATION_FAILURE,
    };

    struct AssetDecodeFailure final
    {
        EAssetDecodeError code{EAssetDecodeError::INVALID_PAYLOAD};
        std::size_t offset{};
    };

    enum class EAssetEncodeError : std::uint8_t
    {
        INVALID_ASSET,
        INVALID_PAYLOAD,
        LIMIT_EXCEEDED,
        ALLOCATION_FAILURE,
    };

    struct AssetEncodeFailure final
    {
        EAssetEncodeError code{EAssetEncodeError::INVALID_ASSET};
        std::size_t offset{};
    };

    struct AssetDecodeLimits final
    {
        AssetDecodeLimits() = delete;

        constexpr AssetDecodeLimits(
            std::size_t image_bytes,
            std::size_t decoded_bytes,
            std::size_t auxiliary_payloads
        ) noexcept
            : max_image_bytes(image_bytes),
              max_decoded_bytes(decoded_bytes),
              max_auxiliary_payloads(auxiliary_payloads)
        {
        }

        std::size_t max_image_bytes;
        std::size_t max_decoded_bytes;
        std::size_t max_auxiliary_payloads;
    };

    struct AssetEncodeLimits final
    {
        AssetEncodeLimits() = delete;

        explicit constexpr AssetEncodeLimits(std::size_t encoded_bytes) noexcept
            : max_encoded_bytes(encoded_bytes)
        {
        }

        std::size_t max_encoded_bytes;
    };
} // namespace lux::asset
