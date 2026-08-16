#pragma once

#include <lux/engine/ecs/pixel/PixelFieldTypes.hpp>
#include <lux/engine/ecs/pixel/systems/PixelDirtyLedger.hpp>
#include <lux/cxx/algorithm/Sha256.hpp>
#include <lux/engine/function/visibility.h>
#include <lux/engine/meta/LuxObject.hpp>
#include <lux/cxx/compile_time/expected.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <limits>
#include <optional>
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
    struct PixelFieldRuntimeConfig final
    {
        /// 0 selects clamp(hardware_concurrency - 2, 1, 16).
        std::uint32_t parallelism{0u};
    };

    struct PixelFieldRenderExport final
    {
        std::vector<PixelDirtyRect> rects;
        std::shared_ptr<const std::byte[]> pixels;
        std::uint64_t pixel_bytes{0u};
        std::uint64_t content_revision{0u};

        [[nodiscard]] bool empty() const noexcept { return rects.empty(); }
    };

    struct PixelChunkDeltaCell final
    {
        std::uint16_t x{0u};
        std::uint16_t y{0u};
        MaterialId material{kEmptyMaterial};

        friend bool operator==(
            const PixelChunkDeltaCell&,
            const PixelChunkDeltaCell&) = default;
    };

    /// Owning immutable-adoption input. A content worker may decode or
    /// generate this value; only the runtime owner thread calls loadChunk().
    struct PixelChunkLoad final
    {
        PixelChunkCoord coordinate{};
        std::vector<MaterialId> materials;
        std::vector<float> temperature;
        std::vector<float> lifetime;
        lux::cxx::algorithm::Sha256Digest base_digest;
        std::uint64_t sequence{0u};
        std::vector<PixelChunkDeltaCell> delta;
        bool presentation_active{false};
        bool simulation_active{false};
    };

    /// Immutable input captured by the Pixel owner before a chunk is sent to
    /// a worker.  Workers never borrow PixelMaterialRegistry or a live Field.
    struct PixelChunkPreparationContext final
    {
        std::uint32_t channels_mask{0u};
        std::vector<std::uint8_t> blocking_materials;
    };

    enum class EPixelChunkPreparationError : std::uint8_t
    {
        INVALID_CONTEXT,
        INVALID_COORDINATE,
        INVALID_CHANNELS,
        INVALID_DELTA
    };

    class PixelFieldRuntime;

    /// Move-only, fully expanded chunk adoption value.  Its storage is
    /// intentionally opaque: it carries simulation scratch, tile blocking
    /// counts and dirty initial state without making those runtime details a
    /// content or ECS component contract.
    class LUX_FUNCTION_PUBLIC PreparedPixelChunk final
    {
    public:
        PreparedPixelChunk() noexcept;
        ~PreparedPixelChunk();
        PreparedPixelChunk(PreparedPixelChunk&&) noexcept;
        PreparedPixelChunk& operator=(PreparedPixelChunk&&) noexcept;
        PreparedPixelChunk(const PreparedPixelChunk&) = delete;
        PreparedPixelChunk& operator=(const PreparedPixelChunk&) = delete;

        [[nodiscard]] explicit operator bool() const noexcept;
        [[nodiscard]] PixelChunkCoord coordinate() const noexcept;

    private:
        struct Storage;
        explicit PreparedPixelChunk(std::unique_ptr<Storage> storage) noexcept;

        friend class PixelFieldRuntime;
        friend LUX_FUNCTION_PUBLIC lux::cxx::expected<
            PreparedPixelChunk,
            EPixelChunkPreparationError>
        preparePixelChunk(
            PixelChunkLoad load,
            PixelChunkPreparationContext context);

        std::unique_ptr<Storage> storage_;
    };

    /// Pure CPU expansion.  Both arguments are owning values and the result
    /// may be moved to the Pixel owner after background execution.
    [[nodiscard]] LUX_FUNCTION_PUBLIC lux::cxx::expected<
        PreparedPixelChunk,
        EPixelChunkPreparationError>
    preparePixelChunk(
        PixelChunkLoad load,
        PixelChunkPreparationContext context);

    struct PixelChunkSnapshot final
    {
        PixelChunkCoord coordinate{};
        std::vector<MaterialId> materials;
        std::vector<float> temperature;
        std::vector<float> lifetime;
        lux::cxx::algorithm::Sha256Digest base_digest;
        std::uint64_t sequence{0u};
        std::vector<PixelChunkDeltaCell> delta;
        bool presentation_active{false};
        bool simulation_active{false};
    };

    /// Persistence capture is intentionally sparse: unloading a chunk never
    /// copies its 65,536-cell base or transient simulation channels.
    struct PixelChunkDeltaSnapshot final
    {
        PixelChunkCoord coordinate{};
        lux::cxx::algorithm::Sha256Digest base_digest;
        std::uint64_t sequence{0u};
        std::vector<PixelChunkDeltaCell> delta;

        friend bool operator==(
            const PixelChunkDeltaSnapshot&,
            const PixelChunkDeltaSnapshot&) = default;
    };

    struct PixelFieldRuntimeStats final
    {
        std::uint64_t resident_chunks{0u};
        std::uint64_t presentation_active_chunks{0u};
        std::uint64_t simulation_active_chunks{0u};
        std::uint64_t resident_bytes{0u};
        std::uint64_t simulation_chunks_visited_last_step{0u};
        std::uint64_t cells_scanned_last_step{0u};
        std::uint64_t moved_cells_last_step{0u};
        std::uint64_t synchronous_chunk_preparations{0u};
        std::uint64_t prepared_chunk_adoptions{0u};
        std::uint64_t capturing_chunk_unloads{0u};
        std::uint64_t discard_chunk_retires{0u};
    };

    /// Scene-owned sparse Pixel field. Logical size and resident memory are
    /// independent: only explicitly loaded chunks exist, and a fixed step
    /// enumerates only simulation-active resident chunks.
    class LUX_FUNCTION_PUBLIC PixelFieldRuntime final
    {
    public:
        static constexpr std::uint32_t kTileSize = 32u;
        static constexpr std::uint32_t kChunkSizeCells = 256u;
        static constexpr std::uint32_t kChunkShift = 8u;
        static constexpr std::uint32_t kChunkMask = kChunkSizeCells - 1u;
        static constexpr std::uint32_t kTilesPerChunk =
            kChunkSizeCells / kTileSize;
        static constexpr std::uint32_t kTilesPerChunkCount =
            kTilesPerChunk * kTilesPerChunk;
        static constexpr std::size_t kChunkCellCount =
            static_cast<std::size_t>(kChunkSizeCells) * kChunkSizeCells;
        static_assert((1u << kChunkShift) == kChunkSizeCells);
        static_assert(kChunkSizeCells % kTileSize == 0u);

        explicit PixelFieldRuntime(PixelFieldRuntimeConfig config = {});
        ~PixelFieldRuntime();
        PixelFieldRuntime(const PixelFieldRuntime&) = delete;
        PixelFieldRuntime& operator=(const PixelFieldRuntime&) = delete;

        [[nodiscard]] PixelMaterialRegistry& materials() noexcept
        {
            return materials_;
        }
        [[nodiscard]] const PixelMaterialRegistry& materials() const noexcept
        {
            return materials_;
        }

        [[nodiscard]] PixelFieldHandle create(const PixelFieldDesc& desc);
        void destroy(PixelFieldHandle handle);
        [[nodiscard]] bool isAlive(PixelFieldHandle handle) const noexcept;
        [[nodiscard]] PixelFieldDesc desc(
            PixelFieldHandle handle) const noexcept;
        [[nodiscard]] bool updateFrame(
            PixelFieldHandle handle,
            const PixelFieldFrame& frame,
            float priority,
            bool visible,
            bool simulation_enabled) noexcept;
        [[nodiscard]] std::size_t fieldCount() const noexcept
        {
            return fields_.size();
        }

        [[nodiscard]] bool loadChunk(
            PixelFieldHandle handle,
            PixelChunkLoad&& chunk);
        /// Captures all immutable material/channel facts needed by a worker.
        /// The returned value owns its table and never aliases live runtime
        /// state.
        [[nodiscard]] std::optional<PixelChunkPreparationContext>
        chunkPreparationContext(PixelFieldHandle handle) const;
        /// O(1) owning publication of a fully prepared chunk.  It performs no
        /// cell scan and does not allocate per-cell arrays.
        [[nodiscard]] bool adoptPreparedChunk(
            PixelFieldHandle handle,
            PreparedPixelChunk&& chunk);
        [[nodiscard]] bool captureChunk(
            PixelFieldHandle handle,
            PixelChunkCoord coordinate,
            PixelChunkSnapshot& output) const;
        [[nodiscard]] bool captureChunkDelta(
            PixelFieldHandle handle,
            PixelChunkCoord coordinate,
            PixelChunkDeltaSnapshot& output) const;
        [[nodiscard]] bool unloadChunk(
            PixelFieldHandle handle,
            PixelChunkCoord coordinate,
            PixelChunkSnapshot& output);
        /// Removes a resident chunk without materializing a persistence
        /// snapshot.  Persistence owners must explicitly call unloadChunk().
        [[nodiscard]] bool discardChunk(
            PixelFieldHandle handle,
            PixelChunkCoord coordinate);
        [[nodiscard]] bool setChunkSimulationActive(
            PixelFieldHandle handle,
            PixelChunkCoord coordinate,
            bool active) noexcept;
        [[nodiscard]] bool setChunkPresentationActive(
            PixelFieldHandle handle,
            PixelChunkCoord coordinate,
            bool active) noexcept;
        [[nodiscard]] bool applyChunkDelta(
            PixelFieldHandle handle,
            PixelChunkCoord coordinate,
            const lux::cxx::algorithm::Sha256Digest& base_digest,
            std::uint64_t sequence,
            std::span<const PixelChunkDeltaCell> delta) noexcept;
        [[nodiscard]] std::optional<lux::cxx::algorithm::Sha256Digest>
        chunkBaseDigest(
            PixelFieldHandle handle,
            PixelChunkCoord coordinate) const noexcept;
        [[nodiscard]] bool chunkResident(
            PixelFieldHandle handle,
            PixelChunkCoord coordinate) const noexcept;
        void residentChunks(
            PixelFieldHandle handle,
            std::vector<PixelChunkCoord>& output) const;
        /// Allocation-free view of simulation-active chunks. The order is
        /// unspecified and the span is invalidated by chunk residency or
        /// activity changes for this field.
        [[nodiscard]] std::span<const PixelChunkCoord> activeKeys(
            PixelFieldHandle handle) const noexcept;
        /// Allocation-free render/presentation membership. Residency and
        /// simulation are independent: a halo can remain resident while both
        /// public activity sets exclude it.
        [[nodiscard]] std::span<const PixelChunkCoord> presentationKeys(
            PixelFieldHandle handle) const noexcept;
        [[nodiscard]] bool chunkSimulationActive(
            PixelFieldHandle handle,
            PixelChunkCoord coordinate) const noexcept;

        void applyCommands();
        void step();
        void enqueue(const PixelFieldCommand& command)
        {
            commands_.push_back(command);
        }
        void drainEvents(std::vector<PixelFieldEvent>& output)
        {
            output.insert(output.end(), events_.begin(), events_.end());
            events_.clear();
        }

        /// Missing chunks inside the logical field are Solid Boundary.
        [[nodiscard]] bool regionBlocked(
            PixelFieldHandle handle,
            PixelCellCoord minimum,
            PixelCellCoord maximum) const noexcept;

        void queryFields(
            const lux::spatial::Position2D& minimum,
            const lux::spatial::Position2D& maximum,
            std::vector<PixelFieldQueryEntry>& output) const;

        [[nodiscard]] std::uint64_t determinismHash(
            PixelFieldHandle handle) const noexcept;
        [[nodiscard]] std::uint32_t movedCellsLastStep(
            PixelFieldHandle handle) const noexcept;
        [[nodiscard]] std::uint32_t cellsScannedLastStep(
            PixelFieldHandle handle) const noexcept;
        [[nodiscard]] std::uint32_t activeTiles(
            PixelFieldHandle handle) const noexcept;
        [[nodiscard]] double stepMillisLast(
            PixelFieldHandle handle) const noexcept;
        [[nodiscard]] std::uint64_t eventsDropped() const noexcept
        {
            return events_dropped_;
        }
        [[nodiscard]] std::uint64_t scratchGrowthCount() const noexcept;
        [[nodiscard]] std::uint64_t dirtySnapshotBytes() const noexcept;
        [[nodiscard]] std::uint64_t dirtySnapshotAllocations() const noexcept;
        [[nodiscard]] PixelFieldRuntimeStats stats() const noexcept;

        [[nodiscard]] PixelDirtyLedger* dirtyLedger(
            PixelFieldHandle handle,
            PixelChunkCoord coordinate) noexcept;
        [[nodiscard]] PixelFieldRenderExport exportDirty(
            PixelFieldHandle handle,
            PixelChunkCoord coordinate,
            const PixelExportBudget& budget);
        void confirmExport(
            PixelFieldHandle handle,
            PixelChunkCoord coordinate,
            std::uint64_t revision,
            bool uploaded);
        [[nodiscard]] std::uint64_t uploadedRevision(
            PixelFieldHandle handle) const noexcept;

    private:
        struct Chunk final
        {
            std::vector<MaterialId> cells;
            std::vector<std::uint8_t> moved;
            PixelDirtyLedger ledger{kChunkSizeCells, kChunkSizeCells};
            std::vector<float> temperature;
            std::vector<float> lifetime;
            std::array<std::uint8_t, kTilesPerChunkCount> active{};
            std::array<std::uint8_t, kTilesPerChunkCount> active_next{};
            std::array<std::uint8_t, kTilesPerChunkCount> changed{};
            std::array<std::uint8_t, kTilesPerChunkCount> minimum_x{};
            std::array<std::uint8_t, kTilesPerChunkCount> minimum_y{};
            std::array<std::uint8_t, kTilesPerChunkCount> maximum_x{};
            std::array<std::uint8_t, kTilesPerChunkCount> maximum_y{};
            std::array<std::uint8_t, kTilesPerChunkCount> next_minimum_x{};
            std::array<std::uint8_t, kTilesPerChunkCount> next_minimum_y{};
            std::array<std::uint8_t, kTilesPerChunkCount> next_maximum_x{};
            std::array<std::uint8_t, kTilesPerChunkCount> next_maximum_y{};
            std::array<std::uint16_t, kTilesPerChunkCount> blocking{};
            lux::cxx::algorithm::Sha256Digest base_digest;
            std::uint64_t sequence{0u};
            std::unordered_map<std::uint16_t, MaterialId> delta;
            bool presentation_active{false};
            bool simulation_active{false};

            [[nodiscard]] bool hasActiveTiles() const noexcept;
        };

        struct Field final
        {
            using ActiveChunks = detail::SparseActiveMap<
                PixelChunkCoord,
                PixelChunkCoordHash,
                PixelChunkCoordEqual>;

            Field();
            ~Field();
            Field(Field&&) noexcept;
            Field& operator=(Field&&) noexcept;
            Field(const Field&) = delete;
            Field& operator=(const Field&) = delete;

            PixelFieldHandle handle{};
            PixelFieldDesc desc{};
            std::unordered_map<PixelChunkCoord, Chunk, PixelChunkCoordHash>
                chunks;
            // Edge-maintained dense index used by the fixed-step simulation.
            // Resident-but-inactive chunks never participate in a per-tick scan.
            std::unique_ptr<ActiveChunks> active_chunks;
            std::unique_ptr<ActiveChunks> presentation_chunks;
            std::uint32_t moved_cells_last{0u};
            std::uint32_t cells_scanned_last{0u};
            std::uint32_t chunks_visited_last{0u};
            double step_ms_last{0.0};
            std::uint64_t steps{0u};
            PixelFieldFrame frame{};
            float frame_priority{0.0f};
            bool frame_valid{false};
            bool visible{true};
            bool simulation_enabled{true};
            std::size_t simulation_dense_index{
                std::numeric_limits<std::size_t>::max()};
        };

        struct StepArena;

        [[nodiscard]] Field* resolve(PixelFieldHandle handle) noexcept;
        [[nodiscard]] const Field* resolve(
            PixelFieldHandle handle) const noexcept;
        [[nodiscard]] static bool coordinateAllowed(
            const Field& field,
            PixelChunkCoord coordinate) noexcept;
        [[nodiscard]] static PixelChunkCoord chunkOf(
            PixelCellCoord coordinate) noexcept;
        [[nodiscard]] static std::uint32_t localCoordinate(
            std::int64_t coordinate) noexcept;
        [[nodiscard]] static std::uint16_t cellOrdinal(
            std::uint32_t x,
            std::uint32_t y) noexcept;
        [[nodiscard]] static std::size_t tileOrdinal(
            std::uint32_t x,
            std::uint32_t y) noexcept;
        [[nodiscard]] Chunk* chunkAt(
            Field& field,
            PixelChunkCoord coordinate) noexcept;
        [[nodiscard]] const Chunk* chunkAt(
            const Field& field,
            PixelChunkCoord coordinate) const noexcept;
        [[nodiscard]] Chunk* chunkAt(
            Field& field,
            PixelCellCoord coordinate) noexcept;
        [[nodiscard]] const Chunk* chunkAt(
            const Field& field,
            PixelCellCoord coordinate) const noexcept;
        static void addActiveChunk(
            Field& field,
            PixelChunkCoord coordinate,
            Chunk& chunk) noexcept;
        static void removeActiveChunk(
            Field& field,
            PixelChunkCoord coordinate,
            Chunk& chunk) noexcept;
        [[nodiscard]] MaterialId* cellAt(
            Field& field,
            PixelCellCoord coordinate) noexcept;
        [[nodiscard]] const MaterialId* cellAt(
            const Field& field,
            PixelCellCoord coordinate) const noexcept;
        [[nodiscard]] std::uint8_t* movedAt(
            Field& field,
            PixelCellCoord coordinate) noexcept;

        void destroySlot(PixelFieldHandle handle);
        void setFieldSimulationEnabled(
            Field& field,
            bool enabled) noexcept;
        void pushEvent(const PixelFieldEvent& event);
        void stepField(Field& field);
        void stepChunk(
            Field& field,
            PixelChunkCoord coordinate,
            Chunk& chunk,
            std::uint32_t& moved,
            std::uint32_t& scanned);
        void wakeSpan(
            Field& field,
            PixelCellCoord minimum,
            PixelCellCoord maximum,
            bool next);
        bool tryMove(
            Field& field,
            PixelCellCoord source,
            PixelCellCoord target,
            bool displace_liquid,
            std::uint32_t& moved_counter);
        void markCellChanged(
            Field& field,
            PixelCellCoord coordinate,
            MaterialId material) noexcept;

        static constexpr std::size_t kMaxPendingEvents = 4096u;

        std::unique_ptr<StepArena> parallel_;
        std::uint64_t dirty_snapshot_bytes_{0u};
        std::uint64_t dirty_snapshot_allocations_{0u};
        std::uint32_t parallelism_{1u};
        PixelMaterialRegistry materials_;
        lux::cxx::SlotMap<Field, PixelFieldTag> fields_;
        std::vector<PixelFieldHandle> simulation_fields_;
        std::vector<PixelFieldCommand> commands_;
        std::vector<PixelFieldEvent> events_;
        std::uint64_t events_dropped_{0u};
        std::uint64_t synchronous_chunk_preparations_{0u};
        std::uint64_t prepared_chunk_adoptions_{0u};
        std::uint64_t capturing_chunk_unloads_{0u};
        std::uint64_t discard_chunk_retires_{0u};
    };
} // namespace lux::ecs
