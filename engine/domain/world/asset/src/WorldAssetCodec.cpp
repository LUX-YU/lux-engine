#include <lux/engine/world/WorldAssetCodec.hpp>

#include <lux/engine/resource/asset/CookedAssetImage.hpp>
#include <lux/engine/resource/asset/detail/CookedAssetWriter.hpp>
#include <lux/engine/serialization/BinaryReader.hpp>
#include <lux/engine/serialization/BinaryWriter.hpp>
#include <lux/engine/world/WorldDescriptionBuilder.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace lux::world
{
    namespace detail
    {
        inline constexpr std::uint32_t kWorldRootFormatVersion = 2U;

        [[nodiscard]] lux::cxx::expected<std::vector<std::byte>, lux::asset::EAssetCodecError>
        codecFailure() noexcept
        {
            return lux::cxx::unexpected(lux::asset::EAssetCodecError::CODEC_FAILURE);
        }

        [[nodiscard]] bool writeBytes(
            lux::serialization::BinaryWriter& writer,
            std::span<const std::byte> bytes
        ) noexcept
        {
            return static_cast<bool>(writer.writeBytes(bytes));
        }

        template <class Type>
        [[nodiscard]] bool writeUnsigned(lux::serialization::BinaryWriter& writer, Type value) noexcept
        {
            return static_cast<bool>(writer.writeUnsigned(value));
        }

        [[nodiscard]] bool writeUuid(
            lux::serialization::BinaryWriter& writer,
            const uuids::uuid& value
        ) noexcept
        {
            return writeBytes(writer, value.as_bytes());
        }

        [[nodiscard]] bool writeString(
            lux::serialization::BinaryWriter& writer,
            std::string_view value
        ) noexcept
        {
            if (value.size() > std::numeric_limits<std::uint32_t>::max())
                return false;
            return writeUnsigned(writer, static_cast<std::uint32_t>(value.size())) &&
                   writeBytes(writer, std::as_bytes(std::span(value.data(), value.size())));
        }

        [[nodiscard]] bool readUuid(lux::serialization::BinaryReader& reader, uuids::uuid& value) noexcept
        {
            std::array<std::uint8_t, 16U> bytes{};
            if (!reader.readBytes(std::as_writable_bytes(std::span(bytes))))
                return false;
            value = uuids::uuid(bytes);
            return true;
        }

        [[nodiscard]] bool readString(
            lux::serialization::BinaryReader& reader,
            std::size_t decoded_limit,
            std::string& value
        )
        {
            auto size = reader.readUnsigned<std::uint32_t>();
            if (!size || *size > reader.remaining() || *size > decoded_limit)
                return false;
            value.resize(*size);
            return static_cast<bool>(
                reader.readBytes(std::as_writable_bytes(std::span(value.data(), value.size())))
            );
        }

        [[nodiscard]] bool countFits(
            std::uint32_t count,
            std::size_t minimum_wire_bytes,
            std::size_t remaining,
            std::size_t decoded_limit,
            std::size_t decoded_element_bytes
        ) noexcept
        {
            if (minimum_wire_bytes != 0U && count > remaining / minimum_wire_bytes)
                return false;
            return decoded_element_bytes == 0U || count <= decoded_limit / decoded_element_bytes;
        }

        [[nodiscard]] lux::cxx::expected<std::vector<std::byte>, lux::asset::EAssetCodecError> encodeWorld(
            const WorldDescription& world,
            std::size_t max_encoded_bytes
        ) noexcept
        {
            try
            {
                std::vector<std::byte> output;
                lux::serialization::BinaryWriter writer(output);

                const bool valid_counts =
                    world.schemas().size() <= std::numeric_limits<std::uint32_t>::max() &&
                    world.storageVolumes().size() <= std::numeric_limits<std::uint32_t>::max() &&
                    world.partitionTable().pages().size() <= std::numeric_limits<std::uint32_t>::max() &&
                    world.partitionIndexes().size() <= std::numeric_limits<std::uint32_t>::max();
                if (!valid_counts)
                    return codecFailure();

                if (!writeUnsigned(writer, WorldAssetPrimaryMagic) ||
                    !writeUnsigned(writer, kWorldRootFormatVersion) ||
                    !writeUuid(writer, world.bundleId().value) ||
                    !writeUuid(writer, world.generation().value) ||
                    !writeString(writer, world.name()) ||
                    !writeUnsigned(writer, static_cast<std::uint32_t>(world.schemas().size())))
                {
                    return codecFailure();
                }

                for (const auto& schema : world.schemas())
                {
                    if (!writeUnsigned(writer, schema.hash) || !writeString(writer, schema.name))
                        return codecFailure();
                }

                if (!writeUnsigned(writer, world.partitioner().id.hash) ||
                    !writeString(writer, world.partitioner().id.name) ||
                    !writeUnsigned(writer, world.partitioner().version) ||
                    !writeUnsigned(writer, world.partitionCount()) ||
                    !writeUnsigned(writer, static_cast<std::uint32_t>(world.storageVolumes().size())))
                {
                    return codecFailure();
                }

                for (const auto& volume : world.storageVolumes())
                {
                    if (!writeString(writer, volume.member_name) ||
                        !writeUnsigned(writer, volume.format_version) ||
                        !writeUnsigned(writer, volume.chunk_count) ||
                        !writeUnsigned(writer, volume.file_size))
                    {
                        return codecFailure();
                    }
                }

                if (!writeUnsigned(
                        writer,
                        static_cast<std::uint32_t>(world.partitionTable().pages().size())
                    ))
                {
                    return codecFailure();
                }
                for (const auto& page : world.partitionTable().pages())
                {
                    if (!writeUnsigned(writer, page.first.value) ||
                        !writeUnsigned(writer, page.count) ||
                        !writeUnsigned(writer, page.chunk.volume) ||
                        !writeUnsigned(writer, page.chunk.chunk))
                    {
                        return codecFailure();
                    }
                }

                if (!writeUnsigned(writer, static_cast<std::uint32_t>(world.partitionIndexes().size())))
                    return codecFailure();
                for (const auto& index : world.partitionIndexes())
                {
                    if (!writeUnsigned(writer, index.type.hash) ||
                        !writeString(writer, index.type.name) ||
                        !writeUnsigned(writer, index.version) ||
                        !writeUnsigned(writer, index.root.volume) ||
                        !writeUnsigned(writer, index.root.chunk))
                    {
                        return codecFailure();
                    }
                }

                if (output.size() > max_encoded_bytes)
                    return codecFailure();
                return output;
            }
            catch (const std::bad_alloc&)
            {
                return lux::cxx::unexpected(lux::asset::EAssetCodecError::OUT_OF_MEMORY);
            }
        }

        [[nodiscard]] lux::cxx::expected<std::shared_ptr<const WorldDescription>, lux::asset::EAssetCodecError>
        decodeWorld(
            std::span<const std::byte> input,
            std::size_t max_input_bytes,
            std::size_t max_decoded_bytes
        ) noexcept
        {
            if (input.size() > max_input_bytes)
                return lux::cxx::unexpected(lux::asset::EAssetCodecError::CODEC_FAILURE);

            try
            {
                lux::serialization::BinaryReader reader(input);
                auto magic = reader.readUnsigned<std::uint32_t>();
                auto version = reader.readUnsigned<std::uint32_t>();
                WorldBundleId bundle;
                WorldBundleGeneration generation;
                std::string name;
                if (!magic || !version || *magic != WorldAssetPrimaryMagic ||
                    *version != kWorldRootFormatVersion ||
                    !readUuid(reader, bundle.value) ||
                    !readUuid(reader, generation.value) ||
                    !readString(reader, max_decoded_bytes, name))
                {
                    return lux::cxx::unexpected(lux::asset::EAssetCodecError::CODEC_FAILURE);
                }

                WorldDescriptionBuilder builder;
                if (!builder.setIdentity(bundle, generation, name))
                    return lux::cxx::unexpected(lux::asset::EAssetCodecError::CODEC_FAILURE);

                auto schema_count = reader.readUnsigned<std::uint32_t>();
                if (!schema_count ||
                    !countFits(
                        *schema_count,
                        sizeof(std::uint64_t) + sizeof(std::uint32_t),
                        reader.remaining(),
                        max_decoded_bytes,
                        sizeof(WorldDataSchemaId)
                    ))
                {
                    return lux::cxx::unexpected(lux::asset::EAssetCodecError::CODEC_FAILURE);
                }
                WorldDataSchemaId previous_schema;
                for (std::uint32_t ordinal{}; ordinal < *schema_count; ++ordinal)
                {
                    auto hash = reader.readUnsigned<std::uint64_t>();
                    std::string schema_name;
                    if (!hash ||
                        !readString(reader, max_decoded_bytes, schema_name))
                    {
                        return lux::cxx::unexpected(lux::asset::EAssetCodecError::CODEC_FAILURE);
                    }
                    WorldDataSchemaId schema{*hash, std::move(schema_name)};
                    if (!schema.valid() ||
                        (ordinal != 0U && !WorldDataSchemaIdLess{}(previous_schema, schema)) ||
                        !builder.addSchema(schema))
                    {
                        return lux::cxx::unexpected(lux::asset::EAssetCodecError::CODEC_FAILURE);
                    }
                    previous_schema = std::move(schema);
                }

                auto partitioner_hash = reader.readUnsigned<std::uint64_t>();
                std::string partitioner_name;
                if (!partitioner_hash ||
                    !readString(reader, max_decoded_bytes, partitioner_name))
                {
                    return lux::cxx::unexpected(lux::asset::EAssetCodecError::CODEC_FAILURE);
                }
                auto partitioner_version = reader.readUnsigned<std::uint32_t>();
                auto partition_count = reader.readUnsigned<std::uint32_t>();
                WorldPartitionerDescriptor partitioner{
                    WorldPartitionerId{*partitioner_hash, std::move(partitioner_name)},
                    partitioner_version ? *partitioner_version : 0U
                };
                if (!partitioner_version || !partition_count ||
                    !builder.setPartitioner(std::move(partitioner), *partition_count))
                {
                    return lux::cxx::unexpected(lux::asset::EAssetCodecError::CODEC_FAILURE);
                }

                auto volume_count = reader.readUnsigned<std::uint32_t>();
                if (!volume_count ||
                    !countFits(
                        *volume_count,
                        20U,
                        reader.remaining(),
                        max_decoded_bytes,
                        sizeof(WorldStorageVolumeDescription)
                    ))
                {
                    return lux::cxx::unexpected(lux::asset::EAssetCodecError::CODEC_FAILURE);
                }
                for (std::uint32_t ordinal{}; ordinal < *volume_count; ++ordinal)
                {
                    WorldStorageVolumeDescription volume;
                    if (!readString(reader, max_decoded_bytes, volume.member_name))
                        return lux::cxx::unexpected(lux::asset::EAssetCodecError::CODEC_FAILURE);
                    auto format_version = reader.readUnsigned<std::uint32_t>();
                    auto chunk_count = reader.readUnsigned<std::uint32_t>();
                    auto file_size = reader.readUnsigned<std::uint64_t>();
                    if (!format_version || !chunk_count || !file_size)
                        return lux::cxx::unexpected(lux::asset::EAssetCodecError::CODEC_FAILURE);
                    volume.format_version = *format_version;
                    volume.chunk_count = *chunk_count;
                    volume.file_size = *file_size;
                    if (!builder.addStorageVolume(std::move(volume)))
                        return lux::cxx::unexpected(lux::asset::EAssetCodecError::CODEC_FAILURE);
                }

                auto page_count = reader.readUnsigned<std::uint32_t>();
                if (!page_count ||
                    !countFits(
                        *page_count,
                        16U,
                        reader.remaining(),
                        max_decoded_bytes,
                        sizeof(WorldPartitionTablePageDescription)
                    ))
                {
                    return lux::cxx::unexpected(lux::asset::EAssetCodecError::CODEC_FAILURE);
                }
                std::uint32_t previous_page_end{};
                for (std::uint32_t ordinal{}; ordinal < *page_count; ++ordinal)
                {
                    auto first = reader.readUnsigned<std::uint32_t>();
                    auto count = reader.readUnsigned<std::uint32_t>();
                    auto volume = reader.readUnsigned<std::uint32_t>();
                    auto chunk = reader.readUnsigned<std::uint32_t>();
                    if (!first || !count || !volume || !chunk ||
                        (ordinal != 0U && *first != previous_page_end) ||
                        *count > std::numeric_limits<std::uint32_t>::max() - *first)
                    {
                        return lux::cxx::unexpected(lux::asset::EAssetCodecError::CODEC_FAILURE);
                    }
                    previous_page_end = *first + *count;
                    if (!builder.addPartitionTablePage({
                            partition::PartitionOrdinal{*first},
                            *count,
                            WorldChunkReference{*volume, *chunk}
                        }))
                    {
                        return lux::cxx::unexpected(lux::asset::EAssetCodecError::CODEC_FAILURE);
                    }
                }

                auto index_count = reader.readUnsigned<std::uint32_t>();
                if (!index_count ||
                    !countFits(
                        *index_count,
                        24U,
                        reader.remaining(),
                        max_decoded_bytes,
                        sizeof(WorldPartitionIndexDescription)
                    ))
                {
                    return lux::cxx::unexpected(lux::asset::EAssetCodecError::CODEC_FAILURE);
                }
                partition::PartitionIndexTypeId previous_index;
                for (std::uint32_t ordinal{}; ordinal < *index_count; ++ordinal)
                {
                    auto hash = reader.readUnsigned<std::uint64_t>();
                    std::string index_name;
                    if (!hash || !readString(reader, max_decoded_bytes, index_name))
                        return lux::cxx::unexpected(lux::asset::EAssetCodecError::CODEC_FAILURE);
                    auto index_version = reader.readUnsigned<std::uint32_t>();
                    auto volume = reader.readUnsigned<std::uint32_t>();
                    auto chunk = reader.readUnsigned<std::uint32_t>();
                    if (!index_version || !volume || !chunk)
                        return lux::cxx::unexpected(lux::asset::EAssetCodecError::CODEC_FAILURE);

                    WorldPartitionIndexDescription index{
                        partition::PartitionIndexTypeId{*hash, std::move(index_name)},
                        *index_version,
                        WorldChunkReference{*volume, *chunk}
                    };
                    const bool is_noncanonical =
                        ordinal != 0U &&
                        !(previous_index.hash < index.type.hash ||
                          (previous_index.hash == index.type.hash && previous_index.name < index.type.name));
                    if (is_noncanonical || !builder.addPartitionIndex(index))
                        return lux::cxx::unexpected(lux::asset::EAssetCodecError::CODEC_FAILURE);
                    previous_index = std::move(index.type);
                }

                if (reader.remaining() != 0U)
                    return lux::cxx::unexpected(lux::asset::EAssetCodecError::CODEC_FAILURE);
                auto world = std::move(builder).build();
                if (!world || world->retainedBytes() > max_decoded_bytes)
                    return lux::cxx::unexpected(lux::asset::EAssetCodecError::CODEC_FAILURE);

                auto shared = std::make_shared<WorldDescription>(std::move(*world));
                return shared;
            }
            catch (const std::bad_alloc&)
            {
                return lux::cxx::unexpected(lux::asset::EAssetCodecError::OUT_OF_MEMORY);
            }
        }
    } // namespace detail

    WorldAsset::WorldAsset(
        lux::asset::AssetInfo info,
        std::shared_ptr<const WorldDescription> data,
        std::vector<lux::asset::AssetAuxiliaryPayload> auxiliary
    ) noexcept
        : TAsset(std::move(info), std::move(data), std::move(auxiliary))
    {
    }

    lux::cxx::expected<std::shared_ptr<const WorldAsset>, lux::asset::AssetDecodeFailure> WorldAsset::create(
        lux::asset::AssetInfo info,
        std::shared_ptr<const WorldDescription> data,
        std::vector<lux::asset::AssetAuxiliaryPayload> auxiliary
    ) noexcept
    {
        const bool invalid_world = !data || data->bundleId().value.is_nil() ||
            data->generation().value.is_nil() || data->name().empty();
        if (info.id.isNull() || invalid_world)
        {
            return lux::cxx::unexpected(lux::asset::AssetDecodeFailure{
                lux::asset::EAssetDecodeError::INVALID_PAYLOAD,
                0U
            });
        }
        info.type = asset_type;
        try
        {
            std::sort(
                auxiliary.begin(),
                auxiliary.end(),
                [](const auto& left, const auto& right) noexcept { return left.tag < right.tag; }
            );
            for (std::size_t index = 0U; index < auxiliary.size(); ++index)
            {
                const bool invalid = auxiliary[index].tag == 0U || auxiliary[index].bytes.empty();
                const bool duplicate = index != 0U && auxiliary[index - 1U].tag == auxiliary[index].tag;
                if (invalid || duplicate)
                {
                    return lux::cxx::unexpected(lux::asset::AssetDecodeFailure{
                        lux::asset::EAssetDecodeError::INVALID_PAYLOAD,
                        index
                    });
                }
            }
            return std::shared_ptr<const WorldAsset>(
                new WorldAsset(std::move(info), std::move(data), std::move(auxiliary))
            );
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(lux::asset::AssetDecodeFailure{
                lux::asset::EAssetDecodeError::ALLOCATION_FAILURE,
                0U
            });
        }
    }
} // namespace lux::world

