#include <lux/engine/ecs/tilemap/systems/TilemapRuntime.hpp>
#include <lux/engine/ecs/detail/SparseActiveMap.hpp>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <utility>

namespace lux::ecs
{
    namespace
    {
        [[nodiscard]] std::int64_t floorDivision256(
            std::int64_t value) noexcept
        {
            auto quotient = value / TilemapRuntime::kChunkSizeTiles;
            if (value % TilemapRuntime::kChunkSizeTiles < 0)
                --quotient;
            return quotient;
        }

        [[nodiscard]] bool validChunkCoordinate(
            TileChunkCoord coordinate) noexcept
        {
            constexpr auto edge = static_cast<std::int64_t>(
                TilemapRuntime::kChunkSizeTiles);
            constexpr auto minimum =
                std::numeric_limits<std::int64_t>::min() / edge + 1;
            constexpr auto maximum =
                std::numeric_limits<std::int64_t>::max() / edge - 1;
            return coordinate.x >= minimum && coordinate.x <= maximum &&
                coordinate.y >= minimum && coordinate.y <= maximum;
        }
    } // namespace

    TilemapRuntime::Field::Field()
        : active_chunks(std::make_unique<ActiveChunks>())
    {}

    TilemapRuntime::Field::~Field() = default;
    TilemapRuntime::Field::Field(Field&&) noexcept = default;
    TilemapRuntime::Field& TilemapRuntime::Field::operator=(
        Field&&) noexcept = default;

    TilemapRuntime::~TilemapRuntime() = default;

    TilemapHandle TilemapRuntime::create(const TilemapDesc& description)
    {
        // Persistent identity is optional for ordinary ECS entities. A
        // non-empty domain id remains unique when authored explicitly, while
        // anonymous fields are distinguished by their generation-safe handle.
        if (!description.id.empty())
        {
            for (const auto& field : fields_.values())
                if (field.description.id == description.id)
                    return {};
        }
        const auto handle = fields_.emplace();
        auto& field = fields_.at(handle);
        field.handle = handle;
        field.description = description;
        return handle;
    }

    void TilemapRuntime::destroy(TilemapHandle handle)
    {
        auto* field = resolve(handle);
        if (!field)
            return;
        for (const auto& [_, chunk] : field->chunks)
            resident_bytes_ -= chunkResidentBytes(chunk);
        resident_chunks_ -= field->chunks.size();
        active_chunks_ -= field->active_chunks->size();
        if (!fields_.erase(handle))
            std::abort();
    }

    bool TilemapRuntime::isAlive(TilemapHandle handle) const noexcept
    {
        return resolve(handle) != nullptr;
    }

    TilemapDesc TilemapRuntime::desc(TilemapHandle handle) const noexcept
    {
        const auto* field = resolve(handle);
        return field ? field->description : TilemapDesc{};
    }

    TilemapRuntime::Field* TilemapRuntime::resolve(
        TilemapHandle handle) noexcept
    {
        return fields_.find(handle);
    }

    const TilemapRuntime::Field* TilemapRuntime::resolve(
        TilemapHandle handle) const noexcept
    {
        return const_cast<TilemapRuntime*>(this)->resolve(handle);
    }

    TileChunkCoord TilemapRuntime::chunkOf(
        TileCellCoord coordinate) noexcept
    {
        return {
            floorDivision256(coordinate.x),
            floorDivision256(coordinate.y)};
    }

    std::uint32_t TilemapRuntime::localCoordinate(
        std::int64_t coordinate) noexcept
    {
        auto result = coordinate % kChunkSizeTiles;
        if (result < 0)
            result += kChunkSizeTiles;
        return static_cast<std::uint32_t>(result);
    }

    std::uint16_t TilemapRuntime::ordinal(
        std::uint32_t x,
        std::uint32_t y) noexcept
    {
        return static_cast<std::uint16_t>(y * kChunkSizeTiles + x);
    }

