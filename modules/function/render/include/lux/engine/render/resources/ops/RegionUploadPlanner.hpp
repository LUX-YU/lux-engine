#pragma once
// ============================================================================
//  RegionUploadPlanner.hpp — client-side dirty-rect → region-batch planning
//  (U2-03: coalescing, per-frame budget with DEFER-not-drop, and the
//  uploaded_revision confirmation bookkeeping).
//
//  One planner tracks ONE surface (one mip-0 layer of a persistent texture —
//  a mipped/layered producer composes one planner per subresource; nothing
//  here speculates about that). The producer owns the AUTHORITATIVE pixels
//  (e.g. a pixel-field chunk's CellStorage): it marks dirty rects as it
//  mutates, then once per frame:
//
//      auto plan = planner.takeBatch(texel_bytes, budget);
//      if (!plan.empty()) {
//          <pack plan.regions' pixels from the authoritative surface,
//           tight rows, at each region's data_offset>          // planner assigned
//          batch = { dst, plan.content_revision, plan.regions, pixels };
//          session.updateTextureRegions(batch)
//                 .then([&](auto& r){ planner.onAck(r.content_revision,
//                                     ERegionUploadStatus(r.status)); });
//      }
//
//  Contracts (the U2-03 checklist, verbatim):
//   - COALESCE: touching/overlapping rects merge when the union's bounding box
//     wastes ≤ 25% over the covered area — so scattered specks never balloon
//     into an unreasonable full-image upload, while tile-bitset extractions
//     (rows of adjacent tiles) collapse to a handful of rectangles.
//   - BUDGET: a per-takeBatch byte/region cap; what does not fit STAYS DIRTY
//     and is emitted by a later takeBatch — deferred, never lost. An oversized
//     single rect is row-SPLIT so a tiny budget still makes forward progress.
//   - REVISION: every non-empty plan carries a fresh content_revision and its
//     coverage is held in flight; onAck(revision, Ok) advances
//     uploadedRevision() — the ONLY thing that does — and any other status
//     re-marks the coverage dirty, so one failed/refused enqueue can never
//     permanently lose dirty data.
//   - STATS: queued bytes / merged regions / deferred regions per takeBatch
//     (lastStats()), the observability the acceptance names.
//
//  Pure CPU logic, deliberately header-only and device-free: the arithmetic is
//  pinned headless, and both the U2-04 synthetic slice and the F2 pixel-field
//  bridge consume it unchanged.
// ============================================================================

#include <lux/engine/render/resources/ops/TextureResourceOperation.hpp>   // TextureRegionDesc / ERegionUploadStatus

