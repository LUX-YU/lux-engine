#include <lux/engine/ecs/pixel/systems/PixelFieldRuntime.hpp>

#include <lux/engine/ecs/detail/SparseActiveMap.hpp>
#include <lux/engine/ecs/pixel/PreparedPixelChunkStorage.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <thread>
#include <utility>

#include <tbb/blocked_range.h>
#include <tbb/parallel_for.h>
#include <tbb/task_arena.h>

namespace lux::ecs
{
    namespace
    {
        constexpr std::int64_t kLiquidDispersion = 8;
        constexpr std::int64_t kMaximumFallCells = 4;
        constexpr std::uint64_t kFnvBasis = 1469598103934665603ull;
        constexpr std::uint64_t kFnvPrime = 1099511628211ull;
        constexpr std::uint64_t kMaximumCommandCells =
            16ull * PixelFieldRuntime::kChunkCellCount;

        [[nodiscard]] std::uint64_t fnv1a64Append(
            std::uint64_t hash,
            const void* data,
            std::size_t bytes) noexcept
        {
            const auto* cursor = static_cast<const std::uint8_t*>(data);
            for (std::size_t index = 0u; index < bytes; ++index)
            {
                hash ^= cursor[index];
                hash *= kFnvPrime;
            }
            return hash;
        }

        [[nodiscard]] bool isBlocking(
            const PixelMaterialRegistry& materials,
            MaterialId material) noexcept
        {
            const auto phase = materials.at(material).phase;
            return phase == EMaterialPhase::SOLID ||
                phase == EMaterialPhase::POWDER;
        }

        [[nodiscard]] std::int64_t floorDivision256(
            std::int64_t value) noexcept
        {
            auto quotient = value / PixelFieldRuntime::kChunkSizeCells;
            if (value % PixelFieldRuntime::kChunkSizeCells < 0)
                --quotient;
            return quotient;
        }

        [[nodiscard]] bool checkedAdd(
            std::int64_t value,
            std::uint32_t amount,
            std::int64_t& result) noexcept
        {
            if (value > std::numeric_limits<std::int64_t>::max() -
                    static_cast<std::int64_t>(amount))
            {
                return false;
            }
            result = value + static_cast<std::int64_t>(amount);
            return true;
        }

        [[nodiscard]] constexpr std::int64_t saturatingAdd(
            std::int64_t value,
            std::int64_t amount) noexcept
        {
            if (amount > 0 &&
                value > std::numeric_limits<std::int64_t>::max() - amount)
            {
                return std::numeric_limits<std::int64_t>::max();
            }
            if (amount < 0 &&
                value < std::numeric_limits<std::int64_t>::min() - amount)
            {
                return std::numeric_limits<std::int64_t>::min();
            }
            return value + amount;
        }

        [[nodiscard]] bool chunkBase(
            PixelChunkCoord coordinate,
            PixelCellCoord& result) noexcept
        {
            constexpr auto edge = static_cast<std::int64_t>(
                PixelFieldRuntime::kChunkSizeCells);
            // Simulation probes neighbours and wake halos around a chunk.
            // Reserve one complete chunk at both integer extremes so those
            // additions stay defined as well as coordinate*edge itself.
            constexpr auto minimum_chunk =
                std::numeric_limits<std::int64_t>::min() / edge + 1;
            constexpr auto maximum_chunk =
                std::numeric_limits<std::int64_t>::max() / edge - 1;
            if (coordinate.x < minimum_chunk || coordinate.x > maximum_chunk ||
                coordinate.y < minimum_chunk || coordinate.y > maximum_chunk)
            {
                return false;
            }
            result = {coordinate.x * edge, coordinate.y * edge};
            return true;
        }

        [[nodiscard]] std::uint32_t coordinateParity(
            std::int64_t coordinate) noexcept
        {
            auto result = coordinate % 2;
            if (result < 0)
                result += 2;
            return static_cast<std::uint32_t>(result);
        }
    } // namespace

    struct PixelFieldRuntime::StepArena final
    {
        struct WorkItem final
        {
            PixelChunkCoord coordinate{};
            Chunk* chunk{};
            std::uint32_t moved{0u};
            std::uint32_t scanned{0u};
        };

        explicit StepArena(std::uint32_t parallelism)
            : arena(
                  static_cast<int>(parallelism),
                  1u,
                  tbb::task_arena::priority::high)
        {}

        tbb::task_arena arena;
        std::vector<WorkItem> work_items;
        std::uint64_t scratch_growth_count{0u};
    };

    PixelFieldRuntime::Field::Field()
        : active_chunks(std::make_unique<ActiveChunks>()),
          presentation_chunks(std::make_unique<ActiveChunks>())
    {}

    PixelFieldRuntime::Field::~Field() = default;
    PixelFieldRuntime::Field::Field(Field&&) noexcept = default;
    PixelFieldRuntime::Field& PixelFieldRuntime::Field::operator=(
        Field&&) noexcept = default;

    bool PixelFieldRuntime::Chunk::hasActiveTiles() const noexcept
    {
        return std::ranges::any_of(active, [](std::uint8_t value)
        {
            return value != 0u;
        });
    }

    PixelFieldRuntime::PixelFieldRuntime(PixelFieldRuntimeConfig config)
    {
        const auto hardware_value = std::thread::hardware_concurrency();
        const auto hardware = hardware_value == 0u ? 4u : hardware_value;
        parallelism_ = config.parallelism == 0u
            ? std::clamp(hardware > 2u ? hardware - 2u : 1u, 1u, 16u)
            : std::clamp(config.parallelism, 1u, 16u);
        parallel_ = std::make_unique<StepArena>(parallelism_);
    }

    PixelFieldRuntime::~PixelFieldRuntime() = default;

    PixelFieldHandle PixelFieldRuntime::create(const PixelFieldDesc& desc)
    {
        if (static_cast<std::uint8_t>(desc.extent) >
                static_cast<std::uint8_t>(EPixelFieldExtent::INFINITE_FIELD) ||
            (desc.extent == EPixelFieldExtent::BOUNDED &&
             !desc.bounds.valid()))
        {
            return {};
        }

        const auto handle = fields_.emplace();
        auto& field = fields_.at(handle);
        field.handle = handle;
        field.desc = desc;
        field.simulation_dense_index = simulation_fields_.size();
        simulation_fields_.push_back(handle);
        return handle;
    }

    void PixelFieldRuntime::destroy(PixelFieldHandle handle)
    {
        if (resolve(handle))
            destroySlot(handle);
    }

    bool PixelFieldRuntime::isAlive(PixelFieldHandle handle) const noexcept
    {
        return resolve(handle) != nullptr;
    }

    PixelFieldDesc PixelFieldRuntime::desc(
        PixelFieldHandle handle) const noexcept
    {
        const auto* field = resolve(handle);
        return field ? field->desc : PixelFieldDesc{};
    }

    PixelFieldRuntime::Field* PixelFieldRuntime::resolve(
        PixelFieldHandle handle) noexcept
    {
        return fields_.find(handle);
    }