    void TilemapRuntime::growDirty(
        DirtyBounds& dirty,
        std::uint32_t x,
        std::uint32_t y) noexcept
    {
        if (!dirty.valid)
        {
            dirty = {
                static_cast<std::uint16_t>(x),
                static_cast<std::uint16_t>(y),
                static_cast<std::uint16_t>(x),
                static_cast<std::uint16_t>(y),
                true};
            return;
        }
        dirty.minimum_x = static_cast<std::uint16_t>(
            std::min<std::uint32_t>(dirty.minimum_x, x));
        dirty.minimum_y = static_cast<std::uint16_t>(
            std::min<std::uint32_t>(dirty.minimum_y, y));
        dirty.maximum_x = static_cast<std::uint16_t>(
            std::max<std::uint32_t>(dirty.maximum_x, x));
        dirty.maximum_y = static_cast<std::uint16_t>(
            std::max<std::uint32_t>(dirty.maximum_y, y));
    }

    void TilemapRuntime::removeActiveChunk(
        Field& field,
        TileChunkCoord coordinate,
        Chunk& chunk) noexcept
    {
        if (!chunk.active ||
            !field.active_chunks->deactivate(coordinate))
        {
            std::abort();
        }
    }

    std::uint64_t TilemapRuntime::chunkResidentBytes(
        const Chunk& chunk) noexcept
    {
        return chunk.tiles.capacity() * sizeof(std::uint16_t) +
            chunk.delta.size() *
                (sizeof(std::uint16_t) * 2u + sizeof(void*) * 2u);
    }

    bool TilemapRuntime::loadChunk(
        TilemapHandle handle,
        TileChunkLoad&& load)
    {
        auto* field = resolve(handle);
        if (!field || !validChunkCoordinate(load.coordinate) ||
            load.tiles.size() != kChunkTileCount ||
            field->chunks.contains(load.coordinate))
        {
            return false;
        }
        std::sort(
            load.delta.begin(),
            load.delta.end(),
            [](const auto& left, const auto& right)
            {
                return ordinal(left.x, left.y) < ordinal(right.x, right.y);
            });
        std::uint16_t previous = 0u;
        bool first = true;
        for (const auto& edit : load.delta)
        {
            if (edit.x >= kChunkSizeTiles || edit.y >= kChunkSizeTiles)
                return false;
            const auto cell = ordinal(edit.x, edit.y);
            if (!first && cell == previous)
                return false;
            first = false;
            previous = cell;
        }
        Chunk chunk;
        chunk.tiles = std::move(load.tiles);
        chunk.base_digest = load.base_digest;
        chunk.sequence = load.sequence;
        chunk.active = load.active;
        for (const auto& edit : load.delta)
        {
            const auto cell = ordinal(edit.x, edit.y);
            chunk.tiles[cell] = edit.tile;
            chunk.delta[cell] = edit.tile;
        }
        const auto [iterator, inserted] = field->chunks.emplace(
            load.coordinate, std::move(chunk));
        if (!inserted)
            std::abort();
        if (!field->active_chunks->track(
                load.coordinate,
                iterator->second.active))
        {
            std::abort();
        }
        if (iterator->second.active)
            ++active_chunks_;
        resident_bytes_ += chunkResidentBytes(iterator->second);
        ++resident_chunks_;
        return true;
    }

    bool TilemapRuntime::captureChunk(
        TilemapHandle handle,
        TileChunkCoord coordinate,
        TileChunkSnapshot& output) const
    {
        const auto* field = resolve(handle);
        if (!field)
            return false;
        const auto found = field->chunks.find(coordinate);
        if (found == field->chunks.end())
            return false;
        const auto& chunk = found->second;
        output.coordinate = coordinate;
        output.tiles = chunk.tiles;
        output.base_digest = chunk.base_digest;
        output.sequence = chunk.sequence;
        output.active = chunk.active;
        output.delta.clear();
        output.delta.reserve(chunk.delta.size());
        for (const auto& [cell, tile] : chunk.delta)
        {
            output.delta.push_back({
                static_cast<std::uint16_t>(cell % kChunkSizeTiles),
                static_cast<std::uint16_t>(cell / kChunkSizeTiles),
                tile});
        }
        std::sort(
            output.delta.begin(),
            output.delta.end(),
            [](const auto& left, const auto& right)
            {
                return ordinal(left.x, left.y) < ordinal(right.x, right.y);
            });
        return true;
    }

