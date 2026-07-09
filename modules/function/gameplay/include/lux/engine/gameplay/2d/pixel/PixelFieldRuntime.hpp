#pragma once
// ============================================================================
//  PixelFieldRuntime.hpp — the scene-scoped pixel-field service
//  (lux::gameplay::d2, F2-01/F2-04/F2-05/F2-06).
//
//  Owns 0..N fields OUTSIDE the ECS (slot map, generational handles): dense
//  single-screen cell grids (chunking is C2-*), the deterministic CA step, the
//  active/dirty/resident tile state and the query/command/event boundary.
//
//  DETERMINISM (F2-04, frozen): single-threaded, strict alternating in-place
//  scan — tiles bottom-up, rows bottom-up within a tile, x direction alternating
//  by (step + row) parity; a per-step moved-mask prevents double-moves. Every
//  move targets an Empty cell (or displaces a lighter liquid by swap), so a
//  target cell accepts at most one write per step BY CONSTRUCTION (sequential
//  scan = first-come ownership). Same initial state + same command sequence ⇒
//  identical cells every step, independent of anything but the rules.
//
//  TILE STATE (F2-05): kTileSize² tiles carry `sim_active` (scan skips sleeping
//  tiles; any move wakes the source/target tiles + their neighbours for the
//  NEXT step) and feed `upload_dirty` into a per-field RegionUploadPlanner
//  (U2-03) — the SAME defer-never-lose / ack-only-advance machinery the texture
//  path already proved. `resident` is uniformly true in the single-screen MVP
//  (the API seam arrives with C2-*). Planner coordinates are CELL coordinates
//  (row 0 = bottom); the render export decides the texture mapping.
//
//  BOUNDARY (F2-06): queries are const and never mutate; ALL writes are
//  commands, applied FIFO at the ApplyFieldCommands fixed phase (never
//  mid-scan); events report facts only. No API returns internal pointers or
//  writable spans.
//
//  Ownership: fields are owned by exactly one PixelField2DComponent. The
//  runtime's owner maintenance VALUE-SCANS the component view each fixed step
//  (the no-trust axiom — no destroy discipline required) and reclaims any live
//  field no component references; destroying the owner entity therefore
//  destroys the field, leak-proof. Wired into Simulation2DSystem's
//  ApplyFieldCommands/SimulateFields phases by d2::install().
// ============================================================================

