#pragma once

#include <lux/engine/ecs/pixel/systems/PixelFieldRuntime.hpp>

#include <array>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace lux::ecs
{
    struct PreparedPixelChunk::Storage final
    {
        PixelChunkCoord coordinate{};
        std::uint32_t channels_mask{0u};
        std::vector<MaterialId> cells;
        std::vector<std::uint8_t> moved;
        PixelDirtyLedger ledger{
            PixelFieldRuntime::kChunkSizeCells,
            PixelFieldRuntime::kChunkSizeCells};
        std::vector<float> temperature;
        std::vector<float> lifetime;
        std::array<
            std::uint8_t,
            PixelFieldRuntime::kTilesPerChunkCount> active{};
        std::array<
            std::uint8_t,
            PixelFieldRuntime::kTilesPerChunkCount> active_next{};
        std::array<
            std::uint8_t,
            PixelFieldRuntime::kTilesPerChunkCount> changed{};
        std::array<
            std::uint8_t,
            PixelFieldRuntime::kTilesPerChunkCount> minimum_x{};
        std::array<
            std::uint8_t,
            PixelFieldRuntime::kTilesPerChunkCount> minimum_y{};
        std::array<
            std::uint8_t,
            PixelFieldRuntime::kTilesPerChunkCount> maximum_x{};
        std::array<
            std::uint8_t,
            PixelFieldRuntime::kTilesPerChunkCount> maximum_y{};
        std::array<
            std::uint8_t,
            PixelFieldRuntime::kTilesPerChunkCount> next_minimum_x{};
        std::array<
            std::uint8_t,
            PixelFieldRuntime::kTilesPerChunkCount> next_minimum_y{};
        std::array<
            std::uint8_t,
            PixelFieldRuntime::kTilesPerChunkCount> next_maximum_x{};
        std::array<
            std::uint8_t,
            PixelFieldRuntime::kTilesPerChunkCount> next_maximum_y{};
        std::array<
            std::uint16_t,
            PixelFieldRuntime::kTilesPerChunkCount> blocking{};
        lux::cxx::algorithm::Sha256Digest base_digest;
        std::uint64_t sequence{0u};
        std::unordered_map<std::uint16_t, MaterialId> delta;
        bool presentation_active{false};
        bool simulation_active{false};
    };
}