namespace lux::asset
{
    lux::cxx::expected<std::shared_ptr<const lux::world::WorldAsset>, AssetDecodeFailure>
    TAssetSerDeser<lux::world::WorldAsset>::decode(
        AssetId requested,
        lux::cxx::SharedBytes<> cooked_image,
        const AssetDecodeLimits& limits
    ) noexcept
    {
        auto image = inspectCookedAssetImage(requested, std::move(cooked_image), limits);
        if (!image)
            return lux::cxx::unexpected(image.error());
        if (image->magic() != lux::world::WorldAsset::primary_magic)
            return lux::cxx::unexpected(AssetDecodeFailure{EAssetDecodeError::INVALID_MAGIC, 0U});
        if (image->metadata().legacy_type_tag != lux::world::WorldAsset::legacy_type_tag)
            return lux::cxx::unexpected(AssetDecodeFailure{EAssetDecodeError::INVALID_TYPE, 0U});
        if (!image->information().empty())
            return lux::cxx::unexpected(AssetDecodeFailure{EAssetDecodeError::INVALID_LAYOUT, 0U});
        auto world = lux::world::detail::decodeWorld(
            image->data().view(),
            image->data().size(),
            limits.max_decoded_bytes
        );
        if (!world)
        {
            const auto code = world.error() == EAssetCodecError::OUT_OF_MEMORY
                ? EAssetDecodeError::ALLOCATION_FAILURE
                : EAssetDecodeError::INVALID_PAYLOAD;
            return lux::cxx::unexpected(AssetDecodeFailure{code, 0U});
        }
        std::vector<AssetAuxiliaryPayload> auxiliary(
            image->auxiliaryPayloads().begin(),
            image->auxiliaryPayloads().end()
        );
        return lux::world::WorldAsset::create(
            AssetInfo{
                image->metadata().id,
                lux::world::WorldAsset::asset_type,
                image->metadata().date,
                image->metadata().display_name,
                image->metadata().source_path,
                image->metadata().source_mtime
            },
            std::move(*world),
            std::move(auxiliary)
        );
    }

    lux::cxx::expected<std::vector<std::byte>, AssetEncodeFailure>
    TAssetSerDeser<lux::world::WorldAsset>::encode(
        const lux::world::WorldAsset& asset,
        const AssetEncodeLimits& limits
    ) noexcept
    {
        auto payload = lux::world::detail::encodeWorld(asset.data(), limits.max_encoded_bytes);
        if (!payload)
        {
            const auto code = payload.error() == EAssetCodecError::OUT_OF_MEMORY
                ? EAssetEncodeError::ALLOCATION_FAILURE
                : EAssetEncodeError::INVALID_PAYLOAD;
            return lux::cxx::unexpected(AssetEncodeFailure{code, 0U});
        }
        return detail::encodeCookedAssetImage(
            detail::CookedAssetWriteRequest{
                lux::world::WorldAsset::primary_magic,
                lux::world::WorldAsset::legacy_type_tag,
                asset.info(),
                {},
                *payload,
                asset.auxiliaryPayloads()
            },
            limits
        );
    }
} // namespace lux::asset
#include <algorithm>