    const PixelFieldRuntime::Field* PixelFieldRuntime::resolve(
        PixelFieldHandle handle) const noexcept
    {
        return const_cast<PixelFieldRuntime*>(this)->resolve(handle);
    }

    void PixelFieldRuntime::pushEvent(const PixelFieldEvent& event)
    {
        if (events_.size() >= kMaxPendingEvents)
        {
            constexpr auto drop = kMaxPendingEvents / 2u;
            events_.erase(
                events_.begin(),
                events_.begin() + static_cast<std::ptrdiff_t>(drop));
            events_dropped_ += drop;
        }
        events_.push_back(event);
    }

    void PixelFieldRuntime::destroySlot(PixelFieldHandle handle)
    {
        auto* field = fields_.find(handle);
        if (!field)
            return;
        setFieldSimulationEnabled(*field, false);
        pushEvent({
            PixelFieldEvent::EKind::FIELD_DESTROYED,
            handle,
            {},
            0u});
        if (!fields_.erase(handle))
            std::abort();
    }

    void PixelFieldRuntime::setFieldSimulationEnabled(
        Field& field,
        bool enabled) noexcept
    {
        constexpr auto kNoDenseIndex =
            std::numeric_limits<std::size_t>::max();
        if (field.simulation_enabled == enabled &&
            ((enabled && field.simulation_dense_index != kNoDenseIndex) ||
             (!enabled && field.simulation_dense_index == kNoDenseIndex)))
        {
            return;
        }
        if (enabled)
        {
            if (field.simulation_dense_index != kNoDenseIndex)
                std::abort();
            field.simulation_dense_index = simulation_fields_.size();
            simulation_fields_.push_back(field.handle);
            field.simulation_enabled = true;
            return;
        }

        const auto index = field.simulation_dense_index;
        if (index == kNoDenseIndex || index >= simulation_fields_.size() ||
            simulation_fields_[index] != field.handle)
        {
            std::abort();
        }
        const auto moved = simulation_fields_.back();
        simulation_fields_[index] = moved;
        simulation_fields_.pop_back();
        if (moved != field.handle)
        {
            auto* moved_field = resolve(moved);
            if (!moved_field)
                std::abort();
            moved_field->simulation_dense_index = index;
        }
        field.simulation_dense_index = kNoDenseIndex;
        field.simulation_enabled = false;
        field.chunks_visited_last = 0u;
        field.cells_scanned_last = 0u;
        field.moved_cells_last = 0u;
        field.step_ms_last = 0.0;
    }

    bool PixelFieldRuntime::coordinateAllowed(
        const Field& field,
        PixelChunkCoord coordinate) noexcept
    {
        return field.desc.extent == EPixelFieldExtent::INFINITE_FIELD ||
            field.desc.bounds.contains(coordinate);
    }

    PixelChunkCoord PixelFieldRuntime::chunkOf(
        PixelCellCoord coordinate) noexcept
    {
        return {
            floorDivision256(coordinate.x),
            floorDivision256(coordinate.y)};
    }

    std::uint32_t PixelFieldRuntime::localCoordinate(
        std::int64_t coordinate) noexcept
    {
        auto result = coordinate %
            static_cast<std::int64_t>(kChunkSizeCells);
        if (result < 0)
            result += kChunkSizeCells;
        return static_cast<std::uint32_t>(result);
    }

    std::uint16_t PixelFieldRuntime::cellOrdinal(
        std::uint32_t x,
        std::uint32_t y) noexcept
    {
        return static_cast<std::uint16_t>(y * kChunkSizeCells + x);
    }

    std::size_t PixelFieldRuntime::tileOrdinal(
        std::uint32_t x,
        std::uint32_t y) noexcept
    {
        return static_cast<std::size_t>(y / kTileSize) * kTilesPerChunk +
            x / kTileSize;
    }

    PixelFieldRuntime::Chunk* PixelFieldRuntime::chunkAt(
        Field& field,
        PixelChunkCoord coordinate) noexcept
    {
        const auto found = field.chunks.find(coordinate);
        return found == field.chunks.end() ? nullptr : &found->second;
    }

    const PixelFieldRuntime::Chunk* PixelFieldRuntime::chunkAt(
        const Field& field,
        PixelChunkCoord coordinate) const noexcept
    {
        const auto found = field.chunks.find(coordinate);
        return found == field.chunks.end() ? nullptr : &found->second;
    }

    PixelFieldRuntime::Chunk* PixelFieldRuntime::chunkAt(
        Field& field,
        PixelCellCoord coordinate) noexcept
    {
        return chunkAt(field, chunkOf(coordinate));
    }

    const PixelFieldRuntime::Chunk* PixelFieldRuntime::chunkAt(
        const Field& field,
        PixelCellCoord coordinate) const noexcept
    {
        return chunkAt(field, chunkOf(coordinate));
    }

    void PixelFieldRuntime::addActiveChunk(
        Field& field,
        PixelChunkCoord coordinate,
        Chunk& chunk) noexcept
    {
        if (chunk.simulation_active ||
            !field.active_chunks->activate(coordinate))
        {
            std::abort();
        }
    }

    void PixelFieldRuntime::removeActiveChunk(
        Field& field,
        PixelChunkCoord coordinate,
        Chunk& chunk) noexcept
    {
        if (!chunk.simulation_active ||
            !field.active_chunks->deactivate(coordinate))
        {
            std::abort();
        }
    }

    MaterialId* PixelFieldRuntime::cellAt(
        Field& field,
        PixelCellCoord coordinate) noexcept
    {
        auto* chunk = chunkAt(field, coordinate);
        if (!chunk)
            return nullptr;
        return &chunk->cells[cellOrdinal(
            localCoordinate(coordinate.x),
            localCoordinate(coordinate.y))];
    }

    const MaterialId* PixelFieldRuntime::cellAt(
        const Field& field,
        PixelCellCoord coordinate) const noexcept
    {
        const auto* chunk = chunkAt(field, coordinate);
        if (!chunk)
            return nullptr;
        return &chunk->cells[cellOrdinal(
            localCoordinate(coordinate.x),
            localCoordinate(coordinate.y))];
    }

    std::uint8_t* PixelFieldRuntime::movedAt(
        Field& field,
        PixelCellCoord coordinate) noexcept
    {
        auto* chunk = chunkAt(field, coordinate);
        if (!chunk)
            return nullptr;
        return &chunk->moved[cellOrdinal(
            localCoordinate(coordinate.x),
            localCoordinate(coordinate.y))];
    }

    bool PixelFieldRuntime::loadChunk(
        PixelFieldHandle handle,
        PixelChunkLoad&& load)
    {
        auto context = chunkPreparationContext(handle);
        if (!context)
            return false;
        ++synchronous_chunk_preparations_;
        auto prepared = preparePixelChunk(
            std::move(load), std::move(*context));
        return prepared && adoptPreparedChunk(handle, std::move(*prepared));
    }

