#include <lux/engine/ecs/WorldSectionContract.hpp>
#include <lux/engine/ecs/WorldSectionImage.hpp>

#include <lux/cxx/core/StableNameId.hpp>

#include <algorithm>
#include <array>
#include <limits>
#include <new>
#include <string_view>
#include <type_traits>
#include <utility>

namespace lux::ecs
{
    struct WorldSectionImage::Impl final
    {
        WorldSectionId id;
        std::uint32_t entity_count{};
        std::vector<std::byte> bytes;
        std::vector<WorldSectionColumnView> columns;
    };

    namespace
    {
        constexpr std::array kMagic{
            std::byte{'L'}, std::byte{'X'}, std::byte{'W'}, std::byte{'C'}};

        struct Region final
        {
            std::uint64_t begin{};
            std::uint64_t end{};
        };

        [[nodiscard]] WorldSectionFailure failure(
            EWorldSectionError code,
            std::uint64_t offset = 0U,
            std::uint32_t column = 0U
        ) noexcept
        {
            WorldSectionFailure result;
            result.code = code;
            result.byte_offset = offset;
            result.column_index = column;
            return result;
        }

        template <class Integer>
        [[nodiscard]] bool readLittle(
            std::span<const std::byte> bytes,
            std::uint64_t offset,
            Integer& result
        ) noexcept
        {
            static_assert(std::is_unsigned_v<Integer>);
            if (offset > bytes.size() ||
                sizeof(Integer) > bytes.size() - static_cast<std::size_t>(offset))
            {
                return false;
            }
            result = 0U;
            for (std::size_t index{}; index < sizeof(Integer); ++index)
            {
                result |= static_cast<Integer>(
                    std::to_integer<std::uint8_t>(
                        bytes[static_cast<std::size_t>(offset) + index]
                    )
                ) << (index * 8U);
            }
            return true;
        }

        [[nodiscard]] bool validRange(
            std::uint64_t offset,
            std::uint64_t size,
            std::uint64_t total
        ) noexcept
        {
            return offset <= total && size <= total - offset;
        }

        [[nodiscard]] bool multiply(
            std::uint64_t left,
            std::uint64_t right,
            std::uint64_t& result
        ) noexcept
        {
            if (left != 0U &&
                right > std::numeric_limits<std::uint64_t>::max() / left)
            {
                return false;
            }
            result = left * right;
            return true;
        }

        [[nodiscard]] bool addRegion(
            std::vector<Region>& regions,
            std::uint64_t offset,
            std::uint64_t size,
            std::uint64_t total
        )
        {
            if (!validRange(offset, size, total))
                return false;
            if (size != 0U)
                regions.push_back({offset, offset + size});
            return true;
        }
    } // namespace

    std::uint64_t WorldSectionColumnView::schemaHash() const noexcept
    {
        return schema_hash_;
    }

    std::string_view WorldSectionColumnView::schemaName() const noexcept
    {
        return schema_name_;
    }

    std::uint32_t WorldSectionColumnView::schemaVersion() const noexcept
    {
        return schema_version_;
    }

    EWorldSectionValueEncoding
    WorldSectionColumnView::valueEncoding() const noexcept
    {
        return value_encoding_;
    }

    EWorldSectionOrdinalEncoding
    WorldSectionColumnView::ordinalEncoding() const noexcept
    {
        return ordinal_encoding_;
    }

    std::uint32_t WorldSectionColumnView::rowCount() const noexcept
    {
        return row_count_;
    }

    std::uint32_t WorldSectionColumnView::fixedStride() const noexcept
    {
        return fixed_stride_;
    }

    std::span<const std::byte>
    WorldSectionColumnView::ordinalBytes() const noexcept
    {
        return ordinal_bytes_;
    }

    std::span<const std::byte>
    WorldSectionColumnView::offsetBytes() const noexcept
    {
        return offset_bytes_;
    }

    std::span<const std::byte> WorldSectionColumnView::payload() const noexcept
    {
        return payload_;
    }

