#pragma once

#include <lux/engine/world/storage/detail/WorldStorageCodec.hpp>
#include <optional>

namespace lux::world::detail
{
    struct WorldStorageReadRange final
    {
        std::uint32_t volume{};
        std::uint64_t offset{};
        std::uint64_t size{};
    };

    // One pure traversal for both owned memory and asynchronous range transports.
    // The description must outlive this decoder. No IO, callback or scheduling occurs here.
    class LUX_ENGINE_WORLD_STORAGE_PUBLIC WorldPartitionDecoder final
    {
    public:
        using Next = lux::cxx::expected<std::optional<WorldStorageReadRange>, WorldStorageCodecFailure>;

        WorldPartitionDecoder(
            const WorldDescription&,
            partition::PartitionOrdinal,
            std::size_t,
            std::stop_token
        ) noexcept;
        WorldPartitionDecoder(const WorldPartitionDecoder&) = delete;
        WorldPartitionDecoder& operator=(const WorldPartitionDecoder&) = delete;
        WorldPartitionDecoder(WorldPartitionDecoder&&) = delete;
        WorldPartitionDecoder& operator=(WorldPartitionDecoder&&) = delete;

        [[nodiscard]] Next start() noexcept;
        [[nodiscard]] Next accept(std::span<const std::byte>) noexcept;
        [[nodiscard]] WorldPartitionData takeResult() && noexcept;

    private:
        enum class EStage : std::uint8_t
        {
            HEADER,
            DESCRIPTOR,
            PAYLOAD
        };

        [[nodiscard]] Next request(std::uint64_t offset, std::uint64_t size) const noexcept;
        [[nodiscard]] Next beginChunk(WorldChunkReference, bool table) noexcept;
        [[nodiscard]] Next nextPartitionChunk() noexcept;

        const WorldDescription& world_;
        partition::PartitionOrdinal partition_;
        std::size_t max_bytes_{};
        std::stop_token stop_;
        EStage stage_{EStage::HEADER};
        bool reading_table_{};
        WorldChunkReference current_;
        const WorldPartitionTablePageDescription* table_page_{};
        WorldStorageVolumeHeader header_;
        WorldStorageChunkDescriptor descriptor_;
        WorldPartitionTablePage partition_page_;
        std::uint32_t first_extent_{};
        std::uint32_t extent_count_{};
        std::uint32_t extent_index_{};
        std::uint32_t chunk_in_extent_{};
        std::vector<std::byte> partition_bytes_;
        WorldPartitionData result_;
    };
}
