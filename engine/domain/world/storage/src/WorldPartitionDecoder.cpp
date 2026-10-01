#include <lux/engine/world/storage/detail/WorldPartitionDecoder.hpp>
#include <utility>

namespace lux::world::detail
{
    WorldPartitionDecoder::WorldPartitionDecoder(
        const WorldDescription& world,
        partition::PartitionOrdinal partition,
        std::size_t max_bytes,
        std::stop_token stop
    ) noexcept
        : world_(world), partition_(partition), max_bytes_(max_bytes), stop_(stop)
    {}

    WorldPartitionDecoder::Next WorldPartitionDecoder::request(std::uint64_t offset, std::uint64_t size) const noexcept
    {
        if (stop_.stop_requested())
            return lux::cxx::unexpected(WorldStorageCodecFailure{EWorldStorageCodecError::CANCELLED});
        if (size > std::numeric_limits<std::size_t>::max() || size > max_bytes_)
            return lux::cxx::unexpected(
                WorldStorageCodecFailure{EWorldStorageCodecError::SIZE_LIMIT, current_.volume, current_.chunk, offset}
            );
        return std::optional{WorldStorageReadRange{current_.volume, offset, size}};
    }

    WorldPartitionDecoder::Next WorldPartitionDecoder::beginChunk(WorldChunkReference reference, bool table) noexcept
    {
        if (reference.volume >= world_.storageVolumes().size())
            return lux::cxx::unexpected(
                WorldStorageCodecFailure{EWorldStorageCodecError::INVALID_VOLUME, reference.volume}
            );
        current_ = reference;
        reading_table_ = table;
        stage_ = EStage::HEADER;
        return request(0U, kWorldStorageVolumeHeaderWireSize);
    }

    WorldPartitionDecoder::Next WorldPartitionDecoder::start() noexcept
    {
        if (max_bytes_ == 0U)
            return lux::cxx::unexpected(WorldStorageCodecFailure{EWorldStorageCodecError::SIZE_LIMIT});
        if (partition_.value >= world_.partitionCount())
            return lux::cxx::unexpected(WorldStorageCodecFailure{EWorldStorageCodecError::INVALID_PARTITION});
        table_page_ = world_.partitionTable().findPage(partition_);
        if (!table_page_)
            return lux::cxx::unexpected(WorldStorageCodecFailure{EWorldStorageCodecError::INVALID_PARTITION});
        return beginChunk(table_page_->chunk, true);
    }

    WorldPartitionDecoder::Next WorldPartitionDecoder::accept(std::span<const std::byte> bytes) noexcept
    {
        if (stop_.stop_requested())
            return lux::cxx::unexpected(WorldStorageCodecFailure{EWorldStorageCodecError::CANCELLED});
        if (stage_ == EStage::HEADER)
        {
            auto decoded = decodeWorldStorageVolumeHeader(
                bytes,
                world_.bundleId(),
                world_.generation(),
                current_.volume,
                world_.storageVolumes()[current_.volume]
            );
            if (!decoded)
                return lux::cxx::unexpected(decoded.error());
            header_ = *decoded;
            if (current_.chunk >= header_.chunk_count)
                return lux::cxx::unexpected(
                    WorldStorageCodecFailure{EWorldStorageCodecError::CORRUPT_DESCRIPTOR, current_.volume}
                );
            stage_ = EStage::DESCRIPTOR;
            return request(
                header_.descriptor_offset + static_cast<std::uint64_t>(current_.chunk) * header_.descriptor_stride,
                header_.descriptor_stride
            );
        }
        if (stage_ == EStage::DESCRIPTOR)
        {
            auto decoded = decodeWorldStorageChunkDescriptor(bytes, header_, current_.chunk);
            if (!decoded)
                return lux::cxx::unexpected(decoded.error());
            descriptor_ = *decoded;
            const bool exceeds_limit = descriptor_.stored_size > max_bytes_ || descriptor_.decoded_size > max_bytes_;
            if (exceeds_limit)
                return lux::cxx::unexpected(WorldStorageCodecFailure{
                    EWorldStorageCodecError::SIZE_LIMIT,
                    current_.volume,
                    current_.chunk,
                    descriptor_.offset
                });
            stage_ = EStage::PAYLOAD;
            return request(descriptor_.offset, descriptor_.stored_size);
        }

        auto payload = decodeWorldStorageChunkPayload(bytes, descriptor_, max_bytes_, stop_);
        if (!payload)
            return lux::cxx::unexpected(payload.error());
        if (reading_table_)
        {
            if (descriptor_.kind != EWorldStorageChunkKind::PARTITION_TABLE_PAGE)
                return lux::cxx::unexpected(
                    WorldStorageCodecFailure{EWorldStorageCodecError::CORRUPT_DESCRIPTOR, current_.volume}
                );
            auto page =
                decodeWorldPartitionTablePage(*payload, table_page_->first, table_page_->count, max_bytes_, stop_);
            if (!page)
                return lux::cxx::unexpected(page.error());
            if (!page->find(partition_))
                return lux::cxx::unexpected(WorldStorageCodecFailure{EWorldStorageCodecError::INVALID_PARTITION});
            partition_page_ = std::move(*page);
            const auto& record = *partition_page_.find(partition_);
            if (record.extent_count == 0U)
                return lux::cxx::unexpected(WorldStorageCodecFailure{EWorldStorageCodecError::DECODE_FAILURE});
            first_extent_ = record.first_extent;
            extent_count_ = record.extent_count;
            extent_index_ = 0U;
            chunk_in_extent_ = 0U;
            return nextPartitionChunk();
        }
        if (descriptor_.kind != EWorldStorageChunkKind::WORLD_PARTITION_DATA)
            return lux::cxx::unexpected(
                WorldStorageCodecFailure{EWorldStorageCodecError::CORRUPT_DESCRIPTOR, current_.volume}
            );
        const bool exceeds_limit =
            partition_bytes_.size() > max_bytes_ || payload->size() > max_bytes_ - partition_bytes_.size();
        if (exceeds_limit)
            return lux::cxx::unexpected(WorldStorageCodecFailure{
                EWorldStorageCodecError::SIZE_LIMIT,
                current_.volume,
                current_.chunk,
                descriptor_.offset
            });
        partition_bytes_.insert(partition_bytes_.end(), payload->begin(), payload->end());
        return nextPartitionChunk();
    }