    bool TilemapRuntime::unloadChunk(
        TilemapHandle handle,
        TileChunkCoord coordinate,
        TileChunkSnapshot& output)
    {
        auto* field = resolve(handle);
        if (!field || !captureChunk(handle, coordinate, output))
            return false;
        auto found = field->chunks.find(coordinate);
        if (found == field->chunks.end())
            std::abort();
        resident_bytes_ -= chunkResidentBytes(found->second);
        --resident_chunks_;
        if (found->second.active)
            --active_chunks_;
        if (!field->active_chunks->untrack(coordinate))
            std::abort();
        field->chunks.erase(coordinate);
        return true;
    }

    bool TilemapRuntime::discardChunk(
        TilemapHandle handle,
        TileChunkCoord coordinate) noexcept
    {
        auto* field = resolve(handle);
        if (!field)
            return false;
        const auto found = field->chunks.find(coordinate);
        if (found == field->chunks.end())
            return false;
        resident_bytes_ -= chunkResidentBytes(found->second);
        --resident_chunks_;
        if (found->second.active)
            --active_chunks_;
        if (!field->active_chunks->untrack(coordinate))
            std::abort();
        field->chunks.erase(found);
        return true;
    }

    bool TilemapRuntime::chunkResident(
        TilemapHandle handle,
        TileChunkCoord coordinate) const noexcept
    {
        const auto* field = resolve(handle);
        return field && field->chunks.contains(coordinate);
    }

    void TilemapRuntime::residentChunks(
        TilemapHandle handle,
        std::vector<TileChunkCoord>& output) const
    {
        output.clear();
        const auto* field = resolve(handle);
        if (!field)
            return;
        output.reserve(field->chunks.size());
        for (const auto& [coordinate, chunk] : field->chunks)
            output.push_back(coordinate);
        std::sort(output.begin(), output.end());
    }

    std::span<const TileChunkCoord> TilemapRuntime::activeKeys(
        TilemapHandle handle) const noexcept
    {
        const auto* field = resolve(handle);
        const auto result = field ? field->active_chunks->keys()
                                  : std::span<const TileChunkCoord>{};
        active_enumeration_chunks_visited_last_ =
            result.size();
        return result;
    }

    bool TilemapRuntime::chunkActive(
        TilemapHandle handle,
        TileChunkCoord coordinate) const noexcept
    {
        const auto* field = resolve(handle);
        return field && field->active_chunks->contains(coordinate);
    }

    bool TilemapRuntime::setChunkActive(
        TilemapHandle handle,
        TileChunkCoord coordinate,
        bool active) noexcept
    {
        auto* field = resolve(handle);
        if (!field)
            return false;
        const auto found = field->chunks.find(coordinate);
        if (found == field->chunks.end())
            return false;
        auto& chunk = found->second;
        if (chunk.active == active)
            return true;
        if (active)
        {
            if (!field->active_chunks->activate(coordinate))
                std::abort();
            chunk.active = true;
            ++active_chunks_;
        }
        else
        {
            removeActiveChunk(*field, coordinate, chunk);
            chunk.active = false;
            --active_chunks_;
        }
        return true;
    }

    std::uint16_t TilemapRuntime::tileAt(
        TilemapHandle handle,
        TileCellCoord coordinate) const noexcept
    {
        const auto* field = resolve(handle);
        if (!field)
            return lux::rdesc::kEmptyTile;
        const auto found = field->chunks.find(chunkOf(coordinate));
        if (found == field->chunks.end())
            return lux::rdesc::kEmptyTile;
        return found->second.tiles[ordinal(
            localCoordinate(coordinate.x),
            localCoordinate(coordinate.y))];
    }