    std::optional<PixelChunkPreparationContext>
    PixelFieldRuntime::chunkPreparationContext(
        PixelFieldHandle handle) const
    {
        const auto* field = resolve(handle);
        if (!field)
            return std::nullopt;
        PixelChunkPreparationContext result;
        result.channels_mask = field->desc.channels_mask;
        result.blocking_materials.resize(materials_.count(), 0u);
        for (std::size_t index = 0u;
             index < result.blocking_materials.size();
             ++index)
        {
            result.blocking_materials[index] = isBlocking(
                materials_, static_cast<MaterialId>(index))
                ? 1u
                : 0u;
        }
        return result;
    }

    bool PixelFieldRuntime::adoptPreparedChunk(
        PixelFieldHandle handle,
        PreparedPixelChunk&& prepared)
    {
        auto* field = resolve(handle);
        auto* storage = prepared.storage_.get();
        if (!field || !storage ||
            !coordinateAllowed(*field, storage->coordinate) ||
            field->chunks.contains(storage->coordinate) ||
            field->desc.channels_mask != storage->channels_mask)
        {
            return false;
        }

        Chunk chunk;
        chunk.cells = std::move(storage->cells);
        chunk.moved = std::move(storage->moved);
        chunk.ledger = std::move(storage->ledger);
        chunk.temperature = std::move(storage->temperature);
        chunk.lifetime = std::move(storage->lifetime);
        chunk.active = std::move(storage->active);
        chunk.active_next = std::move(storage->active_next);
        chunk.changed = std::move(storage->changed);
        chunk.minimum_x = std::move(storage->minimum_x);
        chunk.minimum_y = std::move(storage->minimum_y);
        chunk.maximum_x = std::move(storage->maximum_x);
        chunk.maximum_y = std::move(storage->maximum_y);
        chunk.next_minimum_x = std::move(storage->next_minimum_x);
        chunk.next_minimum_y = std::move(storage->next_minimum_y);
        chunk.next_maximum_x = std::move(storage->next_maximum_x);
        chunk.next_maximum_y = std::move(storage->next_maximum_y);
        chunk.blocking = std::move(storage->blocking);
        chunk.base_digest = storage->base_digest;
        chunk.sequence = storage->sequence;
        chunk.delta = std::move(storage->delta);
        chunk.presentation_active = storage->presentation_active;
        chunk.simulation_active = storage->simulation_active;
        const auto coordinate = storage->coordinate;
        const auto [iterator, inserted] = field->chunks.emplace(
            coordinate, std::move(chunk));
        if (!inserted)
            return false;
        if (!field->active_chunks->track(
                coordinate,
                iterator->second.simulation_active))
        {
            std::abort();
        }
        if (!field->presentation_chunks->track(
                coordinate,
                iterator->second.presentation_active))
        {
            std::abort();
        }
        prepared.storage_.reset();
        ++prepared_chunk_adoptions_;
        pushEvent({
            PixelFieldEvent::EKind::CHUNK_LOADED,
            handle,
            coordinate,
            0u});
        return true;
    }

    bool PixelFieldRuntime::captureChunk(
        PixelFieldHandle handle,
        PixelChunkCoord coordinate,
        PixelChunkSnapshot& output) const
    {
        const auto* field = resolve(handle);
        const auto* chunk = field ? chunkAt(*field, coordinate) : nullptr;
        if (!chunk)
            return false;
        PixelChunkSnapshot result;
        result.coordinate = coordinate;
        result.materials = chunk->cells;
        result.temperature = chunk->temperature;
        result.lifetime = chunk->lifetime;
        result.base_digest = chunk->base_digest;
        result.sequence = chunk->sequence;
        result.presentation_active = chunk->presentation_active;
        result.simulation_active = chunk->simulation_active;
        result.delta.reserve(chunk->delta.size());
        for (const auto& [ordinal, material] : chunk->delta)
        {
            result.delta.push_back({
                static_cast<std::uint16_t>(ordinal % kChunkSizeCells),
                static_cast<std::uint16_t>(ordinal / kChunkSizeCells),
                material});
        }
        std::ranges::sort(result.delta, {}, [](const auto& item)
        {
            return static_cast<std::uint32_t>(item.y) * kChunkSizeCells +
                item.x;
        });
        output = std::move(result);
        return true;
    }

    bool PixelFieldRuntime::captureChunkDelta(
        PixelFieldHandle handle,
        PixelChunkCoord coordinate,
        PixelChunkDeltaSnapshot& output) const
    {
        const auto* field = resolve(handle);
        const auto* chunk = field ? chunkAt(*field, coordinate) : nullptr;
        if (!chunk)
            return false;
        PixelChunkDeltaSnapshot result;
        result.coordinate = coordinate;
        result.base_digest = chunk->base_digest;
        result.sequence = chunk->sequence;
        result.delta.reserve(chunk->delta.size());
        for (const auto& [ordinal, material] : chunk->delta)
        {
            result.delta.push_back({
                static_cast<std::uint16_t>(ordinal % kChunkSizeCells),
                static_cast<std::uint16_t>(ordinal / kChunkSizeCells),
                material});
        }
        std::ranges::sort(result.delta, {}, [](const auto& item)
        {
            return static_cast<std::uint32_t>(item.y) * kChunkSizeCells +
                item.x;
        });
        output = std::move(result);
        return true;
    }

    bool PixelFieldRuntime::unloadChunk(
        PixelFieldHandle handle,
        PixelChunkCoord coordinate,
        PixelChunkSnapshot& output)
    {
        auto* field = resolve(handle);
        if (!field)
            return false;
        if (!captureChunk(handle, coordinate, output))
            return false;
        ++capturing_chunk_unloads_;
        return discardChunk(handle, coordinate);
    }

    bool PixelFieldRuntime::discardChunk(
        PixelFieldHandle handle,
        PixelChunkCoord coordinate)
    {
        auto* field = resolve(handle);
        if (!field)
            return false;
        auto found = field->chunks.find(coordinate);
        if (found == field->chunks.end())
            return false;
        if (!field->active_chunks->untrack(coordinate))
            std::abort();
        if (!field->presentation_chunks->untrack(coordinate))
            std::abort();
        field->chunks.erase(coordinate);
        ++discard_chunk_retires_;
        pushEvent({
            PixelFieldEvent::EKind::CHUNK_UNLOADED,
            handle,
            coordinate,
            0u});
        return true;
    }

    bool PixelFieldRuntime::setChunkSimulationActive(
        PixelFieldHandle handle,
        PixelChunkCoord coordinate,
        bool active) noexcept
    {
        auto* field = resolve(handle);
        auto* chunk = field ? chunkAt(*field, coordinate) : nullptr;
        if (!chunk)
            return false;
        if (chunk->simulation_active == active)
            return true;
        if (active)
        {
            addActiveChunk(*field, coordinate, *chunk);
            chunk->simulation_active = true;
            chunk->active.fill(1u);
            chunk->maximum_x.fill(kTileSize - 1u);
            chunk->maximum_y.fill(kTileSize - 1u);
        }
        else
        {
            removeActiveChunk(*field, coordinate, *chunk);
            chunk->simulation_active = false;
            chunk->active.fill(0u);
            chunk->active_next.fill(0u);
            chunk->changed.fill(0u);
        }
        return true;
    }

