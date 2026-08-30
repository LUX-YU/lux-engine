#include <lux/engine/resource/asset/detail/CookedAssetWriter.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <new>
#include <type_traits>

namespace lux::asset::detail
{
    namespace
    {
        struct WireAssetInfoV2 final
        {
            std::array<std::uint8_t, 16U> id{};
            std::uint32_t type{};
            std::uint64_t date{};
            std::array<char, 64U> display_name{};
            std::array<char, 256U> source_path{};
            std::uint64_t source_mtime{};
        };

        struct WireHeaderV2 final
        {
            std::uint32_t magic{};
            std::uint32_t version{};
            std::uint64_t info_offset{};
            std::uint64_t info_size{};
            std::uint64_t data_offset{};
            std::uint64_t data_size{};
            WireAssetInfoV2 metadata;
        };

        struct AuxiliaryHeader final
        {
            std::uint64_t tag{};
            std::uint64_t size{};
        };

        static_assert(sizeof(WireAssetInfoV2) == 360U);
        static_assert(sizeof(WireHeaderV2) == 400U);
        static_assert(sizeof(AuxiliaryHeader) == 16U);
        static_assert(std::is_trivially_copyable_v<WireHeaderV2>);

        [[nodiscard]] AssetEncodeFailure failure(
            EAssetEncodeError code,
            std::size_t offset = 0U
        ) noexcept
        {
            return AssetEncodeFailure{code, offset};
        }

        [[nodiscard]] bool checkedAdd(std::size_t value, std::size_t& total) noexcept
        {
            if (value > (std::numeric_limits<std::size_t>::max)() - total)
                return false;
            total += value;
            return true;
        }
    } // namespace

    lux::cxx::expected<std::vector<std::byte>, AssetEncodeFailure> encodeCookedAssetImage(
        const CookedAssetWriteRequest& request,
        const AssetEncodeLimits& limits
    ) noexcept
    {
        const bool invalid_metadata = request.metadata.id.isNull() || !request.metadata.type;
        const bool invalid_payload = request.primary_magic == 0U || request.data.empty();
        if (invalid_metadata || invalid_payload)
            return lux::cxx::unexpected(failure(EAssetEncodeError::INVALID_ASSET));

        try
        {
            std::vector<AssetAuxiliaryPayload> auxiliary(
                request.auxiliary.begin(),
                request.auxiliary.end()
            );
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
                    return lux::cxx::unexpected(failure(EAssetEncodeError::INVALID_PAYLOAD, index));
            }

            std::size_t encoded_size = sizeof(WireHeaderV2);
            if (!checkedAdd(request.information.size(), encoded_size) ||
                !checkedAdd(request.data.size(), encoded_size))
            {
                return lux::cxx::unexpected(failure(EAssetEncodeError::LIMIT_EXCEEDED));
            }
            for (const auto& payload : auxiliary)
            {
                if (!checkedAdd(sizeof(AuxiliaryHeader), encoded_size) ||
                    !checkedAdd(payload.bytes.size(), encoded_size))
                {
                    return lux::cxx::unexpected(failure(EAssetEncodeError::LIMIT_EXCEEDED));
                }
            }
            if (encoded_size > limits.max_encoded_bytes)
                return lux::cxx::unexpected(failure(EAssetEncodeError::LIMIT_EXCEEDED));

            WireHeaderV2 header;
            std::memset(&header, 0, sizeof(header));
            header.magic = request.primary_magic;
            header.version = kCookedAssetVersionV2;
            header.info_offset = sizeof(WireHeaderV2);
            header.info_size = request.information.size();
            header.data_offset = header.info_offset + header.info_size;
            header.data_size = request.data.size();
            const auto id_bytes = request.metadata.id.bytes();
            for (std::size_t index = 0U; index < id_bytes.size(); ++index)
                header.metadata.id[index] = std::to_integer<std::uint8_t>(id_bytes[index]);
            header.metadata.type = request.legacy_type_tag;
            header.metadata.date = request.metadata.date;
            header.metadata.display_name = request.metadata.display_name;
            header.metadata.source_path = request.metadata.source_path;
            header.metadata.source_mtime = request.metadata.source_mtime;

            std::vector<std::byte> result(encoded_size);
            std::size_t offset{};
            std::memcpy(result.data() + offset, &header, sizeof(header));
            offset += sizeof(header);
            std::memcpy(result.data() + offset, request.information.data(), request.information.size());
            offset += request.information.size();
            std::memcpy(result.data() + offset, request.data.data(), request.data.size());
            offset += request.data.size();
            for (const auto& payload : auxiliary)
            {
                const AuxiliaryHeader auxiliary_header{payload.tag, payload.bytes.size()};
                std::memcpy(result.data() + offset, &auxiliary_header, sizeof(auxiliary_header));
                offset += sizeof(auxiliary_header);
                std::memcpy(result.data() + offset, payload.bytes.data(), payload.bytes.size());
                offset += payload.bytes.size();
            }
            return result;
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(failure(EAssetEncodeError::ALLOCATION_FAILURE));
        }
        catch (...)
        {
            return lux::cxx::unexpected(failure(EAssetEncodeError::INVALID_PAYLOAD));
        }
    }
} // namespace lux::asset::detail