#include <algorithm>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace lux::render
{
    /// Per-takeBatch caps. 0 = unlimited for either field.
    struct RegionUploadBudget
    {
        std::uint64_t max_bytes_per_frame{0};
        std::uint32_t max_regions_per_frame{0};
    };

    /// What the last takeBatch did (the U2-03 observability acceptance).
    struct RegionUploadStats
    {
        std::uint64_t queued_bytes{0};      ///< tight pixel bytes the plan carries
        std::uint32_t merged_regions{0};    ///< dirty rects eliminated by coalescing
        std::uint32_t deferred_regions{0};  ///< rects left dirty by the budget cap
    };

    class RegionUploadPlanner
    {
    public:
        RegionUploadPlanner(std::uint32_t surface_width, std::uint32_t surface_height) noexcept
            : width_(surface_width), height_(surface_height) {}

        /// Mark a mutated rect (surface texels). Clamped to the surface; empty
        /// after clamping is ignored. Cheap — coalescing happens at takeBatch.
        void markDirty(std::uint32_t x, std::uint32_t y, std::uint32_t w, std::uint32_t h)
        {
            if (x >= width_ || y >= height_) return;
            const std::uint32_t cw = static_cast<std::uint32_t>(std::min<std::uint64_t>(w, width_  - x));
            const std::uint32_t ch = static_cast<std::uint32_t>(std::min<std::uint64_t>(h, height_ - y));
            if (cw == 0 || ch == 0) return;
            dirty_.push_back(Rect{x, y, cw, ch});
        }

        [[nodiscard]] bool hasDirty() const noexcept { return !dirty_.empty(); }

        /// One frame's upload plan: coalesced, budgeted, tight-packed offsets
        /// assigned in region order (pack pixels in that same order). An empty
        /// plan (nothing dirty, or a zero budget refusing even a split row)
        /// bumps NO revision and holds nothing in flight.
        struct Plan
        {
            std::vector<TextureRegionDesc> regions;   ///< data_offset assigned; row_pitch 0 (tight)
            std::uint64_t content_revision{0};
            std::uint64_t pixel_bytes{0};             ///< total tight bytes to pack
            [[nodiscard]] bool empty() const noexcept { return regions.empty(); }
        };

        [[nodiscard]] Plan takeBatch(std::uint32_t texel_bytes, const RegionUploadBudget& budget)
        {
            stats_ = {};
            Plan plan;
            if (dirty_.empty() || texel_bytes == 0)
                return plan;

            coalesce();

            // Deterministic emission order (top-left first) — also the pack order.
            std::sort(dirty_.begin(), dirty_.end(), [](const Rect& a, const Rect& b) {
                return a.y != b.y ? a.y < b.y : (a.x != b.x ? a.x < b.x : a.w < b.w);
            });

            std::vector<Rect> taken;
            std::uint64_t     bytes = 0;
            std::size_t       i     = 0;
            for (; i < dirty_.size(); ++i)
            {
                const Rect&        r       = dirty_[i];
                const std::uint64_t r_bytes = std::uint64_t{r.w} * r.h * texel_bytes;
                const bool byte_room   = budget.max_bytes_per_frame == 0
                                      || bytes + r_bytes <= budget.max_bytes_per_frame;
                const bool region_room = budget.max_regions_per_frame == 0
                                      || taken.size() < budget.max_regions_per_frame;
                if (!region_room)
                    break;
                if (byte_room)
                {
                    taken.push_back(r);
                    bytes += r_bytes;
                    continue;
                }
                // Byte budget hit. If NOTHING was taken yet, row-split this rect so a
                // tiny budget still makes forward progress (defer, never starve).
                if (taken.empty() && budget.max_bytes_per_frame > 0)
                {
                    const std::uint64_t row_bytes = std::uint64_t{r.w} * texel_bytes;
                    const std::uint32_t rows =
                        row_bytes ? static_cast<std::uint32_t>(
                                        std::min<std::uint64_t>(budget.max_bytes_per_frame / row_bytes, r.h))
                                  : 0u;
                    if (rows > 0)
                    {
                        taken.push_back(Rect{r.x, r.y, r.w, rows});
                        bytes += rows * row_bytes;
                        dirty_[i] = Rect{r.x, r.y + rows, r.w, r.h - rows};   // remainder stays dirty
                    }
                }
                break;
            }
            // Everything from i on stays dirty for a later frame (DEFERRED, not lost);
            // a row-split remainder sits at index i and is counted with them.
            stats_.deferred_regions = static_cast<std::uint32_t>(dirty_.size() - i);
            dirty_.erase(dirty_.begin(), dirty_.begin() + static_cast<std::ptrdiff_t>(i));

            if (taken.empty())
                return plan;

            plan.content_revision = ++next_revision_;
            plan.regions.reserve(taken.size());
            std::uint64_t offset = 0;
            for (const Rect& r : taken)
            {
                TextureRegionDesc d{};
                d.x = r.x; d.y = r.y; d.width = r.w; d.height = r.h;
                d.row_pitch_bytes = 0;                                   // tight
                d.data_offset     = static_cast<std::uint32_t>(offset);
                plan.regions.push_back(d);
                offset += std::uint64_t{r.w} * r.h * texel_bytes;
            }
            plan.pixel_bytes    = offset;
            stats_.queued_bytes = offset;

            in_flight_.emplace(plan.content_revision, std::move(taken));
            return plan;
        }

        /// Feed every reply back (the .then continuation). Ok is the ONLY thing
        /// that advances uploadedRevision(); any refusal re-marks the batch's
        /// coverage dirty so the next takeBatch retries it.
        void onAck(std::uint64_t revision, ERegionUploadStatus status)
        {
            const auto it = in_flight_.find(revision);
            if (it == in_flight_.end())
                return;                                   // unknown/duplicate ack
            if (status == ERegionUploadStatus::Ok)
                uploaded_revision_ = std::max(uploaded_revision_, revision);
            else
                dirty_.insert(dirty_.end(), it->second.begin(), it->second.end());
            in_flight_.erase(it);
        }

        [[nodiscard]] std::uint64_t uploadedRevision() const noexcept { return uploaded_revision_; }
        [[nodiscard]] bool hasInFlight() const noexcept { return !in_flight_.empty(); }
        [[nodiscard]] const RegionUploadStats& lastStats() const noexcept { return stats_; }

    private:
        struct Rect { std::uint32_t x{0}, y{0}, w{0}, h{0}; };

        static bool touchOrOverlap(const Rect& a, const Rect& b) noexcept
        {
            // Touching counts (shared edge merges into one copy region).
            return a.x <= b.x + b.w && b.x <= a.x + a.w &&
                   a.y <= b.y + b.h && b.y <= a.y + a.h;
        }
        static Rect unionOf(const Rect& a, const Rect& b) noexcept
        {
            const std::uint32_t x0 = std::min(a.x, b.x);
            const std::uint32_t y0 = std::min(a.y, b.y);
            const std::uint32_t x1 = std::max(a.x + a.w, b.x + b.w);
            const std::uint32_t y1 = std::max(a.y + a.h, b.y + b.h);
            return Rect{x0, y0, x1 - x0, y1 - y0};
        }
        static std::uint64_t area(const Rect& r) noexcept { return std::uint64_t{r.w} * r.h; }
        static std::uint64_t overlapArea(const Rect& a, const Rect& b) noexcept
        {
            const std::uint64_t x0 = std::max(a.x, b.x), x1 = std::min(a.x + a.w, b.x + b.w);
            const std::uint64_t y0 = std::max(a.y, b.y), y1 = std::min(a.y + a.h, b.y + b.h);
            return (x1 > x0 && y1 > y0) ? (x1 - x0) * (y1 - y0) : 0;
        }

        /// Pairwise fixpoint merge with the 25% waste bound: touching/overlapping
        /// rects whose union bbox stays within 5/4 of the covered area collapse.
        /// Never merges far-apart rects (their union would blow the bound), so
        /// two dirty specks in opposite corners can never become a full-image
        /// upload (the checklist's explicit prohibition).
        void coalesce()
        {
            const std::uint32_t before = static_cast<std::uint32_t>(dirty_.size());
            bool merged = true;
            while (merged)
            {
                merged = false;
                for (std::size_t a = 0; a < dirty_.size() && !merged; ++a)
                    for (std::size_t b = a + 1; b < dirty_.size() && !merged; ++b)
                    {
                        if (!touchOrOverlap(dirty_[a], dirty_[b]))
                            continue;
                        const Rect u = unionOf(dirty_[a], dirty_[b]);
                        const std::uint64_t covered =
                            area(dirty_[a]) + area(dirty_[b]) - overlapArea(dirty_[a], dirty_[b]);
                        if (area(u) * 4 <= covered * 5)   // waste ≤ 25%
                        {
                            dirty_[a] = u;
                            dirty_[b] = dirty_.back();
                            dirty_.pop_back();
                            merged = true;
                        }
                    }
            }
            stats_.merged_regions = before - static_cast<std::uint32_t>(dirty_.size());
        }

        std::uint32_t width_{0};
        std::uint32_t height_{0};
        std::vector<Rect> dirty_;
        std::unordered_map<std::uint64_t, std::vector<Rect>> in_flight_;   ///< revision → coverage
        std::uint64_t next_revision_{0};
        std::uint64_t uploaded_revision_{0};
        RegionUploadStats stats_{};
    };

} // namespace lux::render