    WorldPartitionDecoder::Next WorldPartitionDecoder::nextPartitionChunk() noexcept
    {
        while (extent_index_ < extent_count_)
        {
            const std::size_t ordinal = static_cast<std::size_t>(first_extent_) + extent_index_;
            if (ordinal >= partition_page_.extents.size())
                return lux::cxx::unexpected(WorldStorageCodecFailure{EWorldStorageCodecError::CORRUPT_DESCRIPTOR});
            const auto& extent = partition_page_.extents[ordinal];
            if (chunk_in_extent_ == 0U)
            {
                if (extent.volume >= world_.storageVolumes().size())
                    return lux::cxx::unexpected(
                        WorldStorageCodecFailure{EWorldStorageCodecError::INVALID_VOLUME, extent.volume}
                    );
                const auto count = world_.storageVolumes()[extent.volume].chunk_count;
                const bool invalid_first = extent.first_chunk > count;
                const bool invalid_count =
                    extent.chunk_count == 0U || (!invalid_first && extent.chunk_count > count - extent.first_chunk);
                const bool invalid_extent = invalid_first || invalid_count;
                if (invalid_extent)
                    return lux::cxx::unexpected(
                        WorldStorageCodecFailure{EWorldStorageCodecError::CORRUPT_DESCRIPTOR, extent.volume}
                    );
            }
            if (chunk_in_extent_ < extent.chunk_count)
            {
                const WorldChunkReference reference{extent.volume, extent.first_chunk + chunk_in_extent_};
                ++chunk_in_extent_;
                return beginChunk(reference, false);
            }
            ++extent_index_;
            chunk_in_extent_ = 0U;
        }
        const auto* record = partition_page_.find(partition_);
        if (!record)
            return lux::cxx::unexpected(WorldStorageCodecFailure{EWorldStorageCodecError::INVALID_PARTITION});
        auto decoded = decodeWorldPartitionData(
            partition_bytes_,
            world_.bundleId(),
            world_.generation(),
            partition_,
            record->id,
            static_cast<std::uint32_t>(world_.schemas().size()),
            max_bytes_,
            stop_
        );
        if (!decoded)
            return lux::cxx::unexpected(decoded.error());
        result_ = std::move(*decoded);
        return std::nullopt;
    }

    WorldPartitionData WorldPartitionDecoder::takeResult() && noexcept
    {
        return std::move(result_);
    }
}

namespace lux::world
{
    lux::cxx::expected<WorldPartitionData, WorldStorageCodecFailure> decodeWorldStoragePartition(
        const WorldDescription& world,
        std::span<const lux::cxx::SharedBytes<>> volumes,
        partition::PartitionOrdinal partition,
        std::size_t max_bytes,
        std::stop_token stop
    ) noexcept
    {
        detail::WorldPartitionDecoder decoder(world, partition, max_bytes, stop);
        auto next = decoder.start();
        while (next && *next)
        {
            const auto range = **next;
            if (range.volume >= volumes.size())
                return lux::cxx::unexpected(
                    WorldStorageCodecFailure{EWorldStorageCodecError::INVALID_VOLUME, range.volume, 0U, range.offset}
                );
            const auto bytes = volumes[range.volume].view();
            const bool invalid_offset = range.offset > bytes.size();
            const bool invalid_range = invalid_offset || range.size > bytes.size() - range.offset;
            if (invalid_range)
                return lux::cxx::unexpected(
                    WorldStorageCodecFailure{EWorldStorageCodecError::RANGE_OVERFLOW, range.volume, 0U, range.offset}
                );
            next = decoder.accept(
                bytes.subspan(static_cast<std::size_t>(range.offset), static_cast<std::size_t>(range.size))
            );
        }
        if (!next)
            return lux::cxx::unexpected(next.error());
        return std::move(decoder).takeResult();
    }
}