    bool PixelFieldRuntime::setChunkPresentationActive(
        PixelFieldHandle handle,
        PixelChunkCoord coordinate,
        bool active) noexcept
    {
        auto* field = resolve(handle);
        auto* chunk = field ? chunkAt(*field, coordinate) : nullptr;
        if (!chunk)
            return false;
        if (chunk->presentation_active == active)
            return true;
        const bool changed = active
            ? field->presentation_chunks->activate(coordinate)
            : field->presentation_chunks->deactivate(coordinate);
        if (!changed)
            std::abort();
        chunk->presentation_active = active;
        return true;
    }

    bool PixelFieldRuntime::applyChunkDelta(
        PixelFieldHandle handle,
        PixelChunkCoord coordinate,
        const lux::cxx::algorithm::Sha256Digest& base_digest,
        std::uint64_t sequence,
        std::span<const PixelChunkDeltaCell> delta) noexcept
    {
        auto* field = resolve(handle);
        auto* chunk = field ? chunkAt(*field, coordinate) : nullptr;
        if (!chunk || chunk->base_digest != base_digest ||
            sequence < chunk->sequence)
        {
            return false;
        }
        std::uint16_t previous = 0u;
        bool first = true;
        for (const auto& edit : delta)
        {
            if (edit.x >= kChunkSizeCells || edit.y >= kChunkSizeCells)
                return false;
            const auto cell = cellOrdinal(edit.x, edit.y);
            if (!first && cell <= previous)
                return false;
            first = false;
            previous = cell;
        }
        for (const auto& edit : delta)
        {
            const PixelCellCoord cell{
                coordinate.x * kChunkSizeCells + edit.x,
                coordinate.y * kChunkSizeCells + edit.y};
            auto* material = cellAt(*field, cell);
            if (!material)
                return false;
            if (*material == edit.material)
                continue;
            *material = edit.material;
            markCellChanged(*field, cell, edit.material);
        }
        chunk->sequence = sequence;
        return true;
    }

    std::optional<lux::cxx::algorithm::Sha256Digest>
    PixelFieldRuntime::chunkBaseDigest(
        PixelFieldHandle handle,
        PixelChunkCoord coordinate) const noexcept
    {
        const auto* field = resolve(handle);
        const auto* chunk = field ? chunkAt(*field, coordinate) : nullptr;
        return chunk
            ? std::optional<lux::cxx::algorithm::Sha256Digest>{chunk->base_digest}
            : std::nullopt;
    }

    bool PixelFieldRuntime::chunkResident(
        PixelFieldHandle handle,
        PixelChunkCoord coordinate) const noexcept
    {
        const auto* field = resolve(handle);
        return field && chunkAt(*field, coordinate) != nullptr;
    }

    void PixelFieldRuntime::residentChunks(
        PixelFieldHandle handle,
        std::vector<PixelChunkCoord>& output) const
    {
        output.clear();
        const auto* field = resolve(handle);
        if (!field)
            return;
        output.reserve(field->chunks.size());
        for (const auto& [coordinate, _] : field->chunks)
            output.push_back(coordinate);
        std::ranges::sort(output);
    }

    std::span<const PixelChunkCoord> PixelFieldRuntime::activeKeys(
        PixelFieldHandle handle) const noexcept
    {
        const auto* field = resolve(handle);
        return field ? field->active_chunks->keys()
                     : std::span<const PixelChunkCoord>{};
    }

    std::span<const PixelChunkCoord>
    PixelFieldRuntime::presentationKeys(
        PixelFieldHandle handle) const noexcept
    {
        const auto* field = resolve(handle);
        return field ? field->presentation_chunks->keys()
                     : std::span<const PixelChunkCoord>{};
    }

    bool PixelFieldRuntime::chunkSimulationActive(
        PixelFieldHandle handle,
        PixelChunkCoord coordinate) const noexcept
    {
        const auto* field = resolve(handle);
        return field && field->active_chunks->contains(coordinate);
    }

    bool PixelFieldRuntime::updateFrame(
        PixelFieldHandle handle,
        const PixelFieldFrame& frame,
        float priority,
        bool visible,
        bool simulation_enabled) noexcept
    {
        auto* field = resolve(handle);
        if (!field || !lux::math::isFinite(frame.origin) ||
            !(frame.cell_size > 0.0f) || !std::isfinite(priority))
        {
            return false;
        }
        field->frame = frame;
        field->frame_priority = priority;
        field->frame_valid = true;
        field->visible = visible;
        setFieldSimulationEnabled(*field, simulation_enabled);
        return true;
    }

    bool PixelFieldRuntime::regionBlocked(
        PixelFieldHandle handle,
        PixelCellCoord minimum,
        PixelCellCoord maximum) const noexcept
    {
        const auto* field = resolve(handle);
        if (!field || minimum.x > maximum.x || minimum.y > maximum.y)
            return false;
        auto minimum_chunk = chunkOf(minimum);
        auto maximum_chunk = chunkOf(maximum);
        for (auto chunk_y = minimum_chunk.y;; ++chunk_y)
        {
            for (auto chunk_x = minimum_chunk.x;; ++chunk_x)
            {
                const PixelChunkCoord coordinate{chunk_x, chunk_y};
                if (coordinateAllowed(*field, coordinate))
                {
                    const auto* chunk = chunkAt(*field, coordinate);
                    if (!chunk)
                        return true;
                    PixelCellCoord base;
                    if (!chunkBase(coordinate, base))
                        return true;
                    const auto local_minimum_x = static_cast<std::uint32_t>(
                        std::max<std::int64_t>(minimum.x - base.x, 0));
                    const auto local_minimum_y = static_cast<std::uint32_t>(
                        std::max<std::int64_t>(minimum.y - base.y, 0));
                    const auto local_maximum_x = static_cast<std::uint32_t>(
                        std::min<std::int64_t>(
                            maximum.x - base.x, kChunkSizeCells - 1u));
                    const auto local_maximum_y = static_cast<std::uint32_t>(
                        std::min<std::int64_t>(
                            maximum.y - base.y, kChunkSizeCells - 1u));
                    for (auto tile_y = local_minimum_y / kTileSize;
                         tile_y <= local_maximum_y / kTileSize; ++tile_y)
                    {
                        for (auto tile_x = local_minimum_x / kTileSize;
                             tile_x <= local_maximum_x / kTileSize; ++tile_x)
                        {
                            const auto ordinal = static_cast<std::size_t>(
                                tile_y) * kTilesPerChunk + tile_x;
                            if (chunk->blocking[ordinal] == 0u)
                                continue;
                            const auto x0 = std::max(
                                local_minimum_x, tile_x * kTileSize);
                            const auto y0 = std::max(
                                local_minimum_y, tile_y * kTileSize);
                            const auto x1 = std::min(
                                local_maximum_x,
                                tile_x * kTileSize + kTileSize - 1u);
                            const auto y1 = std::min(
                                local_maximum_y,
                                tile_y * kTileSize + kTileSize - 1u);
                            for (auto y = y0; y <= y1; ++y)
                                for (auto x = x0; x <= x1; ++x)
                                    if (isBlocking(
                                            materials_,
                                            chunk->cells[cellOrdinal(x, y)]))
                                        return true;
                        }
                    }
                }
                if (chunk_x == maximum_chunk.x)
                    break;
                if (chunk_x == std::numeric_limits<std::int64_t>::max())
                    return true;
            }
            if (chunk_y == maximum_chunk.y)
                break;
            if (chunk_y == std::numeric_limits<std::int64_t>::max())
                return true;
        }
        return false;
    }