    bool TilemapRuntime::setTile(
        TilemapHandle handle,
        TileCellCoord coordinate,
        std::uint16_t tile) noexcept
    {
        auto* field = resolve(handle);
        if (!field)
            return false;
        const auto found = field->chunks.find(chunkOf(coordinate));
        if (found == field->chunks.end())
            return false;
        const auto x = localCoordinate(coordinate.x);
        const auto y = localCoordinate(coordinate.y);
        const auto cell = ordinal(x, y);
        auto& chunk = found->second;
        if (chunk.tiles[cell] == tile)
            return true;
        chunk.tiles[cell] = tile;
        if (!chunk.delta.contains(cell))
        {
            resident_bytes_ +=
                sizeof(std::uint16_t) * 2u + sizeof(void*) * 2u;
        }
        chunk.delta[cell] = tile;
        ++chunk.sequence;
        ++chunk.revision;
        if (chunk.revision == 0u)
            ++chunk.revision;
        growDirty(chunk.dirty, x, y);
        return true;
    }

    bool TilemapRuntime::markChunkDirty(
        TilemapHandle handle,
        TileChunkCoord coordinate) noexcept
    {
        auto* field = resolve(handle);
        if (!field)
            return false;
        const auto found = field->chunks.find(coordinate);
        if (found == field->chunks.end())
            return false;
        auto& chunk = found->second;
        ++chunk.revision;
        if (chunk.revision == 0u)
            ++chunk.revision;
        chunk.dirty = {0u, 0u, 255u, 255u, true};
        return true;
    }

    bool TilemapRuntime::exportDirty(
        TilemapHandle handle,
        TileChunkCoord coordinate,
        TileChunkRenderExport& output)
    {
        output = {};
        auto* field = resolve(handle);
        if (!field)
            return false;
        const auto found = field->chunks.find(coordinate);
        if (found == field->chunks.end())
            return false;
        auto& chunk = found->second;
        if (!chunk.dirty.valid || chunk.inflight_revision != 0u)
            return true;
        const auto width = static_cast<std::uint32_t>(
            chunk.dirty.maximum_x - chunk.dirty.minimum_x + 1u);
        const auto height = static_cast<std::uint32_t>(
            chunk.dirty.maximum_y - chunk.dirty.minimum_y + 1u);
        const auto bytes = static_cast<std::uint64_t>(width) * height * 2u;
        auto pixels = std::shared_ptr<std::byte[]>(new std::byte[bytes]);
        auto* destination = reinterpret_cast<std::uint16_t*>(pixels.get());
        for (std::uint32_t row = 0u; row < height; ++row)
        {
            const auto source = static_cast<std::size_t>(
                chunk.dirty.minimum_y + row) * kChunkSizeTiles +
                chunk.dirty.minimum_x;
            std::memcpy(
                destination + static_cast<std::size_t>(row) * width,
                chunk.tiles.data() + source,
                static_cast<std::size_t>(width) * 2u);
        }
        chunk.inflight_revision = chunk.revision;
        output.rect = {
            chunk.dirty.minimum_x,
            chunk.dirty.minimum_y,
            width,
            height};
        output.pixels = std::move(pixels);
        output.pixel_bytes = bytes;
        output.content_revision = chunk.revision;
        return true;
    }

    void TilemapRuntime::confirmExport(
        TilemapHandle handle,
        TileChunkCoord coordinate,
        std::uint64_t revision,
        bool uploaded) noexcept
    {
        auto* field = resolve(handle);
        if (!field)
            return;
        const auto found = field->chunks.find(coordinate);
        if (found == field->chunks.end())
            return;
        auto& chunk = found->second;
        if (chunk.inflight_revision != revision)
            return;
        chunk.inflight_revision = 0u;
        if (uploaded && chunk.revision == revision)
            chunk.dirty = {};
    }

    TilemapRuntimeStats TilemapRuntime::stats() const noexcept
    {
        return {
            resident_chunks_,
            active_chunks_,
            resident_bytes_,
            active_enumeration_chunks_visited_last_};
    }
} // namespace lux::ecs
