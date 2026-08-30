#include <lux/engine/resource/asset/CookedAssetImage.hpp>

#include <algorithm>
#include <cstring>
#include <new>
#include <type_traits>

namespace lux::asset
{
    namespace detail
    {
        struct CookedAssetImageAccess final
        {
            static void setHeader(
                CookedAssetImage& target,
                std::uint32_t magic,
                std::uint32_t version,
                CookedAssetMetadata metadata,
                lux::cxx::SharedBytes<> image,
                lux::cxx::SharedBytes<> information,
                lux::cxx::SharedBytes<> data
            ) noexcept
            {
                target.magic_ = magic;
                target.version_ = version;
                target.metadata_ = std::move(metadata);
                target.image_ = std::move(image);
                target.information_ = std::move(information);
                target.data_ = std::move(data);
            }

            static std::vector<AssetAuxiliaryPayload>& auxiliary(CookedAssetImage& target) noexcept
            {
                return target.auxiliary_;
            }
        };
    } // namespace detail

    namespace
    {
        struct WireAssetInfoV1 final
        {
            std::array<std::uint8_t, 16U> id{};
            std::uint32_t type{};
            std::uint64_t date{};
        };

        struct WireAssetInfoV2 final
        {
            std::array<std::uint8_t, 16U> id{};
            std::uint32_t type{};
            std::uint64_t date{};
            std::array<char, 64U> display_name{};
            std::array<char, 256U> source_path{};
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

        struct AuxiliaryHeader final
        {
            std::uint64_t tag{};
            std::uint64_t size{};
        };

        using WireHeaderV1 = WireHeader<WireAssetInfoV1>;
        using WireHeaderV2 = WireHeader<WireAssetInfoV2>;

        static_assert(sizeof(WireAssetInfoV1) == 32U);
        static_assert(sizeof(WireAssetInfoV2) == 360U);
        static_assert(sizeof(WireHeaderV1) == 72U);
        static_assert(sizeof(WireHeaderV2) == 400U);
        static_assert(sizeof(AuxiliaryHeader) == 16U);
        static_assert(std::is_trivially_copyable_v<WireHeaderV1>);
        static_assert(std::is_trivially_copyable_v<WireHeaderV2>);

        [[nodiscard]] AssetDecodeFailure failure(EAssetDecodeError code, std::size_t offset = 0U) noexcept
        {
            return AssetDecodeFailure{code, offset};
        }

        [[nodiscard]] bool validRange(std::uint64_t offset, std::uint64_t size, std::size_t total) noexcept
        {
            return offset <= total && size <= total - offset;
        }
    } // namespace

    std::uint32_t CookedAssetImage::magic() const noexcept
    {
        return magic_;
    }

    std::uint32_t CookedAssetImage::version() const noexcept
    {
        return version_;
    }

    const CookedAssetMetadata& CookedAssetImage::metadata() const noexcept
    {
        return metadata_;
    }

    const lux::cxx::SharedBytes<>& CookedAssetImage::image() const noexcept
    {
        return image_;
    }

    const lux::cxx::SharedBytes<>& CookedAssetImage::information() const noexcept
    {
        return information_;
    }

    const lux::cxx::SharedBytes<>& CookedAssetImage::data() const noexcept
    {
        return data_;
    }

    std::span<const AssetAuxiliaryPayload> CookedAssetImage::auxiliaryPayloads() const noexcept
    {
        return auxiliary_;
    }