    void PixelFieldRuntime::queryFields(
        const lux::math::Position2d& minimum,
        const lux::math::Position2d& maximum,
        std::vector<PixelFieldQueryEntry>& output) const
    {
        output.clear();
        for (const auto& field : fields_.values())
        {
            if (!field.frame_valid || !field.visible)
                continue;
            const auto minimum_cell = worldToCell(field.frame, minimum);
            const auto maximum_cell = worldToCell(field.frame, maximum);
            if (!minimum_cell || !maximum_cell)
                continue;
            bool intersects = field.desc.extent == EPixelFieldExtent::INFINITE_FIELD;
            if (!intersects)
            {
                const auto minimum_chunk = chunkOf(*minimum_cell);
                const auto maximum_chunk = chunkOf(*maximum_cell);
                intersects = minimum_chunk.x <= field.desc.bounds.maximum.x &&
                    maximum_chunk.x >= field.desc.bounds.minimum.x &&
                    minimum_chunk.y <= field.desc.bounds.maximum.y &&
                    maximum_chunk.y >= field.desc.bounds.minimum.y;
            }
            if (intersects)
            {
                output.push_back({
                    field.handle,
                    field.frame,
                    field.frame_priority});
            }
        }
        std::stable_sort(
            output.begin(), output.end(),
            [](const auto& left, const auto& right)
            {
                return left.priority > right.priority;
            });
    }

    void PixelFieldRuntime::markCellChanged(
        Field& field,
        PixelCellCoord coordinate,
        MaterialId material) noexcept
    {
        auto* chunk = chunkAt(field, coordinate);
        if (!chunk)
            return;
        const auto local_x = localCoordinate(coordinate.x);
        const auto local_y = localCoordinate(coordinate.y);
        const auto ordinal = cellOrdinal(local_x, local_y);
        chunk->ledger.markDirty(local_x, local_y, 1u, 1u);
        chunk->changed[tileOrdinal(local_x, local_y)] = 1u;
        chunk->delta.insert_or_assign(ordinal, material);
        ++chunk->sequence;
    }

    void PixelFieldRuntime::applyCommands()
    {
        for (const auto& command : commands_)
        {
            auto* field = resolve(command.field);
            const auto area = static_cast<std::uint64_t>(command.extent.width) *
                command.extent.height;
            const bool stamp_cells =
                command.kind == PixelFieldCommand::EKind::STAMP_CELLS;
            if (!field || area == 0u || area > kMaximumCommandCells ||
                (stamp_cells &&
                 (!command.cells || command.cells->size() != area)))
            {
                if (field)
                    pushEvent({
                        PixelFieldEvent::EKind::COMMANDS_APPLIED,
                        command.field,
                        {},
                        0u});
                continue;
            }
            std::int64_t end_x = 0;
            std::int64_t end_y = 0;
            if (!checkedAdd(
                    command.minimum.x,
                    command.extent.width - 1u,
                    end_x) ||
                !checkedAdd(
                    command.minimum.y,
                    command.extent.height - 1u,
                    end_y))
            {
                pushEvent({
                    PixelFieldEvent::EKind::COMMANDS_APPLIED,
                    command.field,
                    {},
                    0u});
                continue;
            }
            std::uint32_t changed = 0u;
            for (std::uint32_t row = 0u; row < command.extent.height; ++row)
            {
                const auto y = command.minimum.y + row;
                for (std::uint32_t column = 0u;
                     column < command.extent.width; ++column)
                {
                    const PixelCellCoord coordinate{
                        command.minimum.x + column, y};
                    auto* chunk = chunkAt(*field, coordinate);
                    if (!chunk || !coordinateAllowed(
                            *field, chunkOf(coordinate)))
                    {
                        continue;
                    }
                    auto* cell = cellAt(*field, coordinate);
                    auto wanted = command.material;
                    if (stamp_cells)
                    {
                        wanted = (*command.cells)[
                            static_cast<std::size_t>(row) *
                                command.extent.width + column];
                        if (wanted == kEmptyMaterial ||
                            *cell != kEmptyMaterial)
                        {
                            continue;
                        }
                    }
                    if (*cell == wanted)
                        continue;
                    const auto local_x = localCoordinate(coordinate.x);
                    const auto local_y = localCoordinate(coordinate.y);
                    auto& blocking = chunk->blocking[
                        tileOrdinal(local_x, local_y)];
                    const bool was_blocking = isBlocking(materials_, *cell);
                    const bool now_blocking = isBlocking(materials_, wanted);
                    if (was_blocking && !now_blocking)
                        --blocking;
                    else if (!was_blocking && now_blocking)
                        ++blocking;
                    *cell = wanted;
                    markCellChanged(*field, coordinate, wanted);
                    ++changed;
                }
            }
            if (changed != 0u)
            {
                wakeSpan(
                    *field,
                    {
                        saturatingAdd(
                            command.minimum.x,
                            -kLiquidDispersion),
                        saturatingAdd(command.minimum.y, -1)},
                    {
                        saturatingAdd(end_x, kLiquidDispersion),
                        saturatingAdd(end_y, 1)},
                    false);
            }
            pushEvent({
                PixelFieldEvent::EKind::COMMANDS_APPLIED,
                command.field,
                {},
                changed});
        }
        commands_.clear();
    }