#include <lux/engine/gameplay/2d/pixel/PixelFieldTypes.hpp>
#include <lux/engine/meta/LuxObject.hpp>                              // EntityRegistry
#include <lux/engine/render/resources/ops/RegionUploadPlanner.hpp>    // upload_dirty bookkeeping (U2-03)
#include <lux/engine/function/visibility.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace lux::gameplay::d2
{
    /// F2-07: one frame's upload snapshot for ONE field — an OWNED, immutable
    /// copy of the dirty regions' pixels (u16 material ids, tight rows at each
    /// region's data_offset), so the runtime keeps simulating freely after the
    /// export. The consumer (PixelField2DBridge) wraps it in an
    /// OwnedTextureUploadBatch with ITS texture handle and MUST route the
    /// outcome back via confirmExport — Ok advances uploadedRevision, anything
    /// else re-marks the coverage dirty (defer-never-lose); a ticket it failed
    /// to submit at all is confirmed with a non-Ok status for the same reason.
    struct PixelFieldRenderExport
    {
        std::vector<lux::render::TextureRegionDesc> regions;   ///< CELL coordinates (row 0 = bottom)
        std::shared_ptr<const std::byte[]>          pixels;    ///< owned tight-packed u16 ids
        std::uint64_t                               pixel_bytes{0};
        std::uint64_t                               content_revision{0};
        [[nodiscard]] bool empty() const noexcept { return regions.empty(); }
    };

    class LUX_FUNCTION_PUBLIC PixelFieldRuntime
    {
    public:
        static constexpr std::uint32_t kTileSize = 32;

        PixelFieldRuntime() = default;
        PixelFieldRuntime(const PixelFieldRuntime&) = delete;
        PixelFieldRuntime& operator=(const PixelFieldRuntime&) = delete;

        /// The material table (define materials BEFORE stamping them; append-only).
        [[nodiscard]] PixelMaterialRegistry&       materials() noexcept       { return materials_; }
        [[nodiscard]] const PixelMaterialRegistry& materials() const noexcept { return materials_; }

        // ── lifecycle (F2-00/F2-01) ─────────────────────────────────────────
        [[nodiscard]] PixelFieldHandle create(const PixelFieldDesc& desc);
        void destroy(PixelFieldHandle h);                 // stale-safe; emits FieldDestroyed
        [[nodiscard]] bool isAlive(PixelFieldHandle h) const noexcept;
        [[nodiscard]] PixelFieldDesc desc(PixelFieldHandle h) const noexcept;   // zeroed if stale
        [[nodiscard]] bool channelEnabled(PixelFieldHandle h, ECellChannel c) const noexcept;

        /// Owner maintenance: destroy every live field that no live
        /// PixelField2DComponent references (value-scan, no destroy discipline).
        void maintainOwners(lux::meta::EntityRegistry& registry);

        // ── fixed-step phases (wired by d2::install) ────────────────────────
        void applyCommands();   ///< Phase::ApplyFieldCommands — FIFO, then clears the queue
        void step();            ///< Phase::SimulateFields — one deterministic CA substep

        // ── F2-06 boundary ──────────────────────────────────────────────────
        void enqueue(const PixelFieldCommand& cmd) { commands_.push_back(cmd); }
        [[nodiscard]] MaterialId samplePoint(PixelFieldHandle h, Eigen::Vector2i cell) const noexcept;
        /// Row-major COPY of [min, min+size) (clamped; out-of-bounds reads Empty).
        void sampleRegion(PixelFieldHandle h, Eigen::Vector2i min, Eigen::Vector2i size,
                          std::vector<MaterialId>& out) const;
        /// March from @p from_cell along @p dir (cell space) to the first
        /// non-empty cell within @p max_cells. MVP fixed-increment march.
        [[nodiscard]] PixelRaycastHit raycast(PixelFieldHandle h, Eigen::Vector2f from_cell,
                                              Eigen::Vector2f dir, float max_cells) const;
        void drainEvents(std::vector<PixelFieldEvent>& out)
        {
            out.insert(out.end(), events_.begin(), events_.end());
            events_.clear();
        }

        // ── observability + the F2-07 export seam ───────────────────────────
        [[nodiscard]] std::uint64_t determinismHash(PixelFieldHandle h) const noexcept;
        [[nodiscard]] std::uint32_t movedCellsLastStep(PixelFieldHandle h) const noexcept;
        /// Cells VISITED by the last scan (the dirty-rect refinement's receipt:
        /// a lone disturbance in a huge settled world scans a tiny band, not
        /// every cell of every active tile).
        [[nodiscard]] std::uint32_t cellsScannedLastStep(PixelFieldHandle h) const noexcept;
        [[nodiscard]] std::uint32_t activeTiles(PixelFieldHandle h) const noexcept;
        [[nodiscard]] double        stepMillisLast(PixelFieldHandle h) const noexcept;
        /// Events dropped by the pending-queue cap (a consumer that never drains
        /// cannot grow the queue without bound; drops are counted, not silent).
        [[nodiscard]] std::uint64_t eventsDropped() const noexcept { return events_dropped_; }
        /// The field's upload-dirty planner (CELL coordinates). The render export
        /// (F2-07) takes batches from it and routes acks back; tests inspect it.
        /// Null when the handle is stale.
        [[nodiscard]] lux::render::RegionUploadPlanner* uploadPlanner(PixelFieldHandle h) noexcept;

        // ── F2-07: render export ticket ──────────────────────────────────────
        /// Take one budgeted upload snapshot (empty when nothing is dirty / the
        /// handle is stale). The returned pixels are an OWNED copy — the CA may
        /// keep stepping immediately.
        [[nodiscard]] PixelFieldRenderExport exportDirty(PixelFieldHandle h,
                                                         const lux::render::RegionUploadBudget& budget);
        /// Route the submit outcome back (the wire reply's status, or a non-Ok
        /// status for a ticket that was never submitted). Ok is the ONLY thing
        /// that advances uploadedRevision; anything else re-dirties the coverage.
        void confirmExport(PixelFieldHandle h, std::uint64_t revision,
                           lux::render::ERegionUploadStatus status);
        [[nodiscard]] std::uint64_t uploadedRevision(PixelFieldHandle h) const noexcept;

    private:
        struct Field
        {
            std::uint32_t gen{0};
            bool          alive{false};
            PixelFieldDesc desc{};
            std::vector<MaterialId>  cells;         ///< w*h, row-major, row 0 = BOTTOM
            std::vector<std::uint8_t> moved;        ///< per-cell moved-this-step mask
            // tiles (F2-05 + the Noita-style per-tile DIRTY RECT refinement):
            // `active` gates the tile; the rect bounds WHICH CELLS inside it are
            // scanned (within-tile inclusive coords) — a mountain's sleeping
            // interior is never visited, only its ±1-expanded activity band.
            std::uint32_t tiles_x{0}, tiles_y{0};
            std::vector<std::uint8_t> active;       ///< sim_active (this step's scan set)
            std::vector<std::uint8_t> active_next;  ///< woken for the next step
            std::vector<std::uint8_t> changed;      ///< per-step upload-dirty accumulation
            std::vector<std::uint8_t> rmin_x, rmin_y, rmax_x, rmax_y;     ///< current scan rects
            std::vector<std::uint8_t> nrmin_x, nrmin_y, nrmax_x, nrmax_y; ///< next-step rects
            lux::render::RegionUploadPlanner planner{0, 0};
            // optional channels (allocated iff enabled — F2-03)
            std::vector<float> temperature;
            std::vector<float> lifetime;
            // stats
            std::uint32_t moved_cells_last{0};
            std::uint32_t cells_scanned_last{0};
            double        step_ms_last{0.0};
            std::uint64_t steps{0};
        };

        [[nodiscard]] Field*       resolve(PixelFieldHandle h) noexcept;
        [[nodiscard]] const Field* resolve(PixelFieldHandle h) const noexcept;
        void destroySlot(std::uint32_t slot);
        void stepField(Field& f);
        /// Wake every tile overlapping the INCLUSIVE cell span [x0,x1]×[y0,y1]
        /// (clamped) in the given active mask, unioning each tile's scan rect
        /// with its slice of the span. @p next selects the next-step buffers.
        void wakeSpan(Field& f, std::int32_t x0, std::int32_t y0,
                      std::int32_t x1, std::int32_t y1, bool next);
        /// Append an event under the pending cap (drops oldest half + counts
        /// when a consumer never drains — bounded by construction).
        void pushEvent(const PixelFieldEvent& e);
        /// Move/displace (x,y)→(nx,ny). True when something moved (single-write
        /// by construction: the target is Empty or a displaced lighter liquid).
        bool tryMove(Field& f, std::uint32_t x, std::uint32_t y,
                     std::int32_t nx, std::int32_t ny, bool displace_liquid);

        static constexpr std::size_t kMaxPendingEvents = 4096;   ///< un-drained queue cap

        PixelMaterialRegistry          materials_;
        std::vector<Field>             fields_;
        std::vector<std::uint32_t>     free_;
        std::vector<PixelFieldCommand> commands_;
        std::vector<PixelFieldEvent>   events_;
        std::uint64_t                  events_dropped_{0};
        std::vector<std::uint8_t>      owner_scratch_;   ///< maintainOwners referenced-set
    };

} // namespace lux::gameplay::d2
