#include <lux/engine/simulation/pixel/PixelFieldRuntime.hpp>

#include <algorithm>
#include <array>
#include <limits>
#include <new>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lux::simulation
{
    namespace
    {
        constexpr std::uint32_t kChunkShift = 8U;
        constexpr std::uint32_t kChunkSize = 1U << kChunkShift;
        constexpr std::uint32_t kChunkMask = kChunkSize - 1U;
        constexpr std::uint32_t kTileShift = 5U;
        constexpr std::uint32_t kTileSize = 1U << kTileShift;
        constexpr std::uint32_t kTilesPerAxis = kChunkSize / kTileSize;
        constexpr std::uint32_t kTileCount = kTilesPerAxis * kTilesPerAxis;
        constexpr std::size_t kChunkCellCount =
            static_cast<std::size_t>(kChunkSize) * kChunkSize;

        static_assert(kChunkSize == 256U);
        static_assert(kTileSize == 32U);

        [[nodiscard]] constexpr std::uint64_t chunkKey(
            std::uint32_t x,
            std::uint32_t y
        ) noexcept
        {
            return (static_cast<std::uint64_t>(y) << 32U) | x;
        }

        [[nodiscard]] constexpr std::uint32_t chunkX(std::uint64_t key) noexcept
        {
            return static_cast<std::uint32_t>(key);
        }

        [[nodiscard]] constexpr std::uint32_t chunkY(std::uint64_t key) noexcept
        {
            return static_cast<std::uint32_t>(key >> 32U);
        }

        [[nodiscard]] constexpr std::size_t cellIndex(
            std::uint32_t x,
            std::uint32_t y
        ) noexcept
        {
            return static_cast<std::size_t>(y) * kChunkSize + x;
        }

        [[nodiscard]] constexpr std::uint32_t tileIndex(
            std::uint32_t x,
            std::uint32_t y
        ) noexcept
        {
            return (y >> kTileShift) * kTilesPerAxis + (x >> kTileShift);
        }

        template <class T>
        void fnvAppend(std::uint64_t& hash, T value) noexcept
        {
            for (std::size_t index = 0U; index < sizeof(T); ++index)
            {
                const auto byte = static_cast<std::uint8_t>(
                    static_cast<std::uint64_t>(value) >> (index * 8U));
                hash ^= byte;
                hash *= 1099511628211ULL;
            }
        }
    } // namespace

    struct PixelFieldRuntime::Impl final
    {
        struct Chunk final
        {
            Chunk()
                : cells(kChunkCellCount, kEmptyPixelMaterial),
                  moved(kChunkCellCount, 0U)
            {
            }

            std::vector<PixelMaterialId> cells;
            std::vector<std::uint8_t> moved;
            std::array<std::uint8_t, kTileCount> active_tiles{};
            std::array<std::uint8_t, kTileCount> next_tiles{};
            bool current_listed{};
            bool next_listed{};
        };

        explicit Impl(PixelFieldConfiguration value)
            : configuration(value)
        {
            materials.push_back({});
        }

        [[nodiscard]] bool valid(std::int64_t x, std::int64_t y) const noexcept
        {
            return x >= 0 && y >= 0 &&
                static_cast<std::uint64_t>(x) < configuration.width &&
                static_cast<std::uint64_t>(y) < configuration.height;
        }

        [[nodiscard]] std::uint64_t keyFor(std::int64_t x, std::int64_t y) const noexcept
        {
            return chunkKey(
                static_cast<std::uint32_t>(x) >> kChunkShift,
                static_cast<std::uint32_t>(y) >> kChunkShift);
        }

        [[nodiscard]] Chunk* findChunk(std::uint64_t key) noexcept
        {
            const auto found = chunks.find(key);
            return found == chunks.end() ? nullptr : found->second.get();
        }

        [[nodiscard]] const Chunk* findChunk(std::uint64_t key) const noexcept
        {
            const auto found = chunks.find(key);
            return found == chunks.end() ? nullptr : found->second.get();
        }

        [[nodiscard]] lux::cxx::expected<Chunk*, EPixelFieldError> ensureChunk(
            std::uint64_t key
        ) noexcept
        {
            if (auto* existing = findChunk(key))
                return existing;

            try
            {
                const auto required = chunks.size() + 1U;
                current_active.reserve(required);
                next_active.reserve(required);
                resident_keys.reserve(required);
                auto chunk = std::make_unique<Chunk>();
                auto* result = chunk.get();
                chunks.emplace(key, std::move(chunk));
                resident_keys.insert(
                    std::lower_bound(resident_keys.begin(), resident_keys.end(), key),
                    key);
                return result;
            }
            catch (const std::bad_alloc&)
            {
                return lux::cxx::unexpected(EPixelFieldError::ALLOCATION_FAILURE);
            }
        }

        void scheduleCurrent(
            std::uint64_t key,
            Chunk& chunk,
            std::uint32_t local_x,
            std::uint32_t local_y
        ) noexcept
        {
            chunk.active_tiles[tileIndex(local_x, local_y)] = 1U;
            if (!chunk.current_listed)
            {
                chunk.current_listed = true;
                current_active.push_back(key);
            }
        }

        void scheduleNext(
            std::uint64_t key,
            Chunk& chunk,
            std::uint32_t local_x,
            std::uint32_t local_y
        ) noexcept
        {
            chunk.next_tiles[tileIndex(local_x, local_y)] = 1U;
            if (!chunk.next_listed)
            {
                chunk.next_listed = true;
                next_active.push_back(key);
            }
        }

        [[nodiscard]] PixelMaterialId cellUnchecked(
            std::int64_t x,
            std::int64_t y
        ) const noexcept
        {
            const auto* chunk = findChunk(keyFor(x, y));
            if (!chunk)
                return kEmptyPixelMaterial;
            return chunk->cells[cellIndex(
                static_cast<std::uint32_t>(x) & kChunkMask,
                static_cast<std::uint32_t>(y) & kChunkMask)];
        }

        [[nodiscard]] bool resident(std::int64_t x, std::int64_t y) const noexcept
        {
            return valid(x, y) && findChunk(keyFor(x, y)) != nullptr;
        }

        [[nodiscard]] bool tryMove(
            std::int64_t source_x,
            std::int64_t source_y,
            std::int64_t target_x,
            std::int64_t target_y,
            PixelMaterialId source_material
        ) noexcept
        {
            if (!resident(target_x, target_y))
                return false;

            const auto target_key = keyFor(target_x, target_y);
            auto* target_chunk = findChunk(target_key);
            const auto target_local_x = static_cast<std::uint32_t>(target_x) & kChunkMask;
            const auto target_local_y = static_cast<std::uint32_t>(target_y) & kChunkMask;
            const auto target_index = cellIndex(target_local_x, target_local_y);
            if (target_chunk->cells[target_index] != kEmptyPixelMaterial)
                return false;

            const auto source_key = keyFor(source_x, source_y);
            auto* source_chunk = findChunk(source_key);
            const auto source_local_x = static_cast<std::uint32_t>(source_x) & kChunkMask;
            const auto source_local_y = static_cast<std::uint32_t>(source_y) & kChunkMask;
            const auto source_index = cellIndex(source_local_x, source_local_y);

            source_chunk->cells[source_index] = kEmptyPixelMaterial;
            target_chunk->cells[target_index] = source_material;
            target_chunk->moved[target_index] = 1U;
            scheduleNext(source_key, *source_chunk, source_local_x, source_local_y);
            scheduleNext(target_key, *target_chunk, target_local_x, target_local_y);
            ++moved_cells_last;
            return true;
        }

        void clearMovedForActiveTiles(Chunk& chunk) noexcept
        {
            for (std::uint32_t tile = 0U; tile < kTileCount; ++tile)
            {
                if (chunk.active_tiles[tile] == 0U)
                    continue;
                const auto tile_x = (tile % kTilesPerAxis) * kTileSize;
                const auto tile_y = (tile / kTilesPerAxis) * kTileSize;
                for (std::uint32_t y = tile_y; y < tile_y + kTileSize; ++y)
                {
                    std::fill_n(
                        chunk.moved.begin() + static_cast<std::ptrdiff_t>(cellIndex(tile_x, y)),
                        kTileSize,
                        std::uint8_t{0U});
                }
            }
        }

        void stepChunk(std::uint64_t key, Chunk& chunk) noexcept
        {
            const auto base_x = static_cast<std::int64_t>(chunkX(key)) * kChunkSize;
            const auto base_y = static_cast<std::int64_t>(chunkY(key)) * kChunkSize;

            for (std::int32_t tile_y = static_cast<std::int32_t>(kTilesPerAxis) - 1;
                 tile_y >= 0;
                 --tile_y)
            {
                const bool reverse_tiles =
                    ((step_index + static_cast<std::uint64_t>(tile_y)) & 1ULL) != 0ULL;
                for (std::uint32_t tile_offset = 0U; tile_offset < kTilesPerAxis; ++tile_offset)
                {
                    const auto tile_x = reverse_tiles
                        ? (kTilesPerAxis - 1U - tile_offset)
                        : tile_offset;
                    const auto tile =
                        static_cast<std::uint32_t>(tile_y) * kTilesPerAxis + tile_x;
                    if (chunk.active_tiles[tile] == 0U)
                        continue;

                    const auto minimum_x = tile_x * kTileSize;
                    const auto minimum_y = static_cast<std::uint32_t>(tile_y) * kTileSize;
                    for (std::int32_t local_y =
                             static_cast<std::int32_t>(minimum_y + kTileSize) - 1;
                         local_y >= static_cast<std::int32_t>(minimum_y);
                         --local_y)
                    {
                        const bool reverse_x =
                            ((step_index + static_cast<std::uint64_t>(base_y + local_y)) & 1ULL) != 0ULL;
                        for (std::uint32_t offset = 0U; offset < kTileSize; ++offset)
                        {
                            const auto local_x = reverse_x
                                ? (minimum_x + kTileSize - 1U - offset)
                                : (minimum_x + offset);
                            const auto index =
                                cellIndex(local_x, static_cast<std::uint32_t>(local_y));
                            ++cells_scanned_last;
                            if (chunk.moved[index] != 0U)
                                continue;

                            const auto id = chunk.cells[index];
                            if (id == kEmptyPixelMaterial || id >= materials.size())
                                continue;
                            const auto phase = materials[id].phase;
                            if (phase != EPixelMaterialPhase::POWDER &&
                                phase != EPixelMaterialPhase::LIQUID)
                            {
                                continue;
                            }

                            const auto x = base_x + local_x;
                            const auto y = base_y + local_y;
                            if (tryMove(x, y, x, y + 1, id))
                                continue;

                            const auto first = reverse_x ? 1 : -1;
                            const auto second = -first;
                            if (phase == EPixelMaterialPhase::POWDER)
                            {
                                if (tryMove(x, y, x + first, y + 1, id) ||
                                    tryMove(x, y, x + second, y + 1, id))
                                {
                                    continue;
                                }
                            }
                            else if (tryMove(x, y, x + first, y, id) ||
                                     tryMove(x, y, x + second, y, id))
                            {
                                continue;
                            }
                        }
                    }
                }
            }
        }

        PixelFieldConfiguration configuration;
        std::vector<PixelMaterialDefinition> materials;
        std::unordered_map<std::uint64_t, std::unique_ptr<Chunk>> chunks;
        std::vector<std::uint64_t> resident_keys;
        std::vector<std::uint64_t> current_active;
        std::vector<std::uint64_t> next_active;
        std::uint64_t step_index{};
        std::uint64_t cells_scanned_last{};
        std::uint64_t moved_cells_last{};
        bool stepping{};
    };

    PixelFieldRuntime::PixelFieldRuntime(PixelFieldConfiguration configuration)
        : impl_(std::make_unique<Impl>(configuration))
    {
    }

    PixelFieldRuntime::~PixelFieldRuntime() noexcept = default;
    PixelFieldRuntime::PixelFieldRuntime(PixelFieldRuntime&&) noexcept = default;
    PixelFieldRuntime& PixelFieldRuntime::operator=(PixelFieldRuntime&&) noexcept = default;

    lux::cxx::expected<std::unique_ptr<PixelFieldRuntime>, EPixelFieldError>
    PixelFieldRuntime::create(PixelFieldConfiguration configuration) noexcept
    {
        if (configuration.width == 0U || configuration.height == 0U)
            return lux::cxx::unexpected(EPixelFieldError::INVALID_CONFIGURATION);
        try
        {
            return std::unique_ptr<PixelFieldRuntime>(new PixelFieldRuntime(configuration));
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(EPixelFieldError::ALLOCATION_FAILURE);
        }
    }

    PixelFieldConfiguration PixelFieldRuntime::configuration() const noexcept
    {
        return impl_->configuration;
    }

    lux::cxx::expected<PixelMaterialId, EPixelFieldError>
    PixelFieldRuntime::addMaterial(PixelMaterialDefinition definition) noexcept
    {
        if (impl_->materials.size() > std::numeric_limits<PixelMaterialId>::max())
            return lux::cxx::unexpected(EPixelFieldError::MATERIAL_CAPACITY_EXCEEDED);
        try
        {
            impl_->materials.push_back(definition);
            return static_cast<PixelMaterialId>(impl_->materials.size() - 1U);
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(EPixelFieldError::ALLOCATION_FAILURE);
        }
    }

    const PixelMaterialDefinition& PixelFieldRuntime::material(PixelMaterialId id) const noexcept
    {
        return id < impl_->materials.size() ? impl_->materials[id] : impl_->materials.front();
    }

    lux::cxx::expected<void, EPixelFieldError> PixelFieldRuntime::setCell(
        std::int64_t x,
        std::int64_t y,
        PixelMaterialId material_id
    ) noexcept
    {
        if (!impl_->valid(x, y))
            return lux::cxx::unexpected(EPixelFieldError::OUT_OF_BOUNDS);
        if (material_id >= impl_->materials.size())
            return lux::cxx::unexpected(EPixelFieldError::INVALID_MATERIAL);

        const auto key = impl_->keyFor(x, y);
        auto chunk_result = impl_->ensureChunk(key);
        if (!chunk_result)
            return lux::cxx::unexpected(chunk_result.error());

        auto* chunk = *chunk_result;
        const auto local_x = static_cast<std::uint32_t>(x) & kChunkMask;
        const auto local_y = static_cast<std::uint32_t>(y) & kChunkMask;
        const auto index = cellIndex(local_x, local_y);
        if (chunk->cells[index] == material_id)
            return {};

        chunk->cells[index] = material_id;
        impl_->scheduleCurrent(key, *chunk, local_x, local_y);
        return {};
    }

    lux::cxx::expected<PixelMaterialId, EPixelFieldError> PixelFieldRuntime::cell(
        std::int64_t x,
        std::int64_t y
    ) const noexcept
    {
        if (!impl_->valid(x, y))
            return lux::cxx::unexpected(EPixelFieldError::OUT_OF_BOUNDS);
        return impl_->cellUnchecked(x, y);
    }

    void PixelFieldRuntime::step() noexcept
    {
        impl_->cells_scanned_last = 0U;
        impl_->moved_cells_last = 0U;
        if (impl_->current_active.empty())
        {
            ++impl_->step_index;
            return;
        }

        std::sort(impl_->current_active.begin(), impl_->current_active.end());
        impl_->stepping = true;

        for (const auto key : impl_->current_active)
        {
            if (auto* chunk = impl_->findChunk(key))
                impl_->clearMovedForActiveTiles(*chunk);
        }

        for (const auto key : impl_->current_active)
        {
            auto* chunk = impl_->findChunk(key);
            if (!chunk)
                continue;
            chunk->current_listed = false;
            impl_->stepChunk(key, *chunk);
            chunk->active_tiles.fill(0U);
        }

        impl_->stepping = false;
        impl_->current_active.clear();
        impl_->current_active.swap(impl_->next_active);

        for (const auto key : impl_->current_active)
        {
            auto* chunk = impl_->findChunk(key);
            if (!chunk)
                continue;
            chunk->active_tiles = chunk->next_tiles;
            chunk->next_tiles.fill(0U);
            chunk->current_listed = true;
            chunk->next_listed = false;
        }
        impl_->next_active.clear();
        ++impl_->step_index;
    }

    std::uint64_t PixelFieldRuntime::determinismHash() const noexcept
    {
        std::uint64_t hash = 1469598103934665603ULL;
        fnvAppend(hash, impl_->configuration.width);
        fnvAppend(hash, impl_->configuration.height);
        fnvAppend(hash, static_cast<std::uint64_t>(impl_->materials.size()));
        for (const auto& definition : impl_->materials)
        {
            fnvAppend(hash, static_cast<std::uint8_t>(definition.phase));
            fnvAppend(hash, definition.density);
            fnvAppend(hash, definition.rgba8);
        }
        for (const auto key : impl_->resident_keys)
        {
            fnvAppend(hash, key);
            const auto* chunk = impl_->findChunk(key);
            for (const auto value : chunk->cells)
                fnvAppend(hash, value);
        }
        return hash;
    }

    std::size_t PixelFieldRuntime::residentChunkCount() const noexcept
    {
        return impl_->chunks.size();
    }

    std::size_t PixelFieldRuntime::activeChunkCount() const noexcept
    {
        return impl_->current_active.size();
    }

    std::uint64_t PixelFieldRuntime::cellsScannedLastStep() const noexcept
    {
        return impl_->cells_scanned_last;
    }

    std::uint64_t PixelFieldRuntime::movedCellsLastStep() const noexcept
    {
        return impl_->moved_cells_last;
    }
} // namespace lux::simulation