    void PixelFieldRuntime::wakeSpan(
        Field& field,
        PixelCellCoord minimum,
        PixelCellCoord maximum,
        bool next)
    {
        if (minimum.x > maximum.x || minimum.y > maximum.y)
            return;
        const auto minimum_chunk = chunkOf(minimum);
        const auto maximum_chunk = chunkOf(maximum);
        for (auto chunk_y = minimum_chunk.y;; ++chunk_y)
        {
            for (auto chunk_x = minimum_chunk.x;; ++chunk_x)
            {
                const PixelChunkCoord coordinate{chunk_x, chunk_y};
                auto* chunk = chunkAt(field, coordinate);
                if (chunk && chunk->simulation_active)
                {
                    PixelCellCoord base;
                    if (!chunkBase(coordinate, base))
                        return;
                    const auto x0 = static_cast<std::uint32_t>(
                        std::max<std::int64_t>(minimum.x - base.x, 0));
                    const auto y0 = static_cast<std::uint32_t>(
                        std::max<std::int64_t>(minimum.y - base.y, 0));
                    const auto x1 = static_cast<std::uint32_t>(
                        std::min<std::int64_t>(
                            maximum.x - base.x, kChunkSizeCells - 1u));
                    const auto y1 = static_cast<std::uint32_t>(
                        std::min<std::int64_t>(
                            maximum.y - base.y, kChunkSizeCells - 1u));
                    auto& active = next
                        ? chunk->active_next
                        : chunk->active;
                    auto& minimum_x = next
                        ? chunk->next_minimum_x
                        : chunk->minimum_x;
                    auto& minimum_y = next
                        ? chunk->next_minimum_y
                        : chunk->minimum_y;
                    auto& maximum_x = next
                        ? chunk->next_maximum_x
                        : chunk->maximum_x;
                    auto& maximum_y = next
                        ? chunk->next_maximum_y
                        : chunk->maximum_y;
                    for (auto tile_y = y0 / kTileSize;
                         tile_y <= y1 / kTileSize; ++tile_y)
                    {
                        for (auto tile_x = x0 / kTileSize;
                             tile_x <= x1 / kTileSize; ++tile_x)
                        {
                            const auto ordinal = static_cast<std::size_t>(
                                tile_y) * kTilesPerChunk + tile_x;
                            const auto local_minimum_x =
                                static_cast<std::uint8_t>(
                                    std::max(x0, tile_x * kTileSize) -
                                    tile_x * kTileSize);
                            const auto local_minimum_y =
                                static_cast<std::uint8_t>(
                                    std::max(y0, tile_y * kTileSize) -
                                    tile_y * kTileSize);
                            const auto local_maximum_x =
                                static_cast<std::uint8_t>(
                                    std::min(
                                        x1,
                                        tile_x * kTileSize +
                                            kTileSize - 1u) -
                                    tile_x * kTileSize);
                            const auto local_maximum_y =
                                static_cast<std::uint8_t>(
                                    std::min(
                                        y1,
                                        tile_y * kTileSize +
                                            kTileSize - 1u) -
                                    tile_y * kTileSize);
                            if (active[ordinal] == 0u)
                            {
                                active[ordinal] = 1u;
                                minimum_x[ordinal] = local_minimum_x;
                                minimum_y[ordinal] = local_minimum_y;
                                maximum_x[ordinal] = local_maximum_x;
                                maximum_y[ordinal] = local_maximum_y;
                            }
                            else
                            {
                                minimum_x[ordinal] = std::min(
                                    minimum_x[ordinal], local_minimum_x);
                                minimum_y[ordinal] = std::min(
                                    minimum_y[ordinal], local_minimum_y);
                                maximum_x[ordinal] = std::max(
                                    maximum_x[ordinal], local_maximum_x);
                                maximum_y[ordinal] = std::max(
                                    maximum_y[ordinal], local_maximum_y);
                            }
                        }
                    }
                }
                if (chunk_x == maximum_chunk.x)
                    break;
                if (chunk_x == std::numeric_limits<std::int64_t>::max())
                    return;
            }
            if (chunk_y == maximum_chunk.y)
                break;
            if (chunk_y == std::numeric_limits<std::int64_t>::max())
                return;
        }
    }

    bool PixelFieldRuntime::tryMove(
        Field& field,
        PixelCellCoord source,
        PixelCellCoord target,
        bool displace_liquid,
        std::uint32_t& moved_counter)
    {
        auto* source_chunk = chunkAt(field, source);
        auto* target_chunk = chunkAt(field, target);
        if (!source_chunk || !target_chunk ||
            !source_chunk->simulation_active ||
            !target_chunk->simulation_active)
        {
            return false;
        }
        auto* source_cell = cellAt(field, source);
        auto* target_cell = cellAt(field, target);
        const auto source_material = *source_cell;
        const auto target_material = *target_cell;
        if (target_material == kEmptyMaterial)
        {
            *target_cell = source_material;
            *source_cell = kEmptyMaterial;
        }
        else if (displace_liquid &&
            materials_.at(target_material).phase == EMaterialPhase::LIQUID &&
            materials_.at(source_material).density >
                materials_.at(target_material).density)
        {
            *target_cell = source_material;
            *source_cell = target_material;
            if (auto* moved = movedAt(field, source))
                *moved = 1u;
        }
        else
        {
            return false;
        }
        if (auto* moved = movedAt(field, target))
            *moved = 1u;
        ++moved_counter;

        const auto source_local_x = localCoordinate(source.x);
        const auto source_local_y = localCoordinate(source.y);
        const auto target_local_x = localCoordinate(target.x);
        const auto target_local_y = localCoordinate(target.y);
        const auto source_tile = tileOrdinal(
            source_local_x, source_local_y);
        const auto target_tile = tileOrdinal(
            target_local_x, target_local_y);
        if ((source_chunk != target_chunk || source_tile != target_tile) &&
            isBlocking(materials_, source_material))
        {
            --source_chunk->blocking[source_tile];
            ++target_chunk->blocking[target_tile];
        }
        markCellChanged(field, source, *source_cell);
        markCellChanged(field, target, *target_cell);
        wakeSpan(
            field,
            {
                saturatingAdd(std::min(source.x, target.x), -1),
                saturatingAdd(std::min(source.y, target.y), -1)},
            {
                saturatingAdd(std::max(source.x, target.x), 1),
                saturatingAdd(std::max(source.y, target.y), 1)},
            true);
        if (*source_cell == kEmptyMaterial)
        {
            wakeSpan(
                field,
                {
                    saturatingAdd(source.x, -kLiquidDispersion),
                    source.y},
                {
                    saturatingAdd(source.x, kLiquidDispersion),
                    saturatingAdd(source.y, 1)},
                true);
        }
        return true;
    }