    WorldSectionImage::WorldSectionImage(
        std::shared_ptr<const Impl> impl
    ) noexcept
        : impl_(std::move(impl))
    {
    }

    WorldSectionImage::~WorldSectionImage() = default;

    lux::cxx::expected<WorldSectionImage, WorldSectionFailure>
    WorldSectionImage::open(
        std::vector<std::byte> bytes,
        const WorldSectionValidationBudget& budget
    ) noexcept
    {
        try
        {
            const auto source = std::span<const std::byte>(bytes);
            if (source.size() > budget.max_image_bytes)
            {
                return lux::cxx::unexpected(
                    failure(EWorldSectionError::LIMIT_EXCEEDED)
                );
            }
            if (source.size() < WorldSectionHeaderBytes)
            {
                return lux::cxx::unexpected(
                    failure(EWorldSectionError::TRUNCATED, source.size())
                );
            }
            if (!std::equal(kMagic.begin(), kMagic.end(), source.begin()))
            {
                return lux::cxx::unexpected(
                    failure(EWorldSectionError::INVALID_MAGIC)
                );
            }

            std::uint32_t format_version{};
            std::uint32_t loader_contract{};
            std::uint32_t header_bytes{};
            std::uint32_t entity_count{};
            std::uint32_t column_count{};
            std::uint64_t name_offset{};
            std::uint64_t name_bytes{};
            std::uint64_t column_offset{};
            std::uint64_t column_bytes{};
            std::uint64_t file_bytes{};
            if (!readLittle(source, 4U, format_version) ||
                !readLittle(source, 8U, loader_contract) ||
                !readLittle(source, 12U, header_bytes) ||
                !readLittle(source, 32U, entity_count) ||
                !readLittle(source, 36U, column_count) ||
                !readLittle(source, 40U, name_offset) ||
                !readLittle(source, 48U, name_bytes) ||
                !readLittle(source, 56U, column_offset) ||
                !readLittle(source, 64U, column_bytes) ||
                !readLittle(source, 72U, file_bytes))
            {
                return lux::cxx::unexpected(
                    failure(EWorldSectionError::TRUNCATED)
                );
            }
            if (format_version != worldSectionFormatVersion())
            {
                return lux::cxx::unexpected(
                    failure(EWorldSectionError::UNSUPPORTED_FORMAT_VERSION, 4U)
                );
            }
            if (loader_contract != worldSectionLoaderContractVersion())
            {
                return lux::cxx::unexpected(failure(
                    EWorldSectionError::UNSUPPORTED_LOADER_CONTRACT,
                    8U
                ));
            }
            if (header_bytes != WorldSectionHeaderBytes ||
                file_bytes != source.size())
            {
                return lux::cxx::unexpected(
                    failure(EWorldSectionError::INVALID_HEADER, 12U)
                );
            }
            if (entity_count > budget.max_entities ||
                column_count > budget.max_columns)
            {
                return lux::cxx::unexpected(
                    failure(EWorldSectionError::LIMIT_EXCEEDED)
                );
            }

            std::uint64_t expected_column_bytes{};
            if (!multiply(
                    column_count,
                    WorldSectionColumnDescriptorBytes,
                    expected_column_bytes) ||
                column_bytes != expected_column_bytes)
            {
                return lux::cxx::unexpected(
                    failure(EWorldSectionError::INVALID_HEADER, 64U)
                );
            }

            std::array<std::uint8_t, 16> uuid_bytes{};
            for (std::size_t index{}; index < uuid_bytes.size(); ++index)
            {
                uuid_bytes[index] = std::to_integer<std::uint8_t>(
                    source[16U + index]
                );
            }
            auto result = std::make_shared<Impl>();
            result->id.value = uuids::uuid(uuid_bytes);
            if (result->id.value.is_nil())
            {
                return lux::cxx::unexpected(
                    failure(EWorldSectionError::INVALID_SECTION_ID, 16U)
                );
            }
            result->entity_count = entity_count;
            result->bytes = std::move(bytes);
            const auto owned = std::span<const std::byte>(result->bytes);

            std::vector<Region> regions;
            regions.reserve(3U + static_cast<std::size_t>(column_count) * 3U);
            regions.push_back({0U, WorldSectionHeaderBytes});
            if (!addRegion(regions, name_offset, name_bytes, file_bytes) ||
                !addRegion(regions, column_offset, column_bytes, file_bytes))
            {
                return lux::cxx::unexpected(
                    failure(EWorldSectionError::TRUNCATED)
                );
            }

            result->columns.reserve(column_count);
            std::uint64_t total_component_rows{};
            std::uint64_t previous_hash{};
            std::string_view previous_name;
            bool has_previous{};

            for (std::uint32_t index{}; index < column_count; ++index)
            {
                const std::uint64_t descriptor =
                    column_offset +
                    static_cast<std::uint64_t>(index) *
                        WorldSectionColumnDescriptorBytes;
                std::uint64_t schema_hash{};
                std::uint32_t schema_version{};
                std::uint16_t reserved{};
                std::uint32_t schema_name_offset{};
                std::uint32_t schema_name_size{};
                std::uint32_t row_count{};
                std::uint32_t fixed_stride{};
                std::uint64_t ordinals_offset{};
                std::uint64_t ordinals_bytes{};
                std::uint64_t offsets_offset{};
                std::uint64_t offsets_bytes{};
                std::uint64_t payload_offset{};
                std::uint64_t payload_bytes{};
                if (!readLittle(owned, descriptor, schema_hash) ||
                    !readLittle(owned, descriptor + 8U, schema_version) ||
                    !readLittle(owned, descriptor + 14U, reserved) ||
                    !readLittle(owned, descriptor + 16U, schema_name_offset) ||
                    !readLittle(owned, descriptor + 20U, schema_name_size) ||
                    !readLittle(owned, descriptor + 24U, row_count) ||
                    !readLittle(owned, descriptor + 28U, fixed_stride) ||
                    !readLittle(owned, descriptor + 32U, ordinals_offset) ||
                    !readLittle(owned, descriptor + 40U, ordinals_bytes) ||
                    !readLittle(owned, descriptor + 48U, offsets_offset) ||
                    !readLittle(owned, descriptor + 56U, offsets_bytes) ||
                    !readLittle(owned, descriptor + 64U, payload_offset) ||
                    !readLittle(owned, descriptor + 72U, payload_bytes))
                {
                    return lux::cxx::unexpected(
                        failure(EWorldSectionError::TRUNCATED, descriptor, index)
                    );
                }
                const auto value_raw = std::to_integer<std::uint8_t>(
                    owned[static_cast<std::size_t>(descriptor + 12U)]
                );
                const auto ordinal_raw = std::to_integer<std::uint8_t>(
                    owned[static_cast<std::size_t>(descriptor + 13U)]
                );
                if (reserved != 0U || schema_version == 0U ||
                    value_raw > static_cast<std::uint8_t>(
                        EWorldSectionValueEncoding::VARIABLE) ||
                    ordinal_raw > static_cast<std::uint8_t>(
                        EWorldSectionOrdinalEncoding::U32_LIST))
                {
                    return lux::cxx::unexpected(
                        failure(EWorldSectionError::INVALID_ENCODING, descriptor, index)
                    );
                }
                if (schema_name_size == 0U ||
                    schema_name_offset > name_bytes ||
                    schema_name_size > name_bytes - schema_name_offset)
                {
                    return lux::cxx::unexpected(
                        failure(EWorldSectionError::INVALID_SCHEMA, descriptor, index)
                    );
                }
                const auto* schema_data = reinterpret_cast<const char*>(
                    owned.data() + static_cast<std::size_t>(
                        name_offset + schema_name_offset
                    )
                );
                const std::string_view schema_name(
                    schema_data,
                    schema_name_size
                );
                if (lux::cxx::Fnv1a64::hash(schema_name) != schema_hash ||
                    (has_previous &&
                     (schema_hash < previous_hash ||
                      (schema_hash == previous_hash &&
                       schema_name <= previous_name))))
                {
                    return lux::cxx::unexpected(
                        failure(EWorldSectionError::INVALID_SCHEMA, descriptor, index)
                    );
                }
                if (has_previous && schema_hash == previous_hash &&
                    schema_name != previous_name)
                {
                    return lux::cxx::unexpected(
                        failure(EWorldSectionError::INVALID_SCHEMA, descriptor, index)
                    );
                }
                has_previous = true;
                previous_hash = schema_hash;
                previous_name = schema_name;

                const auto value_encoding =
                    static_cast<EWorldSectionValueEncoding>(value_raw);
                const auto ordinal_encoding =
                    static_cast<EWorldSectionOrdinalEncoding>(ordinal_raw);
                if (row_count >
                    std::numeric_limits<std::uint64_t>::max() -
                        total_component_rows)
                {
                    return lux::cxx::unexpected(
                        failure(EWorldSectionError::LIMIT_EXCEEDED)
                    );
                }
                total_component_rows += row_count;
                if (total_component_rows > budget.max_component_rows)
                {
                    return lux::cxx::unexpected(
                        failure(EWorldSectionError::LIMIT_EXCEEDED)
                    );
                }
                std::uint64_t expected_ordinal_bytes{};
                if (ordinal_encoding == EWorldSectionOrdinalEncoding::DENSE)
                {
                    if (row_count != entity_count || ordinals_bytes != 0U)
                    {
                        return lux::cxx::unexpected(failure(
                            EWorldSectionError::INVALID_ORDINAL,
                            descriptor,
                            index
                        ));
                    }
                }
                else if (!multiply(row_count, 4U, expected_ordinal_bytes) ||
                         ordinals_bytes != expected_ordinal_bytes)
                {
                    return lux::cxx::unexpected(failure(
                        EWorldSectionError::INVALID_ORDINAL,
                        descriptor,
                        index
                    ));
                }
                if (!addRegion(
                        regions,
                        ordinals_offset,
                        ordinals_bytes,
                        file_bytes))
                {
                    return lux::cxx::unexpected(
                        failure(EWorldSectionError::TRUNCATED, ordinals_offset, index)
                    );
                }
                std::uint32_t previous_ordinal{};
                for (std::uint32_t row{};
                     ordinal_encoding == EWorldSectionOrdinalEncoding::U32_LIST &&
                     row < row_count;
                     ++row)
                {
                    std::uint32_t ordinal{};
                    (void)readLittle(
                        owned,
                        ordinals_offset + static_cast<std::uint64_t>(row) * 4U,
                        ordinal
                    );
                    if (ordinal >= entity_count ||
                        (row != 0U && ordinal <= previous_ordinal))
                    {
                        return lux::cxx::unexpected(failure(
                            EWorldSectionError::INVALID_ORDINAL,
                            ordinals_offset + static_cast<std::uint64_t>(row) * 4U,
                            index
                        ));
                    }
                    previous_ordinal = ordinal;
                }

                if (value_encoding == EWorldSectionValueEncoding::TAG)
                {
                    if (fixed_stride != 0U || offsets_bytes != 0U ||
                        payload_bytes != 0U)
                    {
                        return lux::cxx::unexpected(failure(
                            EWorldSectionError::INVALID_PAYLOAD,
                            descriptor,
                            index
                        ));
                    }
                }
                else if (value_encoding == EWorldSectionValueEncoding::FIXED)
                {
                    std::uint64_t expected_payload{};
                    if (fixed_stride == 0U || offsets_bytes != 0U ||
                        !multiply(row_count, fixed_stride, expected_payload) ||
                        payload_bytes != expected_payload)
                    {
                        return lux::cxx::unexpected(failure(
                            EWorldSectionError::INVALID_PAYLOAD,
                            descriptor,
                            index
                        ));
                    }
                }
                else
                {
                    std::uint64_t expected_offsets{};
                    if (fixed_stride != 0U ||
                        !multiply(
                            static_cast<std::uint64_t>(row_count) + 1U,
                            4U,
                            expected_offsets) ||
                        offsets_bytes != expected_offsets ||
                        payload_bytes >= (1ULL << 32U))
                    {
                        return lux::cxx::unexpected(failure(
                            EWorldSectionError::INVALID_OFFSETS,
                            descriptor,
                            index
                        ));
                    }
                }
                if (!addRegion(
                        regions,
                        offsets_offset,
                        offsets_bytes,
                        file_bytes) ||
                    !addRegion(
                        regions,
                        payload_offset,
                        payload_bytes,
                        file_bytes))
                {
                    return lux::cxx::unexpected(
                        failure(EWorldSectionError::TRUNCATED, descriptor, index)
                    );
                }
                if (value_encoding == EWorldSectionValueEncoding::VARIABLE)
                {
                    std::uint32_t previous_offset{};
                    for (std::uint32_t row{}; row <= row_count; ++row)
                    {
                        std::uint32_t current{};
                        (void)readLittle(
                            owned,
                            offsets_offset + static_cast<std::uint64_t>(row) * 4U,
                            current
                        );
                        if ((row == 0U && current != 0U) ||
                            (row != 0U && current < previous_offset) ||
                            current > payload_bytes ||
                            (row == row_count && current != payload_bytes))
                        {
                            return lux::cxx::unexpected(failure(
                                EWorldSectionError::INVALID_OFFSETS,
                                offsets_offset + static_cast<std::uint64_t>(row) * 4U,
                                index
                            ));
                        }
                        previous_offset = current;
                    }
                }

                WorldSectionColumnView view;
                view.schema_hash_ = schema_hash;
                view.schema_name_ = schema_name;
                view.schema_version_ = schema_version;
                view.value_encoding_ = value_encoding;
                view.ordinal_encoding_ = ordinal_encoding;
                view.row_count_ = row_count;
                view.fixed_stride_ = fixed_stride;
                view.ordinal_bytes_ = owned.subspan(
                    static_cast<std::size_t>(ordinals_offset),
                    static_cast<std::size_t>(ordinals_bytes)
                );
                view.offset_bytes_ = owned.subspan(
                    static_cast<std::size_t>(offsets_offset),
                    static_cast<std::size_t>(offsets_bytes)
                );
                view.payload_ = owned.subspan(
                    static_cast<std::size_t>(payload_offset),
                    static_cast<std::size_t>(payload_bytes)
                );
                result->columns.push_back(view);
            }

            std::sort(
                regions.begin(),
                regions.end(),
                [](const Region& left, const Region& right)
                {
                    return left.begin < right.begin ||
                        (left.begin == right.begin && left.end < right.end);
                }
            );
            for (std::size_t index = 1U; index < regions.size(); ++index)
            {
                if (regions[index].begin < regions[index - 1U].end)
                {
                    return lux::cxx::unexpected(failure(
                        EWorldSectionError::OVERLAPPING_REGION,
                        regions[index].begin
                    ));
                }
            }
            return WorldSectionImage(std::move(result));
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(
                failure(EWorldSectionError::ALLOCATION_FAILURE)
            );
        }
    }

    const WorldSectionId& WorldSectionImage::id() const noexcept
    {
        static const WorldSectionId empty;
        return impl_ ? impl_->id : empty;
    }

    std::uint32_t WorldSectionImage::entityCount() const noexcept
    {
        return impl_ ? impl_->entity_count : 0U;
    }

    std::span<const WorldSectionColumnView>
    WorldSectionImage::columns() const noexcept
    {
        return impl_
            ? std::span<const WorldSectionColumnView>(impl_->columns)
            : std::span<const WorldSectionColumnView>{};
    }

    std::span<const std::byte> WorldSectionImage::bytes() const noexcept
    {
        return impl_
            ? std::span<const std::byte>(impl_->bytes)
            : std::span<const std::byte>{};
    }
} // namespace lux::ecs
