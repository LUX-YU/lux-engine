#pragma once

#include <lux/cxx/container/SlotMap.hpp>

#include <lux/engine/ecs/tilemap/TilemapId.hpp>
#include <lux/cxx/algorithm/Sha256.hpp>
#include <lux/engine/description/Tilemap2D.hpp>
#include <lux/engine/math/Grid.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace lux::ecs
{
    struct TilemapRuntimeTag final {};
    using TilemapHandle = lux::cxx::SlotKey<TilemapRuntimeTag>;
    using TileChunkCoord = lux::math::GridCoord2i64;

    struct TileChunkCoordHash final
    {
        [[nodiscard]] std::size_t operator()(
            const TileChunkCoord& value) const noexcept
        {
            auto mix = [](std::uint64_t input) noexcept
            {
                input ^= input >> 30u;
                input *= 0xbf58476d1ce4e5b9ull;
                input ^= input >> 27u;
                input *= 0x94d049bb133111ebull;
                return input ^ (input >> 31u);
            };
            return static_cast<std::size_t>(
                mix(static_cast<std::uint64_t>(value.x)) ^
                (mix(static_cast<std::uint64_t>(value.y)) << 1u));
        }
    };

    struct TileChunkCoordEqual final
    {
        [[nodiscard]] bool operator()(
            const TileChunkCoord& left,
            const TileChunkCoord& right) const noexcept
        {
            return left.x == right.x && left.y == right.y;
        }
    };

    struct TileCellCoord final
    {
        std::int64_t x{0};
        std::int64_t y{0};
    };

    struct TilemapDesc final
    {
        TilemapId id;
    };

    struct TileChunkDeltaCell final
    {
        std::uint16_t x{0u};
        std::uint16_t y{0u};
        std::uint16_t tile{lux::rdesc::kEmptyTile};

        friend bool operator==(
            const TileChunkDeltaCell&,
            const TileChunkDeltaCell&) = default;
    };

    struct TileChunkLoad final
    {
        TileChunkCoord coordinate{};
        std::vector<std::uint16_t> tiles;
        lux::cxx::algorithm::Sha256Digest base_digest;
        std::uint64_t sequence{0u};
        std::vector<TileChunkDeltaCell> delta;
        bool active{true};
    };

    struct TileChunkSnapshot final
    {
        TileChunkCoord coordinate{};
        std::vector<std::uint16_t> tiles;
        lux::cxx::algorithm::Sha256Digest base_digest;
        std::uint64_t sequence{0u};
        std::vector<TileChunkDeltaCell> delta;
        bool active{true};
    };

    struct TileDirtyRect final
    {
        std::uint32_t x{0u};
        std::uint32_t y{0u};
        std::uint32_t width{0u};
        std::uint32_t height{0u};
    };

    struct TileChunkRenderExport final
    {
        TileDirtyRect rect;
        std::shared_ptr<const std::byte[]> pixels;
        std::uint64_t pixel_bytes{0u};
        std::uint64_t content_revision{0u};

        [[nodiscard]] bool empty() const noexcept
        {
            return !pixels || pixel_bytes == 0u;
        }
    };

    struct TilemapRuntimeStats final
    {
        std::uint64_t resident_chunks{0u};
        std::uint64_t active_chunks{0u};
        std::uint64_t resident_bytes{0u};
        std::uint64_t active_enumeration_chunks_visited_last{0u};
    };
} // namespace lux::ecs
