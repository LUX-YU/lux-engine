#pragma once

#include <lux/engine/ecs/tilemap/TilemapTypes.hpp>
#include <lux/engine/ecs/tilemap/visibility.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <unordered_map>
#include <vector>

namespace lux::ecs::detail
{
    template <typename Key, typename Hash, typename Equal>
    class SparseActiveMap;
}

namespace lux::ecs
{
    class LUX_ENGINE_ECS_TILEMAP_PUBLIC TilemapRuntime final
    {
    public:
        static constexpr std::uint32_t kChunkSizeTiles = 256u;
        static constexpr std::uint32_t kChunkShift = 8u;
        static constexpr std::uint32_t kChunkMask = 255u;
        static constexpr std::size_t kChunkTileCount =
            static_cast<std::size_t>(kChunkSizeTiles) * kChunkSizeTiles;

        TilemapRuntime() = default;
        ~TilemapRuntime();
        TilemapRuntime(const TilemapRuntime&) = delete;
        TilemapRuntime& operator=(const TilemapRuntime&) = delete;

        [[nodiscard]] TilemapHandle create(const TilemapDesc& description);
        void destroy(TilemapHandle handle);
        [[nodiscard]] bool isAlive(TilemapHandle handle) const noexcept;
        [[nodiscard]] TilemapDesc desc(TilemapHandle handle) const noexcept;

        [[nodiscard]] bool loadChunk(
            TilemapHandle handle,
            TileChunkLoad&& load);
        [[nodiscard]] bool captureChunk(
            TilemapHandle handle,
            TileChunkCoord coordinate,
            TileChunkSnapshot& output) const;
        [[nodiscard]] bool unloadChunk(
            TilemapHandle handle,
            TileChunkCoord coordinate,
            TileChunkSnapshot& output);
        /// Retire one resident chunk without copying a persistence snapshot.
        /// The caller owns any domain persistence policy before this point.
        [[nodiscard]] bool discardChunk(
            TilemapHandle handle,
            TileChunkCoord coordinate) noexcept;
        [[nodiscard]] bool chunkResident(
            TilemapHandle handle,
            TileChunkCoord coordinate) const noexcept;
        void residentChunks(
            TilemapHandle handle,
            std::vector<TileChunkCoord>& output) const;
        /// Allocation-free view of active chunks. The order is unspecified
        /// and the span is invalidated by chunk residency or activity changes.
        [[nodiscard]] std::span<const TileChunkCoord> activeKeys(
            TilemapHandle handle) const noexcept;
        [[nodiscard]] bool chunkActive(
            TilemapHandle handle,
            TileChunkCoord coordinate) const noexcept;
        [[nodiscard]] bool setChunkActive(
            TilemapHandle handle,
            TileChunkCoord coordinate,
            bool active) noexcept;

        [[nodiscard]] std::uint16_t tileAt(
            TilemapHandle handle,
            TileCellCoord coordinate) const noexcept;
        [[nodiscard]] bool setTile(
            TilemapHandle handle,
            TileCellCoord coordinate,
            std::uint16_t tile) noexcept;
        [[nodiscard]] bool markChunkDirty(
            TilemapHandle handle,
            TileChunkCoord coordinate) noexcept;

        [[nodiscard]] bool exportDirty(
            TilemapHandle handle,
            TileChunkCoord coordinate,
            TileChunkRenderExport& output);
        void confirmExport(
            TilemapHandle handle,
            TileChunkCoord coordinate,
            std::uint64_t revision,
            bool uploaded) noexcept;

        [[nodiscard]] TilemapRuntimeStats stats() const noexcept;

    private:
        struct DirtyBounds final
        {
            std::uint16_t minimum_x{0u};
            std::uint16_t minimum_y{0u};
            std::uint16_t maximum_x{0u};
            std::uint16_t maximum_y{0u};
            bool valid{false};
        };

        struct Chunk final
        {
            std::vector<std::uint16_t> tiles;
            lux::cxx::algorithm::Sha256Digest base_digest;
            std::uint64_t sequence{0u};
            std::uint64_t revision{1u};
            std::uint64_t inflight_revision{0u};
            std::unordered_map<std::uint16_t, std::uint16_t> delta;
            DirtyBounds dirty{0u, 0u, 255u, 255u, true};
            bool active{true};
        };

        struct Field final
        {
            using ActiveChunks = detail::SparseActiveMap<
                TileChunkCoord,
                TileChunkCoordHash,
                TileChunkCoordEqual>;

            Field();
            ~Field();
            Field(Field&&) noexcept;
            Field& operator=(Field&&) noexcept;
            Field(const Field&) = delete;
            Field& operator=(const Field&) = delete;

            TilemapHandle handle{};
            TilemapDesc description;
            std::unordered_map<TileChunkCoord, Chunk, TileChunkCoordHash>
                chunks;
            std::unique_ptr<ActiveChunks> active_chunks;
        };

        [[nodiscard]] Field* resolve(TilemapHandle handle) noexcept;
        [[nodiscard]] const Field* resolve(
            TilemapHandle handle) const noexcept;
        [[nodiscard]] static TileChunkCoord chunkOf(
            TileCellCoord coordinate) noexcept;
        [[nodiscard]] static std::uint32_t localCoordinate(
            std::int64_t coordinate) noexcept;
        [[nodiscard]] static std::uint16_t ordinal(
            std::uint32_t x,
            std::uint32_t y) noexcept;
        static void growDirty(
            DirtyBounds& dirty,
            std::uint32_t x,
            std::uint32_t y) noexcept;
        static void removeActiveChunk(
            Field& field,
            TileChunkCoord coordinate,
            Chunk& chunk) noexcept;
        [[nodiscard]] static std::uint64_t chunkResidentBytes(
            const Chunk& chunk) noexcept;

        lux::cxx::SlotMap<Field, TilemapRuntimeTag> fields_;
        std::uint64_t resident_chunks_{0u};
        std::uint64_t active_chunks_{0u};
        std::uint64_t resident_bytes_{0u};
        mutable std::uint64_t active_enumeration_chunks_visited_last_{0u};
    };
} // namespace lux::ecs