    void PixelFieldRuntime::stepChunk(
        Field& field,
        PixelChunkCoord coordinate,
        Chunk& chunk,
        std::uint32_t& moved,
        std::uint32_t& scanned)
    {
        if (!chunk.simulation_active)
            return;
        PixelCellCoord base;
        if (!chunkBase(coordinate, base))
            return;
        for (std::uint32_t tile_y = 0u;
             tile_y < kTilesPerChunk; ++tile_y)
        {
            for (std::uint32_t tile_x = 0u;
                 tile_x < kTilesPerChunk; ++tile_x)
            {
                const auto tile = static_cast<std::size_t>(tile_y) *
                    kTilesPerChunk + tile_x;
                if (chunk.active[tile] == 0u)
                    continue;
                const auto y0 = tile_y * kTileSize +
                    chunk.minimum_y[tile];
                const auto y1 = tile_y * kTileSize +
                    chunk.maximum_y[tile];
                const auto x0 = tile_x * kTileSize +
                    chunk.minimum_x[tile];
                const auto x1 = tile_x * kTileSize +
                    chunk.maximum_x[tile];
                for (auto local_y = y0; local_y <= y1; ++local_y)
                {
                    const auto global_y = base.y + local_y;
                    const bool right_to_left =
                        (static_cast<std::uint64_t>(global_y) +
                         field.steps) % 2u != 0u;
                    const std::int64_t first = right_to_left ? 1 : -1;
                    for (std::uint32_t offset = 0u;
                         offset <= x1 - x0; ++offset)
                    {
                        const auto local_x = right_to_left
                            ? x1 - offset
                            : x0 + offset;
                        const PixelCellCoord current{
                            base.x + local_x, global_y};
                        ++scanned;
                        const auto material =
                            chunk.cells[cellOrdinal(local_x, local_y)];
                        if (material == kEmptyMaterial ||
                            chunk.moved[cellOrdinal(local_x, local_y)] != 0u)
                        {
                            continue;
                        }
                        const auto phase = materials_.at(material).phase;
                        if (phase == EMaterialPhase::POWDER ||
                            phase == EMaterialPhase::LIQUID)
                        {
                            std::int64_t distance = 0;
                            while (distance < kMaximumFallCells)
                            {
                                const PixelCellCoord target{
                                    current.x,
                                    current.y - distance - 1};
                                const auto* target_chunk =
                                    chunkAt(field, target);
                                const auto* target_cell =
                                    cellAt(field, target);
                                if (!target_chunk ||
                                    !target_chunk->simulation_active ||
                                    !target_cell ||
                                    *target_cell != kEmptyMaterial)
                                {
                                    break;
                                }
                                ++distance;
                            }
                            if (distance != 0 && tryMove(
                                    field,
                                    current,
                                    {current.x, current.y - distance},
                                    false,
                                    moved))
                            {
                                continue;
                            }
                        }
                        if (phase == EMaterialPhase::POWDER)
                        {
                            if (tryMove(
                                    field,
                                    current,
                                    {current.x, current.y - 1},
                                    true,
                                    moved) ||
                                tryMove(
                                    field,
                                    current,
                                    {current.x + first, current.y - 1},
                                    true,
                                    moved) ||
                                tryMove(
                                    field,
                                    current,
                                    {current.x - first, current.y - 1},
                                    true,
                                    moved))
                            {
                                continue;
                            }
                        }
                        else if (phase == EMaterialPhase::LIQUID)
                        {
                            if (tryMove(
                                    field,
                                    current,
                                    {current.x + first, current.y - 1},
                                    false,
                                    moved) ||
                                tryMove(
                                    field,
                                    current,
                                    {current.x - first, current.y - 1},
                                    false,
                                    moved))
                            {
                                continue;
                            }
                            const auto flow_toward = [&](
                                std::int64_t direction) noexcept
                            {
                                for (std::int64_t distance = 1;
                                     distance <= kLiquidDispersion;
                                     ++distance)
                                {
                                    const PixelCellCoord path{
                                        current.x + direction * distance,
                                        current.y};
                                    const auto* path_chunk = chunkAt(field, path);
                                    const auto* path_cell = cellAt(field, path);
                                    const auto* below = cellAt(
                                        field, {path.x, path.y - 1});
                                    if (!path_chunk ||
                                        !path_chunk->simulation_active ||
                                        !path_cell ||
                                        *path_cell != kEmptyMaterial)
                                    {
                                        return false;
                                    }
                                    if (below && *below == kEmptyMaterial)
                                    {
                                        return tryMove(
                                            field,
                                            current,
                                            {current.x + direction,
                                             current.y},
                                            false,
                                            moved);
                                    }
                                }
                                return false;
                            };
                            if (flow_toward(first) || flow_toward(-first))
                                continue;
                        }
                    }
                }
            }
        }
    }

