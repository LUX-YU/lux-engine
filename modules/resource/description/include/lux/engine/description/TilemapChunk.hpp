#pragma once
/**
 * @file TilemapChunk.hpp
 * @brief Description-owned immutable Tilemap chunk values.
 */

#include <cstddef>
#include <cstdint>
#include <vector>

namespace lux::tilemap
{
    inline constexpr std::uint32_t kTilemapChunkEdge = 256u;
    inline constexpr std::size_t kTilemapChunkTileCount =
        static_cast<std::size_t>(kTilemapChunkEdge) * kTilemapChunkEdge;

    struct TilemapChunkBlobV1 final
    {
        std::vector<std::uint16_t> tiles;

        friend bool operator==(
            const TilemapChunkBlobV1&,
            const TilemapChunkBlobV1&) = default;
    };
} // namespace lux::tilemap
