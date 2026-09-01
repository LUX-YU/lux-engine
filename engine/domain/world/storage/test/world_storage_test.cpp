#include <lux/engine/world/WorldPartitionData.hpp>
#include <lux/engine/world/storage/detail/WorldStorageCodec.hpp>

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <utility>
#include <vector>

namespace
{
    template <class Type>
    [[nodiscard]] Type id(std::uint8_t tail)
    {
        std::array<std::uint8_t, 16> bytes{};
        bytes[15] = tail;
        return Type{uuids::uuid(bytes)};
    }
}

int main()
{
    using namespace lux::world;
    using namespace lux::world::detail;

    const std::array payload_a{std::byte{1U}, std::byte{2U}};
    const std::array payload_b{std::byte{3U}};
    const std::array payload_c{std::byte{4U}, std::byte{5U}, std::byte{6U}};

    const std::array first_data{
        WorldEncodedDataRecord{0U, 1U, payload_a},
        WorldEncodedDataRecord{1U, 2U, payload_b}
    };
    const std::array second_data{
        WorldEncodedDataRecord{0U, 1U, payload_c}
    };
    const std::array objects{
        WorldEncodedObjectRecord{id<lux::world::WorldObjectId>(1U), first_data},
        WorldEncodedObjectRecord{id<lux::world::WorldObjectId>(2U), second_data}
    };

    auto partition_wire = encodeWorldPartitionData(lux::partition::PartitionOrdinal{7U}, objects);
    assert(partition_wire);

    const std::array records{
        WorldPartitionRecord{id<WorldPartitionId>(1U), 0U, 2U},
        WorldPartitionRecord{id<WorldPartitionId>(2U), 2U, 1U}
    };
    const std::array extents{
        WorldPartitionExtent{0U, 1U, 1U},
        WorldPartitionExtent{1U, 0U, 2U},
        WorldPartitionExtent{0U, 3U, 1U}
    };
    auto page_wire = encodeWorldPartitionTablePage(lux::partition::PartitionOrdinal{4U}, records, extents);
    assert(page_wire);

    const std::array index_wire{std::byte{9U}, std::byte{8U}};
    const std::array chunks{
        WorldStorageChunkInput{
            EWorldStorageChunkKind::PARTITION_TABLE_PAGE,
            EWorldStorageCodec::NONE,
            *page_wire
        },
        WorldStorageChunkInput{
            EWorldStorageChunkKind::WORLD_PARTITION_DATA,
            EWorldStorageCodec::NONE,
            *partition_wire
        },
        WorldStorageChunkInput{
            EWorldStorageChunkKind::PARTITION_INDEX_PAGE,
            EWorldStorageCodec::NONE,
            index_wire
        }
    };

    const auto bundle = id<WorldBundleId>(10U);
    const auto generation = id<WorldBundleGeneration>(11U);
    auto volume = encodeWorldStorageVolume(bundle, generation, 0U, chunks);
    assert(volume);
    const WorldStorageVolumeDescription volume_description{
        "test.wvol0",
        1U,
        static_cast<std::uint32_t>(chunks.size()),
        volume->size()
    };
    assert(volume->size() >= kWorldStorageVolumeHeaderWireSize + chunks.size() * kWorldStorageChunkDescriptorWireSize);

    auto header = decodeWorldStorageVolumeHeader(
        std::span<const std::byte>(*volume).first(kWorldStorageVolumeHeaderWireSize),
        bundle,
        generation,
        0U,
        volume_description
    );
    assert(header);
    assert(header->chunk_count == chunks.size());
    assert(header->descriptor_stride == kWorldStorageChunkDescriptorWireSize);
    auto wrong_format = volume_description;
    wrong_format.format_version = 2U;
    assert(!decodeWorldStorageVolumeHeader(
        std::span<const std::byte>(*volume).first(kWorldStorageVolumeHeaderWireSize),
        bundle,
        generation,
        0U,
        wrong_format
    ));
    auto wrong_chunk_count = volume_description;
    ++wrong_chunk_count.chunk_count;
    assert(!decodeWorldStorageVolumeHeader(
        std::span<const std::byte>(*volume).first(kWorldStorageVolumeHeaderWireSize),
        bundle,
        generation,
        0U,
        wrong_chunk_count
    ));
    auto wrong_file_size = volume_description;
    ++wrong_file_size.file_size;
    assert(!decodeWorldStorageVolumeHeader(
        std::span<const std::byte>(*volume).first(kWorldStorageVolumeHeaderWireSize),
        bundle,
        generation,
        0U,
        wrong_file_size
    ));

    assert(!decodeWorldStorageVolumeHeader(
        std::span<const std::byte>(*volume).first(kWorldStorageVolumeHeaderWireSize),
        id<WorldBundleId>(12U),
        generation,
        0U,
        volume_description
    ));
    assert(!decodeWorldStorageVolumeHeader(
        std::span<const std::byte>(*volume).first(kWorldStorageVolumeHeaderWireSize),
        bundle,
        id<WorldBundleGeneration>(13U),
        0U,
        volume_description
    ));
    assert(!decodeWorldStorageVolumeHeader(
        std::span<const std::byte>(*volume).first(kWorldStorageVolumeHeaderWireSize),
        bundle,
        generation,
        1U,
        volume_description
    ));

    std::array<WorldStorageChunkDescriptor, 3U> descriptors;
    for (std::uint32_t ordinal{}; ordinal < descriptors.size(); ++ordinal)
    {
        const std::size_t descriptor_offset =
            static_cast<std::size_t>(header->descriptor_offset) +
            ordinal * kWorldStorageChunkDescriptorWireSize;
        auto descriptor = decodeWorldStorageChunkDescriptor(
            std::span<const std::byte>(*volume).subspan(
                descriptor_offset,
                kWorldStorageChunkDescriptorWireSize
            ),
            *header,
            ordinal
        );
        assert(descriptor);
        descriptors[ordinal] = *descriptor;
    }
    assert(descriptors[0].kind == EWorldStorageChunkKind::PARTITION_TABLE_PAGE);
    assert(descriptors[1].kind == EWorldStorageChunkKind::WORLD_PARTITION_DATA);
    assert(descriptors[2].kind == EWorldStorageChunkKind::PARTITION_INDEX_PAGE);

    auto decodeChunk = [&](std::size_t ordinal) {
        const auto& descriptor = descriptors[ordinal];
        return decodeWorldStorageChunkPayload(
            std::span<const std::byte>(*volume).subspan(
                static_cast<std::size_t>(descriptor.offset),
                static_cast<std::size_t>(descriptor.stored_size)
            ),
            descriptor,
            std::numeric_limits<std::size_t>::max(),
            {}
        );
    };

    auto decoded_page_chunk = decodeChunk(0U);
    auto decoded_partition_chunk = decodeChunk(1U);
    assert(decoded_page_chunk);
    assert(decoded_partition_chunk);
    assert(*decoded_page_chunk == *page_wire);
    assert(*decoded_partition_chunk == *partition_wire);

    auto page = decodeWorldPartitionTablePage(
        *decoded_page_chunk,
        lux::partition::PartitionOrdinal{4U},
        2U,
        std::numeric_limits<std::size_t>::max(),
        {}
    );
    assert(page);
    const auto* first_record = page->find(lux::partition::PartitionOrdinal{4U});
    const auto* second_record = page->find(lux::partition::PartitionOrdinal{5U});
    assert(first_record != nullptr);
    assert(second_record != nullptr);
    assert(page->find(lux::partition::PartitionOrdinal{6U}) == nullptr);
    assert(page->partitionExtents(*first_record).size() == 2U);
    assert(page->partitionExtents(*first_record)[1].volume == 1U);
    assert(page->partitionExtents(*second_record).size() == 1U);

    auto partition = decodeWorldPartitionData(
        *decoded_partition_chunk,
        bundle,
        generation,
        lux::partition::PartitionOrdinal{7U},
        2U,
        std::numeric_limits<std::size_t>::max(),
        {}
    );
    assert(partition);
    assert(partition->bundle() == bundle);
    assert(partition->generation() == generation);
    assert(partition->partition().value == 7U);
    assert(partition->objectCount() == 2U);
    const auto first_object = partition->objectAt(0U);
    assert(first_object);
    assert(first_object.id() == id<lux::world::WorldObjectId>(1U));
    assert(first_object.bundle() == bundle);
    assert(first_object.generation() == generation);
    assert(first_object.dataCount() == 2U);
    assert(first_object.schemaOrdinalAt(0U) == 0U);
    assert(first_object.schemaVersionAt(1U) == 2U);
    assert(std::ranges::equal(first_object.payloadAt(0U), payload_a));
    assert(std::ranges::equal(first_object.payloadAt(1U), payload_b));
    const auto second_object = partition->objectAt(1U);
    assert(std::ranges::equal(second_object.payloadAt(0U), payload_c));
    assert(!partition->objectAt(2U));

    assert(!decodeWorldPartitionData(
        *decoded_partition_chunk,
        bundle,
        generation,
        lux::partition::PartitionOrdinal{8U},
        2U,
        std::numeric_limits<std::size_t>::max(),
        {}
    ));
    assert(!decodeWorldPartitionData(
        *decoded_partition_chunk,
        bundle,
        generation,
        lux::partition::PartitionOrdinal{7U},
        1U,
        std::numeric_limits<std::size_t>::max(),
        {}
    ));

    auto corrupted_volume = *volume;
    corrupted_volume[static_cast<std::size_t>(descriptors[1].offset)] ^= std::byte{0x01U};
    assert(!decodeWorldStorageChunkPayload(
        std::span<const std::byte>(corrupted_volume).subspan(
            static_cast<std::size_t>(descriptors[1].offset),
            static_cast<std::size_t>(descriptors[1].stored_size)
        ),
        descriptors[1],
        std::numeric_limits<std::size_t>::max(),
        {}
    ));

    assert(!decodeWorldStorageChunkDescriptor(
        std::span<const std::byte>(*volume).subspan(
            static_cast<std::size_t>(header->descriptor_offset),
            kWorldStorageChunkDescriptorWireSize - 1U
        ),
        *header,
        0U
    ));

    std::stop_source cancelled;
    cancelled.request_stop();
    auto cancelled_chunk = decodeWorldStorageChunkPayload(
        std::span<const std::byte>(*volume).subspan(
            static_cast<std::size_t>(descriptors[1].offset),
            static_cast<std::size_t>(descriptors[1].stored_size)
        ),
        descriptors[1],
        std::numeric_limits<std::size_t>::max(),
        cancelled.get_token()
    );
    assert(!cancelled_chunk);
    assert(cancelled_chunk.error().code == EWorldStorageCodecError::CANCELLED);
    auto cancelled_page = decodeWorldPartitionTablePage(
        *decoded_page_chunk,
        lux::partition::PartitionOrdinal{4U},
        2U,
        std::numeric_limits<std::size_t>::max(),
        cancelled.get_token()
    );
    assert(!cancelled_page);
    assert(cancelled_page.error().code == EWorldStorageCodecError::CANCELLED);
    auto cancelled_partition = decodeWorldPartitionData(
        *decoded_partition_chunk,
        bundle,
        generation,
        lux::partition::PartitionOrdinal{7U},
        2U,
        std::numeric_limits<std::size_t>::max(),
        cancelled.get_token()
    );
    assert(!cancelled_partition);
    assert(cancelled_partition.error().code == EWorldStorageCodecError::CANCELLED);

    return 0;
}