    void PixelFieldRuntime::stepField(Field& field)
    {
        const auto started = std::chrono::steady_clock::now();
        const auto active_keys = field.active_chunks->keys();
        field.chunks_visited_last = static_cast<std::uint32_t>(
            active_keys.size());
        std::size_t active_count = 0u;
        for (const auto coordinate : active_keys)
        {
            auto* chunk_pointer = chunkAt(field, coordinate);
            if (!chunk_pointer || !chunk_pointer->simulation_active)
                std::abort();
            auto& chunk = *chunk_pointer;
            std::memset(
                chunk.moved.data(), 0, chunk.moved.size());
            chunk.changed.fill(0u);
            chunk.active_next.fill(0u);
            if (chunk.hasActiveTiles())
                ++active_count;
        }
        field.moved_cells_last = 0u;
        field.cells_scanned_last = 0u;

        static_assert(
            kLiquidDispersion + 1 <= kTileSize &&
            kMaximumFallCells + 1 <= kTileSize);

        auto& work = parallel_->work_items;
        if (work.capacity() < active_count)
        {
            work.reserve(active_count);
            ++parallel_->scratch_growth_count;
        }
        for (std::uint32_t colour = 0u; colour < 4u; ++colour)
        {
            work.clear();
            const auto parity_x = colour & 1u;
            const auto parity_y = colour >> 1u;
            for (const auto coordinate : active_keys)
            {
                auto* chunk_pointer = chunkAt(field, coordinate);
                if (!chunk_pointer || !chunk_pointer->simulation_active)
                    std::abort();
                auto& chunk = *chunk_pointer;
                if (chunk.hasActiveTiles() &&
                    coordinateParity(coordinate.x) == parity_x &&
                    coordinateParity(coordinate.y) == parity_y)
                {
                    work.push_back({coordinate, &chunk});
                }
            }
            std::ranges::sort(work, {}, [](const auto& item)
            {
                return item.coordinate;
            });
            if (parallelism_ == 1u || work.size() <= 1u)
            {
                for (auto& item : work)
                    stepChunk(
                        field,
                        item.coordinate,
                        *item.chunk,
                        item.moved,
                        item.scanned);
            }
            else
            {
                parallel_->arena.execute([&]() noexcept
                {
                    tbb::parallel_for(
                        tbb::blocked_range<std::size_t>{
                            0u, work.size(), 1u},
                        [&](const auto& range)
                        {
                            for (auto index = range.begin();
                                 index != range.end(); ++index)
                            {
                                auto& item = work[index];
                                stepChunk(
                                    field,
                                    item.coordinate,
                                    *item.chunk,
                                    item.moved,
                                    item.scanned);
                            }
                        });
                });
            }
            for (const auto& item : work)
            {
                field.moved_cells_last += item.moved;
                field.cells_scanned_last += item.scanned;
            }
        }

        for (const auto coordinate : active_keys)
        {
            auto* chunk_pointer = chunkAt(field, coordinate);
            if (!chunk_pointer || !chunk_pointer->simulation_active)
                std::abort();
            auto& chunk = *chunk_pointer;
            for (std::uint32_t tile_y = 0u;
                 tile_y < kTilesPerChunk; ++tile_y)
            {
                for (std::uint32_t tile_x = 0u;
                     tile_x < kTilesPerChunk; ++tile_x)
                {
                    const auto ordinal = static_cast<std::size_t>(tile_y) *
                        kTilesPerChunk + tile_x;
                    if (chunk.changed[ordinal] != 0u)
                    {
                        chunk.ledger.markDirty(
                            tile_x * kTileSize,
                            tile_y * kTileSize,
                            kTileSize,
                            kTileSize);
                    }
                }
            }
            chunk.active.swap(chunk.active_next);
            chunk.minimum_x.swap(chunk.next_minimum_x);
            chunk.minimum_y.swap(chunk.next_minimum_y);
            chunk.maximum_x.swap(chunk.next_maximum_x);
            chunk.maximum_y.swap(chunk.next_maximum_y);
        }
        ++field.steps;
        field.step_ms_last = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - started).count();
    }

    void PixelFieldRuntime::step()
    {
        for (const auto handle : simulation_fields_)
        {
            auto* field = resolve(handle);
            if (!field || !field->simulation_enabled)
                std::abort();
            stepField(*field);
        }
    }

    std::uint64_t PixelFieldRuntime::determinismHash(
        PixelFieldHandle handle) const noexcept
    {
        const auto* field = resolve(handle);
        if (!field)
            return 0u;
        std::vector<PixelChunkCoord> coordinates;
        coordinates.reserve(field->chunks.size());
        for (const auto& [coordinate, _] : field->chunks)
            coordinates.push_back(coordinate);
        std::ranges::sort(coordinates);
        auto hash = kFnvBasis;
        for (const auto coordinate : coordinates)
        {
            const auto* chunk = chunkAt(*field, coordinate);
            hash = fnv1a64Append(hash, &coordinate.x, sizeof(coordinate.x));
            hash = fnv1a64Append(hash, &coordinate.y, sizeof(coordinate.y));
            hash = fnv1a64Append(
                hash,
                chunk->cells.data(),
                chunk->cells.size() * sizeof(MaterialId));
        }
        return hash;
    }

    std::uint32_t PixelFieldRuntime::movedCellsLastStep(
        PixelFieldHandle handle) const noexcept
    {
        const auto* field = resolve(handle);
        return field ? field->moved_cells_last : 0u;
    }

    std::uint32_t PixelFieldRuntime::cellsScannedLastStep(
        PixelFieldHandle handle) const noexcept
    {
        const auto* field = resolve(handle);
        return field ? field->cells_scanned_last : 0u;
    }

    std::uint32_t PixelFieldRuntime::activeTiles(
        PixelFieldHandle handle) const noexcept
    {
        const auto* field = resolve(handle);
        if (!field)
            return 0u;
        std::uint32_t result = 0u;
        for (const auto coordinate : field->active_chunks->keys())
        {
            const auto* chunk = chunkAt(*field, coordinate);
            if (!chunk || !chunk->simulation_active)
                std::abort();
            for (const auto active : chunk->active)
                result += active != 0u ? 1u : 0u;
        }
        return result;
    }

    double PixelFieldRuntime::stepMillisLast(
        PixelFieldHandle handle) const noexcept
    {
        const auto* field = resolve(handle);
        return field ? field->step_ms_last : 0.0;
    }

    std::uint64_t PixelFieldRuntime::scratchGrowthCount() const noexcept
    {
        return parallel_ ? parallel_->scratch_growth_count : 0u;
    }

    std::uint64_t PixelFieldRuntime::dirtySnapshotBytes() const noexcept
    {
        return dirty_snapshot_bytes_;
    }

    std::uint64_t PixelFieldRuntime::dirtySnapshotAllocations() const noexcept
    {
        return dirty_snapshot_allocations_;
    }

    PixelFieldRuntimeStats PixelFieldRuntime::stats() const noexcept
    {
        PixelFieldRuntimeStats result;
        for (const auto& field : fields_.values())
        {
            result.cells_scanned_last_step += field.cells_scanned_last;
            result.moved_cells_last_step += field.moved_cells_last;
            result.simulation_chunks_visited_last_step +=
                field.chunks_visited_last;
            result.resident_chunks += field.chunks.size();
            result.simulation_active_chunks += field.active_chunks->size();
            result.presentation_active_chunks +=
                field.presentation_chunks->size();
            for (const auto& [_, chunk] : field.chunks)
            {
                result.resident_bytes +=
                    chunk.cells.size() * sizeof(MaterialId) +
                    chunk.moved.size() +
                    chunk.temperature.size() * sizeof(float) +
                    chunk.lifetime.size() * sizeof(float) +
                    chunk.delta.size() *
                        (sizeof(std::uint16_t) + sizeof(MaterialId));
            }
        }
        result.synchronous_chunk_preparations =
            synchronous_chunk_preparations_;
        result.prepared_chunk_adoptions = prepared_chunk_adoptions_;
        result.capturing_chunk_unloads = capturing_chunk_unloads_;
        result.discard_chunk_retires = discard_chunk_retires_;
        return result;
    }

    PixelDirtyLedger* PixelFieldRuntime::dirtyLedger(
        PixelFieldHandle handle,
        PixelChunkCoord coordinate) noexcept
    {
        auto* field = resolve(handle);
        auto* chunk = field ? chunkAt(*field, coordinate) : nullptr;
        return chunk ? &chunk->ledger : nullptr;
    }

    PixelFieldRenderExport PixelFieldRuntime::exportDirty(
        PixelFieldHandle handle,
        PixelChunkCoord coordinate,
        const PixelExportBudget& budget)
    {
        PixelFieldRenderExport result;
        auto* field = resolve(handle);
        auto* chunk = field ? chunkAt(*field, coordinate) : nullptr;
        if (!chunk)
            return result;
        auto plan = chunk->ledger.takeBatch(sizeof(MaterialId), budget);
        if (plan.empty())
            return result;
        auto pixels = std::shared_ptr<std::byte[]>(
            new std::byte[plan.pixel_bytes]);
        dirty_snapshot_bytes_ += plan.pixel_bytes;
        ++dirty_snapshot_allocations_;
        for (const auto& rectangle : plan.rects)
        {
            const auto row_bytes = static_cast<std::size_t>(
                rectangle.width) * sizeof(MaterialId);
            for (std::uint32_t row = 0u;
                 row < rectangle.height; ++row)
            {
                std::memcpy(
                    pixels.get() + rectangle.data_offset +
                        static_cast<std::size_t>(row) * row_bytes,
                    &chunk->cells[cellOrdinal(
                        rectangle.x, rectangle.y + row)],
                    row_bytes);
            }
        }
        result.rects = std::move(plan.rects);
        result.pixels = std::move(pixels);
        result.pixel_bytes = plan.pixel_bytes;
        result.content_revision = plan.content_revision;
        return result;
    }

    void PixelFieldRuntime::confirmExport(
        PixelFieldHandle handle,
        PixelChunkCoord coordinate,
        std::uint64_t revision,
        bool uploaded)
    {
        if (auto* ledger = dirtyLedger(handle, coordinate))
            ledger->onAck(revision, uploaded);
    }

    std::uint64_t PixelFieldRuntime::uploadedRevision(
        PixelFieldHandle handle) const noexcept
    {
        const auto* field = resolve(handle);
        if (!field)
            return 0u;
        std::uint64_t result = 0u;
        for (const auto& [_, chunk] : field->chunks)
            result += chunk.ledger.uploadedRevision();
        return result;
    }
} // namespace lux::ecs
