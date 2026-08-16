#include <lux/engine/ecs/pixel/PreparedPixelChunkStorage.hpp>

#include <limits>
#include <utility>

namespace lux::ecs
{
    namespace
    {
        [[nodiscard]] bool coordinateIsSafe(
            PixelChunkCoord coordinate) noexcept
        {
            constexpr auto edge = static_cast<std::int64_t>(
                PixelFieldRuntime::kChunkSizeCells);
            constexpr auto minimum =
                std::numeric_limits<std::int64_t>::min() / edge + 1;
            constexpr auto maximum =
                std::numeric_limits<std::int64_t>::max() / edge - 1;
            return coordinate.x >= minimum && coordinate.x <= maximum &&
                coordinate.y >= minimum && coordinate.y <= maximum;
        }

        [[nodiscard]] bool isBlocking(
            const PixelChunkPreparationContext& context,
            MaterialId material) noexcept
        {
            return material < context.blocking_materials.size() &&
                context.blocking_materials[material] != 0u;
        }
    }

    PreparedPixelChunk::PreparedPixelChunk() noexcept = default;

    PreparedPixelChunk::PreparedPixelChunk(
        std::unique_ptr<Storage> storage) noexcept
        : storage_(std::move(storage))
    {}

    PreparedPixelChunk::~PreparedPixelChunk() = default;
    PreparedPixelChunk::PreparedPixelChunk(PreparedPixelChunk&&) noexcept =
        default;
    PreparedPixelChunk& PreparedPixelChunk::operator=(
        PreparedPixelChunk&&) noexcept = default;

    PreparedPixelChunk::operator bool() const noexcept
    {
        return static_cast<bool>(storage_);
    }

    PixelChunkCoord PreparedPixelChunk::coordinate() const noexcept
    {
        return storage_ ? storage_->coordinate : PixelChunkCoord{};
    }

    lux::cxx::expected<PreparedPixelChunk, EPixelChunkPreparationError>
    preparePixelChunk(
        PixelChunkLoad load,
        PixelChunkPreparationContext context)
    {
        if (context.blocking_materials.empty() ||
            context.blocking_materials.front() != 0u)
        {
            return lux::cxx::unexpected(
                EPixelChunkPreparationError::INVALID_CONTEXT);
        }
        if (!coordinateIsSafe(load.coordinate))
        {
            return lux::cxx::unexpected(
                EPixelChunkPreparationError::INVALID_COORDINATE);
        }
        const bool needs_temperature =
            (context.channels_mask & channelBit(ECellChannel::TEMPERATURE)) !=
            0u;
        const bool needs_lifetime =
            (context.channels_mask & channelBit(ECellChannel::LIFETIME)) !=
            0u;
        if (load.materials.size() != PixelFieldRuntime::kChunkCellCount ||
            (!load.temperature.empty() &&
             load.temperature.size() != PixelFieldRuntime::kChunkCellCount) ||
            (!load.lifetime.empty() &&
             load.lifetime.size() != PixelFieldRuntime::kChunkCellCount) ||
            needs_temperature != !load.temperature.empty() ||
            needs_lifetime != !load.lifetime.empty())
        {
            return lux::cxx::unexpected(
                EPixelChunkPreparationError::INVALID_CHANNELS);
        }

        auto storage = std::make_unique<PreparedPixelChunk::Storage>();
        storage->coordinate = load.coordinate;
        storage->channels_mask = context.channels_mask;
        storage->cells = std::move(load.materials);
        storage->moved.assign(PixelFieldRuntime::kChunkCellCount, 0u);
        storage->temperature = std::move(load.temperature);
        storage->lifetime = std::move(load.lifetime);
        storage->base_digest = load.base_digest;
        storage->sequence = load.sequence;
        storage->presentation_active = load.presentation_active;
        storage->simulation_active = load.simulation_active;

        std::uint32_t previous = 0u;
        bool first = true;
        storage->delta.reserve(load.delta.size());
        for (const auto& item : load.delta)
        {
            if (item.x >= PixelFieldRuntime::kChunkSizeCells ||
                item.y >= PixelFieldRuntime::kChunkSizeCells)
            {
                return lux::cxx::unexpected(
                    EPixelChunkPreparationError::INVALID_DELTA);
            }
            const auto ordinal = static_cast<std::uint32_t>(item.y) *
                PixelFieldRuntime::kChunkSizeCells + item.x;
            if (!first && ordinal <= previous)
            {
                return lux::cxx::unexpected(
                    EPixelChunkPreparationError::INVALID_DELTA);
            }
            first = false;
            previous = ordinal;
            storage->delta.emplace(
                static_cast<std::uint16_t>(ordinal), item.material);
            storage->cells[ordinal] = item.material;
        }

        for (std::uint32_t y = 0u;
             y < PixelFieldRuntime::kChunkSizeCells;
             ++y)
        {
            for (std::uint32_t x = 0u;
                 x < PixelFieldRuntime::kChunkSizeCells;
                 ++x)
            {
                const auto cell = static_cast<std::size_t>(y) *
                    PixelFieldRuntime::kChunkSizeCells + x;
                if (isBlocking(context, storage->cells[cell]))
                {
                    const auto tile = static_cast<std::size_t>(
                        y / PixelFieldRuntime::kTileSize) *
                        PixelFieldRuntime::kTilesPerChunk +
                        x / PixelFieldRuntime::kTileSize;
                    ++storage->blocking[tile];
                }
            }
        }
        storage->ledger.markDirty(
            0u,
            0u,
            PixelFieldRuntime::kChunkSizeCells,
            PixelFieldRuntime::kChunkSizeCells);
        if (storage->simulation_active)
        {
            storage->active.fill(1u);
            storage->maximum_x.fill(PixelFieldRuntime::kTileSize - 1u);
            storage->maximum_y.fill(PixelFieldRuntime::kTileSize - 1u);
        }
        return PreparedPixelChunk{std::move(storage)};
    }
}