    lux::cxx::expected<CookedAssetImage, AssetDecodeFailure> inspectCookedAssetImage(
        lux::cxx::SharedBytes<> image,
        const AssetDecodeLimits& limits
    ) noexcept
    {
        if (image.size() > limits.max_image_bytes)
            return lux::cxx::unexpected(failure(EAssetDecodeError::LIMIT_EXCEEDED));
        if (image.size() < sizeof(std::uint32_t) * 2U)
            return lux::cxx::unexpected(failure(EAssetDecodeError::TRUNCATED, image.size()));

        std::uint32_t version{};
        std::memcpy(&version, image.data() + sizeof(std::uint32_t), sizeof(version));

        try
        {
            const auto inspect = [&]<class Header>()
                -> lux::cxx::expected<CookedAssetImage, AssetDecodeFailure> {
                const auto bytes = image.view();
                if (bytes.size() < sizeof(Header))
                    return lux::cxx::unexpected(failure(EAssetDecodeError::TRUNCATED, bytes.size()));

                Header header{};
                std::memcpy(&header, bytes.data(), sizeof(header));
                const bool invalid_info = header.info_offset != sizeof(Header) ||
                    !validRange(header.info_offset, header.info_size, bytes.size());
                const bool invalid_data = !validRange(header.data_offset, header.data_size, bytes.size()) ||
                    header.data_offset != header.info_offset + header.info_size;
                if (invalid_info || invalid_data)
                    return lux::cxx::unexpected(failure(EAssetDecodeError::INVALID_LAYOUT));

                CookedAssetMetadata metadata{
                    AssetId{header.metadata.id},
                    header.metadata.type,
                    header.metadata.date
                };
                if constexpr (std::is_same_v<Header, WireHeaderV2>)
                {
                    metadata.display_name = header.metadata.display_name;
                    metadata.source_path = header.metadata.source_path;
                    metadata.source_mtime = header.metadata.source_mtime;
                }
                if (metadata.id.isNull())
                    return lux::cxx::unexpected(failure(EAssetDecodeError::INVALID_ASSET_ID));
                std::size_t decoded_bytes{};
                const auto account = [&](std::uint64_t size) noexcept {
                    if (size > limits.max_decoded_bytes - decoded_bytes)
                        return false;
                    decoded_bytes += static_cast<std::size_t>(size);
                    return true;
                };
                if (!account(header.info_size) || !account(header.data_size))
                    return lux::cxx::unexpected(failure(EAssetDecodeError::LIMIT_EXCEEDED));

                CookedAssetImage result;
                detail::CookedAssetImageAccess::setHeader(
                    result,
                    header.magic,
                    header.version,
                    metadata,
                    image,
                    image.subspan(
                        static_cast<std::size_t>(header.info_offset),
                        static_cast<std::size_t>(header.info_size)
                    ),
                    image.subspan(
                        static_cast<std::size_t>(header.data_offset),
                        static_cast<std::size_t>(header.data_size)
                    )
                );
                auto& result_auxiliary = detail::CookedAssetImageAccess::auxiliary(result);

                std::size_t offset = static_cast<std::size_t>(header.data_offset + header.data_size);
                while (offset < bytes.size())
                {
                    if (result_auxiliary.size() >= limits.max_auxiliary_payloads)
                        return lux::cxx::unexpected(failure(EAssetDecodeError::LIMIT_EXCEEDED, offset));
                    if (bytes.size() - offset < sizeof(AuxiliaryHeader))
                        return lux::cxx::unexpected(failure(EAssetDecodeError::TRUNCATED, offset));

                    AuxiliaryHeader auxiliary{};
                    std::memcpy(&auxiliary, bytes.data() + offset, sizeof(auxiliary));
                    offset += sizeof(auxiliary);
                    if (auxiliary.tag == 0U || auxiliary.size > bytes.size() - offset)
                        return lux::cxx::unexpected(failure(EAssetDecodeError::INVALID_LAYOUT, offset));
                    if (!account(auxiliary.size))
                        return lux::cxx::unexpected(failure(EAssetDecodeError::LIMIT_EXCEEDED, offset));
                    const auto duplicate = std::find_if(
                        result_auxiliary.begin(),
                        result_auxiliary.end(),
                        [tag = auxiliary.tag](const AssetAuxiliaryPayload& value) noexcept {
                            return value.tag == tag;
                        }
                    );
                    if (duplicate != result_auxiliary.end())
                        return lux::cxx::unexpected(failure(EAssetDecodeError::INVALID_LAYOUT, offset));
                    result_auxiliary.push_back({
                        auxiliary.tag,
                        image.subspan(offset, static_cast<std::size_t>(auxiliary.size))
                    });
                    offset += static_cast<std::size_t>(auxiliary.size);
                }
                return result;
            };

            if (version == kCookedAssetVersionV1)
                return inspect.template operator()<WireHeaderV1>();
            if (version == kCookedAssetVersionV2)
                return inspect.template operator()<WireHeaderV2>();
            return lux::cxx::unexpected(
                failure(EAssetDecodeError::UNSUPPORTED_VERSION, sizeof(std::uint32_t))
            );
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(failure(EAssetDecodeError::ALLOCATION_FAILURE));
        }
        catch (...)
        {
            return lux::cxx::unexpected(failure(EAssetDecodeError::INVALID_LAYOUT));
        }
    }

    lux::cxx::expected<CookedAssetImage, AssetDecodeFailure> inspectCookedAssetImage(
        AssetId requested,
        lux::cxx::SharedBytes<> image,
        const AssetDecodeLimits& limits
    ) noexcept
    {
        if (requested.isNull())
            return lux::cxx::unexpected(failure(EAssetDecodeError::INVALID_ASSET_ID));
        auto inspected = inspectCookedAssetImage(std::move(image), limits);
        if (!inspected)
            return lux::cxx::unexpected(inspected.error());
        if (inspected->metadata().id != requested)
            return lux::cxx::unexpected(failure(EAssetDecodeError::ASSET_ID_MISMATCH));
        return inspected;
    }
} // namespace lux::asset
