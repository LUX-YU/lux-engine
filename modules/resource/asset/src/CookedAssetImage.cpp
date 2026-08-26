#include <lux/engine/resource/asset/CookedAssetImage.hpp>

#include <algorithm>
#include <cstring>
#include <limits>
#include <type_traits>

namespace lux::asset
{
    namespace
    {
        struct WireAssetInfoV1 final
        {
            std::array<std::uint8_t, 16> id{};
            std::uint32_t type{};
            std::uint64_t date{};
        };

        struct WireAssetInfoV2 final
        {
            std::array<std::uint8_t, 16> id{};
            std::uint32_t type{};
            std::uint64_t date{};
            std::array<char, 64> display_name{};
            std::array<char, 256> source_path{};
            std::uint64_t source_mtime{};
        };

        template <class Info>
        struct WireHeader final
        {
            std::uint32_t magic{};
            std::uint32_t version{};
            std::uint64_t info_offset{};
            std::uint64_t info_size{};
            std::uint64_t data_offset{};
            std::uint64_t data_size{};
            Info metadata;
        };

        using WireHeaderV1 = WireHeader<WireAssetInfoV1>;
        using WireHeaderV2 = WireHeader<WireAssetInfoV2>;

        static_assert(sizeof(WireAssetInfoV1) == 32u);
        static_assert(sizeof(WireAssetInfoV2) == 360u);
        static_assert(sizeof(WireHeaderV1) == 72u);
        static_assert(sizeof(WireHeaderV2) == 400u);
        static_assert(std::is_trivially_copyable_v<WireHeaderV1>);
        static_assert(std::is_trivially_copyable_v<WireHeaderV2>);

        [[nodiscard]] bool validRange(
            std::uint64_t offset,
            std::uint64_t size,
            std::size_t total
        ) noexcept
        {
            return offset <= total && size <= total - offset;
        }

        [[nodiscard]] bool validAuxiliaryPayloads(
            std::span<const std::byte> bytes
        ) noexcept
        {
            while (!bytes.empty())
            {
                if (bytes.size() < 16u)
                    return false;
                std::uint64_t payload_size{};
                std::memcpy(
                    &payload_size,
                    bytes.data() + sizeof(std::uint64_t),
                    sizeof(payload_size)
                );
                if (payload_size > bytes.size() - 16u)
                    return false;
                bytes = bytes.subspan(16u + static_cast<std::size_t>(payload_size));
            }
            return true;
        }

        template <class Header>
        [[nodiscard]] lux::cxx::expected<
            CookedAssetImageView,
            ECookedAssetImageError>
        inspectVersion(std::span<const std::byte> image) noexcept
        {
            if (image.size() < sizeof(Header))
                return lux::cxx::unexpected(ECookedAssetImageError::TRUNCATED);

            Header header{};
            std::memcpy(&header, image.data(), sizeof(header));
            if (header.info_offset != sizeof(Header) ||
                !validRange(header.info_offset, header.info_size, image.size()) ||
                !validRange(header.data_offset, header.data_size, image.size()) ||
                header.data_offset < header.info_offset + header.info_size)
            {
                return lux::cxx::unexpected(ECookedAssetImageError::INVALID_LAYOUT);
            }

            const auto auxiliary_offset = header.data_offset + header.data_size;
            const auto auxiliary = image.subspan(static_cast<std::size_t>(auxiliary_offset));
            if (!validAuxiliaryPayloads(auxiliary))
                return lux::cxx::unexpected(ECookedAssetImageError::INVALID_LAYOUT);

            CookedAssetMetadata metadata{
                AssetId{header.metadata.id},
                header.metadata.type,
                header.metadata.date,
            };
            if constexpr (std::is_same_v<Header, WireHeaderV2>)
            {
                metadata.display_name = header.metadata.display_name;
                metadata.source_path = header.metadata.source_path;
                metadata.source_mtime = header.metadata.source_mtime;
            }

            return CookedAssetImageView{
                header.magic,
                header.version,
                metadata,
                image.subspan(
                    static_cast<std::size_t>(header.info_offset),
                    static_cast<std::size_t>(header.info_size)
                ),
                image.subspan(
                    static_cast<std::size_t>(header.data_offset),
                    static_cast<std::size_t>(header.data_size)
                ),
                auxiliary,
            };
        }
    } // namespace

    lux::cxx::expected<CookedAssetImageView, ECookedAssetImageError>
    inspectCookedAssetImage(
        std::span<const std::byte> image,
        const CookedAssetImageLimits& limits
    ) noexcept
    {
        if (image.size() > limits.max_image_bytes)
            return lux::cxx::unexpected(ECookedAssetImageError::LIMIT_EXCEEDED);
        if (image.size() < sizeof(std::uint32_t) * 2u)
            return lux::cxx::unexpected(ECookedAssetImageError::TRUNCATED);

        std::uint32_t version{};
        std::memcpy(
            &version,
            image.data() + sizeof(std::uint32_t),
            sizeof(version)
        );
        if (version == kCookedAssetVersionV1)
            return inspectVersion<WireHeaderV1>(image);
        if (version == kCookedAssetVersionV2)
            return inspectVersion<WireHeaderV2>(image);
        return lux::cxx::unexpected(ECookedAssetImageError::UNSUPPORTED_VERSION);
    }
} // namespace lux::asset
