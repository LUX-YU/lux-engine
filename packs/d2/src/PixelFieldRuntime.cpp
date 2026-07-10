// ============================================================================
//  PixelFieldRuntime.cpp — deterministic chunked pixel simulation
//  (F2-01 lifecycle, F2-04 CA, F2-05 tile state, F2-06 boundary,
//   C2-00 chunked storage — all chunks resident, P3a).
//  Contracts and the scan-order determinism argument live in the header.
//
//  C2-00 NOTE: cell/moved/channel storage is per-chunk (256², uniform blocks;
//  the header's cellAt/movedAt access layer hides the split — rule code is
//  UNCHANGED in logic). The CA scans the same fixed global order as before
//  (tiles bottom-up, dirty-rect refined), reads/writes across chunk borders by
//  plain global addressing (no halo copies — correctness needs none while the
//  step is serial; I2-02's parallel scheduling adds the write-reach guard).
//  Upload dirt routes to the owning chunk's planner in CHUNK-LOCAL coords —
//  the render mirror is one texture per chunk.
// ============================================================================

#include <lux/pack/d2/pixel/PixelFieldRuntime.hpp>
#include <lux/pack/d2/pixel/PixelField2DComponent.hpp>
#include <lux/pack/d2/world/components/WorldTransform2DComponent.hpp>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <functional>
#include <mutex>
#include <thread>

namespace lux::pack
{
    namespace
    {
        [[nodiscard]] constexpr std::uint32_t tileCount(std::uint32_t cells, std::uint32_t tile) noexcept
        {
            return (cells + tile - 1u) / tile;
        }

        /// How far a liquid scans for a drainage hole per step (cells). Larger =
        /// flatter equilibrium surfaces (terraces at most this wide), same
        /// determinism; a rule knob, not a correctness one.
        constexpr std::int32_t kLiquidDispersion = 8;

        /// Free-fall speed in cells per step (constant TERMINAL velocity — a
        /// falling cell drops through up to this many EMPTY cells at once).
        /// 1 cell/step reads as slow-motion sinking at world scale (user
        /// feedback 2026-07-10); a constant multi-cell drop fixes the feel
        /// with ZERO per-cell state — no schema/save/determinism surface.
        /// True acceleration (per-cell velocity, Noita-style) is a recorded
        /// future line: it needs save schema v2 + restore-continuation rework.
        /// Displacement (sinking through liquid) stays 1 cell/step — drag.
        /// Bounded by the four-colour guard in stepField (reach ≤ one tile).
        constexpr std::int32_t kMaxFallCells = 4;

        /// FNV-1a 64 running hash over a contiguous block.
        [[nodiscard]] std::uint64_t fnv1a64Append(std::uint64_t h, const void* data,
                                                  std::size_t bytes) noexcept
        {
            const auto* p = static_cast<const std::uint8_t*>(data);
            for (std::size_t i = 0; i < bytes; ++i)
            {
                h ^= p[i];
                h *= 1099511628211ull;
            }
            return h;
        }
        constexpr std::uint64_t kFnvBasis = 1469598103934665603ull;

        /// I2-00: does this material BLOCK movement/collision? Solid + Powder
        /// (a character stands on sand); liquids and empty do not.
        [[nodiscard]] bool isBlocking(const PixelMaterialRegistry& mats, MaterialId id) noexcept
        {
            if (id == kEmptyMaterial) return false;
            const auto phase = mats.at(id).phase;
            return phase == EMaterialPhase::Solid || phase == EMaterialPhase::Powder;
        }
    } // namespace

    // ── I2-02: the persistent CA worker pool ─────────────────────────────────
    //
    // Deliberately minimal (a falling-sand step is a fixed fan-out/join, not a
    // task graph): N sleeping threads, one job at a time, atomic work-stealing
    // over the item index. Threads exist ONLY while worker_threads_ > 1 —
    // setWorkerThreads(1) tears the pool down and the step path degenerates to
    // the pure serial loop with zero synchronisation.
    struct PixelFieldRuntime::StepPool
    {
        explicit StepPool(std::uint32_t n)
        {
            threads_.reserve(n);
            for (std::uint32_t i = 0; i < n; ++i)
                threads_.emplace_back([this] { workerMain(); });
        }
        ~StepPool()
        {
            {
                std::lock_guard lk(m_);
                quit_ = true;
            }
            cv_work_.notify_all();
            for (std::thread& t : threads_)
                t.join();
        }

        /// Run fn(0..count-1) across the pool; returns when ALL items are done.
        /// (Single caller by contract — the runtime's step path.)
        void run(std::uint32_t count, const std::function<void(std::uint32_t)>& fn)
        {
            {
                std::lock_guard lk(m_);
                job_       = &fn;
                job_count_ = count;
                next_.store(0, std::memory_order_relaxed);
                done_ = 0;
                ++generation_;
            }
            cv_work_.notify_all();
            std::unique_lock lk(m_);
            cv_done_.wait(lk, [&] { return done_ == threads_.size(); });
            job_ = nullptr;
        }

    private:
        void workerMain()
        {
            std::uint64_t seen = 0;
            for (;;)
            {
                std::unique_lock lk(m_);
                cv_work_.wait(lk, [&] { return quit_ || generation_ != seen; });
                if (quit_) return;
                seen = generation_;
                const auto*         fn    = job_;
                const std::uint32_t count = job_count_;
                lk.unlock();

                for (;;)
                {
                    const std::uint32_t i = next_.fetch_add(1, std::memory_order_relaxed);
                    if (i >= count) break;
                    (*fn)(i);
                }

                lk.lock();
                if (++done_ == threads_.size())   // done_ counts THIS generation only
                    cv_done_.notify_one();
            }
        }

        std::vector<std::thread>                  threads_;
        std::mutex                                m_;
        std::condition_variable                   cv_work_, cv_done_;
        const std::function<void(std::uint32_t)>* job_{nullptr};
        std::uint32_t                             job_count_{0};
        std::atomic<std::uint32_t>                next_{0};
        std::size_t                               done_{0};        ///< under m_
        std::uint64_t                             generation_{0};  ///< under m_
        bool                                      quit_{false};    ///< under m_
    };

    PixelFieldRuntime::PixelFieldRuntime()  = default;
    PixelFieldRuntime::~PixelFieldRuntime() = default;

    void PixelFieldRuntime::setWorkerThreads(std::uint32_t n)
    {
        n = std::clamp(n, 1u, 16u);
        if (n == worker_threads_) return;
        worker_threads_ = n;
        pool_.reset();
        if (n > 1)
            pool_ = std::make_unique<StepPool>(n);
    }

    std::uint32_t PixelFieldRuntime::workerThreads() const noexcept
    {
        return worker_threads_;
    }

    // ── lifecycle ────────────────────────────────────────────────────────────

    PixelFieldHandle PixelFieldRuntime::create(const PixelFieldDesc& desc)
    {
        if (desc.cells_w == 0 || desc.cells_h == 0)
            return {};

        std::uint32_t slot;
        if (!free_.empty()) { slot = free_.back(); free_.pop_back(); }
        else { slot = static_cast<std::uint32_t>(fields_.size()); fields_.emplace_back(); }

        Field& f = fields_[slot];
        f.alive  = true;
        f.desc   = desc;

        // C2-00: dense chunk grid, all resident. Uniform full-size blocks (edge
        // clipping is logical); each chunk's planner covers ITS clipped extent.
        f.chunks_x = tileCount(desc.cells_w, kChunkSizeCells);
        f.chunks_y = tileCount(desc.cells_h, kChunkSizeCells);
        f.chunks.assign(static_cast<std::size_t>(f.chunks_x) * f.chunks_y, Chunk{});
        constexpr std::size_t kChunkCellCount =
            static_cast<std::size_t>(kChunkSizeCells) * kChunkSizeCells;
        for (std::uint32_t cy = 0; cy < f.chunks_y; ++cy)
            for (std::uint32_t cx = 0; cx < f.chunks_x; ++cx)
            {
                Chunk& c = f.chunks[static_cast<std::size_t>(cy) * f.chunks_x + cx];
                c.cells.assign(kChunkCellCount, kEmptyMaterial);
                c.moved.assign(kChunkCellCount, 0);
                const std::uint32_t cw =
                    std::min(kChunkSizeCells, desc.cells_w - cx * kChunkSizeCells);
                const std::uint32_t ch =
                    std::min(kChunkSizeCells, desc.cells_h - cy * kChunkSizeCells);
                c.planner = lux::render::RegionUploadPlanner{cw, ch};
                // Optional channels: a disabled channel allocates NOTHING (F2-03).
                if (desc.channels_mask & channelBit(ECellChannel::Temperature))
                    c.temperature.assign(kChunkCellCount, 0.f);
                if (desc.channels_mask & channelBit(ECellChannel::Lifetime))
                    c.lifetime.assign(kChunkCellCount, 0.f);
            }

        f.tiles_x = tileCount(desc.cells_w, kTileSize);
        f.tiles_y = tileCount(desc.cells_h, kTileSize);
        const std::size_t t = static_cast<std::size_t>(f.tiles_x) * f.tiles_y;
        f.active.assign(t, 0);
        f.active_next.assign(t, 0);
        f.changed.assign(t, 0);
        f.rmin_x.assign(t, 0);  f.rmin_y.assign(t, 0);
        f.rmax_x.assign(t, 0);  f.rmax_y.assign(t, 0);
        f.nrmin_x.assign(t, 0); f.nrmin_y.assign(t, 0);
        f.nrmax_x.assign(t, 0); f.nrmax_y.assign(t, 0);
        f.tile_blocking.assign(t, 0);   // I2-00
        f.moved_cells_last = 0;
        f.step_ms_last     = 0.0;
        f.steps            = 0;

        return PixelFieldHandle{slot, f.gen};
    }

    void PixelFieldRuntime::destroy(PixelFieldHandle h)
    {
        if (resolve(h))
            destroySlot(h.index);
    }

    void PixelFieldRuntime::pushEvent(const PixelFieldEvent& e)
    {
        if (events_.size() >= kMaxPendingEvents)
        {
            const std::size_t drop = kMaxPendingEvents / 2;
            events_.erase(events_.begin(), events_.begin() + static_cast<std::ptrdiff_t>(drop));
            events_dropped_ += drop;
        }
        events_.push_back(e);
    }

    void PixelFieldRuntime::destroySlot(std::uint32_t slot)
    {
        Field& f = fields_[slot];
        pushEvent(PixelFieldEvent{PixelFieldEvent::EKind::FieldDestroyed,
                                  PixelFieldHandle{slot, f.gen}, 0});
        f.alive = false;
        ++f.gen;                                  // stale every outstanding handle
        // Cell planes can be megabytes — actually release them (swap idiom).
        std::vector<Chunk>().swap(f.chunks);
        f.chunks_x = f.chunks_y = 0;
        std::vector<std::uint8_t>().swap(f.active);
        std::vector<std::uint8_t>().swap(f.active_next);
        std::vector<std::uint8_t>().swap(f.changed);
        std::vector<std::uint8_t>().swap(f.rmin_x);  std::vector<std::uint8_t>().swap(f.rmin_y);
        std::vector<std::uint8_t>().swap(f.rmax_x);  std::vector<std::uint8_t>().swap(f.rmax_y);
        std::vector<std::uint8_t>().swap(f.nrmin_x); std::vector<std::uint8_t>().swap(f.nrmin_y);
        std::vector<std::uint8_t>().swap(f.nrmax_x); std::vector<std::uint8_t>().swap(f.nrmax_y);
        std::vector<std::uint16_t>().swap(f.tile_blocking);
        free_.push_back(slot);
    }

    bool PixelFieldRuntime::isAlive(PixelFieldHandle h) const noexcept { return resolve(h) != nullptr; }

    PixelFieldDesc PixelFieldRuntime::desc(PixelFieldHandle h) const noexcept
    {
        const Field* f = resolve(h);
        return f ? f->desc : PixelFieldDesc{};
    }

    bool PixelFieldRuntime::channelEnabled(PixelFieldHandle h, ECellChannel c) const noexcept
    {
        const Field* f = resolve(h);
        return f && (f->desc.channels_mask & channelBit(c)) != 0;
    }

    PixelFieldRuntime::Field* PixelFieldRuntime::resolve(PixelFieldHandle h) noexcept
    {
        if (h.is_null() || h.index >= fields_.size()) return nullptr;
        Field& f = fields_[h.index];
        return (f.alive && f.gen == h.gen) ? &f : nullptr;
    }
    const PixelFieldRuntime::Field* PixelFieldRuntime::resolve(PixelFieldHandle h) const noexcept
    {
        return const_cast<PixelFieldRuntime*>(this)->resolve(h);
    }

    void PixelFieldRuntime::maintainOwners(lux::meta::EntityRegistry& registry)
    {
        owner_scratch_.assign(fields_.size(), 0);
        for (auto e : registry.view<PixelField2DComponent>())
        {
            const auto h = registry.get<PixelField2DComponent>(e).field;
            if (resolve(h))
                owner_scratch_[h.index] = 1;
        }
        for (std::uint32_t i = 0; i < fields_.size(); ++i)
            if (fields_[i].alive && !owner_scratch_[i])
                destroySlot(i);

        // ── C2-03: cache each field's frame snapshot (min corner from the
        // owner's WorldTransform2D — the F2-02 contract) for query routing.
        for (Field& f : fields_)
            f.frame_valid = false;
        registry.view<PixelField2DComponent, WorldTransform2DComponent>().each(
            [&](const PixelField2DComponent& pc, const WorldTransform2DComponent& wt)
            {
                if (Field* f = resolve(pc.field))
                {
                    f->frame.origin    = {wt.world(0, 3), wt.world(1, 3)};
                    f->frame.cell_size = pc.cell_size;
                    f->frame_priority  = pc.priority;
                    f->frame_valid     = true;
                }
            });

        // R-08: the MVP FORBIDS overlapping fields — detect and warn (once, so
        // a persistent misconfiguration doesn't spam), count for diagnostics.
        overlap_pairs_ = 0;
        for (std::size_t i = 0; i < fields_.size(); ++i)
        {
            const Field& a = fields_[i];
            if (!a.alive || !a.frame_valid) continue;
            const Eigen::Vector2f a_max = a.frame.origin +
                Eigen::Vector2f(static_cast<float>(a.desc.cells_w),
                                static_cast<float>(a.desc.cells_h)) * a.frame.cell_size;
            for (std::size_t j = i + 1; j < fields_.size(); ++j)
            {
                const Field& b = fields_[j];
                if (!b.alive || !b.frame_valid) continue;
                const Eigen::Vector2f b_max = b.frame.origin +
                    Eigen::Vector2f(static_cast<float>(b.desc.cells_w),
                                    static_cast<float>(b.desc.cells_h)) * b.frame.cell_size;
                const bool overlap =
                    a.frame.origin.x() < b_max.x() && a_max.x() > b.frame.origin.x() &&
                    a.frame.origin.y() < b_max.y() && a_max.y() > b.frame.origin.y();
                if (overlap)
                {
                    ++overlap_pairs_;
                    if (!overlap_warned_)
                    {
                        overlap_warned_ = true;
                        std::fprintf(stderr,
                            "[PixelFieldRuntime] R-08 violation: overlapping pixel fields "
                            "(slots %zu and %zu). The MVP forbids spatial overlap — query "
                            "routing returns both, but interaction semantics are undefined.\n",
                            i, j);
                    }
                }
            }
        }
    }

    // ── C2-02a/b: shared save codec helpers ──────────────────────────────────

    namespace
    {
        constexpr std::uint32_t kSaveMagic      = 0x5653504Cu;   // 'LPSV' (LE)
        constexpr std::uint32_t kSaveEndian     = 0x01020304u;
        constexpr std::uint32_t kSaveSchema     = 1u;
        constexpr std::uint32_t kChunkBlobMagic = 0x4843504Cu;   // 'LPCH' (LE)

        void putU32(std::vector<std::byte>& out, std::uint32_t v)
        {
            const auto* p = reinterpret_cast<const std::byte*>(&v);
            out.insert(out.end(), p, p + 4);
        }
        void putU64(std::vector<std::byte>& out, std::uint64_t v)
        {
            const auto* p = reinterpret_cast<const std::byte*>(&v);
            out.insert(out.end(), p, p + 8);
        }
        struct SaveReader
        {
            std::span<const std::byte> in;
            std::size_t pos{0};
            bool ok{true};
            bool bytes(void* dst, std::size_t n)
            {
                if (!ok || pos + n > in.size()) { ok = false; return false; }
                std::memcpy(dst, in.data() + pos, n);
                pos += n;
                return true;
            }
            std::uint32_t u32() { std::uint32_t v{}; bytes(&v, 4); return v; }
            std::uint64_t u64() { std::uint64_t v{}; bytes(&v, 8); return v; }
        };
    }

    // ── C2-02b: per-chunk CPU residency ──────────────────────────────────────

    std::vector<std::byte> PixelFieldRuntime::captureChunk(PixelFieldHandle h,
                                                           std::uint32_t cx, std::uint32_t cy) const
    {
        std::vector<std::byte> out;
        const Field* f = resolve(h);
        if (!f || cx >= f->chunks_x || cy >= f->chunks_y) return out;
        const Chunk& c = f->chunks[static_cast<std::size_t>(cy) * f->chunks_x + cx];
        if (!c.resident) return out;

        const std::uint32_t cw = std::min(kChunkSizeCells, f->desc.cells_w - cx * kChunkSizeCells);
        const std::uint32_t ch = std::min(kChunkSizeCells, f->desc.cells_h - cy * kChunkSizeCells);
        const bool has_temp = (f->desc.channels_mask & channelBit(ECellChannel::Temperature)) != 0;
        const bool has_life = (f->desc.channels_mask & channelBit(ECellChannel::Lifetime)) != 0;

        // Self-describing standalone record: header + the SAME checksummed
        // payload captureState writes for this chunk.
        putU32(out, kChunkBlobMagic);
        putU32(out, kSaveSchema);
        putU32(out, cw);
        putU32(out, ch);
        putU32(out, f->desc.channels_mask);

        std::vector<std::byte> payload;
        const auto putRows = [&](const auto* plane, std::size_t elem)
        {
            for (std::uint32_t row = 0; row < ch; ++row)
            {
                const auto* src = reinterpret_cast<const std::byte*>(plane)
                                + (static_cast<std::size_t>(row) << kChunkShift) * elem;
                payload.insert(payload.end(), src, src + static_cast<std::size_t>(cw) * elem);
            }
        };
        putRows(c.cells.data(), sizeof(MaterialId));
        if (has_temp) putRows(c.temperature.data(), sizeof(float));
        if (has_life) putRows(c.lifetime.data(), sizeof(float));
        putU64(out, fnv1a64Append(kFnvBasis, payload.data(), payload.size()));
        out.insert(out.end(), payload.begin(), payload.end());
        return out;
    }

    bool PixelFieldRuntime::unloadChunk(PixelFieldHandle h, std::uint32_t cx, std::uint32_t cy,
                                        std::vector<std::byte>& out_blob)
    {
        out_blob = captureChunk(h, cx, cy);
        if (out_blob.empty()) return false;
        Field* f = resolve(h);
        Chunk& c = f->chunks[static_cast<std::size_t>(cy) * f->chunks_x + cx];
        // Release the planes (megabytes) and go dark. The planner resets — the
        // GPU mirror of an unloaded chunk is disposable; loading repaints fully.
        std::vector<MaterialId>().swap(c.cells);
        std::vector<std::uint8_t>().swap(c.moved);
        std::vector<float>().swap(c.temperature);
        std::vector<float>().swap(c.lifetime);
        c.planner  = lux::render::RegionUploadPlanner{0, 0};
        c.resident = false;
        ++transfer_epoch_;   // I2-01: residency change mutates observable cells
        return true;
    }

    bool PixelFieldRuntime::loadChunk(PixelFieldHandle h, std::uint32_t cx, std::uint32_t cy,
                                      std::span<const std::byte> blob, std::string* error_out)
    {
        const auto fail = [&](const char* what)
        {
            if (error_out) *error_out = what;
            return false;
        };
        Field* f = resolve(h);
        if (!f || cx >= f->chunks_x || cy >= f->chunks_y) return fail("bad handle / chunk");
        Chunk& c = f->chunks[static_cast<std::size_t>(cy) * f->chunks_x + cx];
        if (c.resident) return fail("chunk already resident");

        const std::uint32_t cw = std::min(kChunkSizeCells, f->desc.cells_w - cx * kChunkSizeCells);
        const std::uint32_t ch = std::min(kChunkSizeCells, f->desc.cells_h - cy * kChunkSizeCells);
        const bool has_temp = (f->desc.channels_mask & channelBit(ECellChannel::Temperature)) != 0;
        const bool has_life = (f->desc.channels_mask & channelBit(ECellChannel::Lifetime)) != 0;

        SaveReader r{blob};
        if (r.u32() != kChunkBlobMagic)          return fail("wrong chunk magic");
        if (r.u32() != kSaveSchema)              return fail("unsupported chunk schema");
        if (r.u32() != cw || r.u32() != ch)      return fail("chunk extent mismatch");
        if (r.u32() != f->desc.channels_mask)    return fail("channel mask mismatch");
        const std::uint64_t want = r.u64();
        if (!r.ok)                               return fail("truncated chunk header");

        std::size_t bytes = static_cast<std::size_t>(cw) * ch * sizeof(MaterialId);
        if (has_temp) bytes += static_cast<std::size_t>(cw) * ch * sizeof(float);
        if (has_life) bytes += static_cast<std::size_t>(cw) * ch * sizeof(float);
        std::vector<std::byte> payload(bytes);
        if (!r.bytes(payload.data(), bytes))     return fail("truncated chunk payload");
        if (r.pos != blob.size())                return fail("trailing bytes");
        if (fnv1a64Append(kFnvBasis, payload.data(), payload.size()) != want)
            return fail("chunk checksum mismatch");

        constexpr std::size_t kChunkCellCount =
            static_cast<std::size_t>(kChunkSizeCells) * kChunkSizeCells;
        c.cells.assign(kChunkCellCount, kEmptyMaterial);
        c.moved.assign(kChunkCellCount, 0);
        if (has_temp) c.temperature.assign(kChunkCellCount, 0.f);
        if (has_life) c.lifetime.assign(kChunkCellCount, 0.f);

        std::size_t off = 0;
        const auto getRows = [&](auto* plane, std::size_t elem)
        {
            for (std::uint32_t row = 0; row < ch; ++row)
            {
                std::memcpy(reinterpret_cast<std::byte*>(plane)
                                + (static_cast<std::size_t>(row) << kChunkShift) * elem,
                            payload.data() + off,
                            static_cast<std::size_t>(cw) * elem);
                off += static_cast<std::size_t>(cw) * elem;
            }
        };
        getRows(c.cells.data(), sizeof(MaterialId));
        if (has_temp) getRows(c.temperature.data(), sizeof(float));
        if (has_life) getRows(c.lifetime.data(), sizeof(float));

        c.planner  = lux::render::RegionUploadPlanner{cw, ch};
        c.planner.markDirty(0, 0, cw, ch);   // the mirror repaints fully
        c.resident = true;
        recountChunkOccupancy(*f, cx, cy);   // I2-00
        // Wake the chunk (+1 border cell so neighbours re-evaluate the seam).
        wakeSpan(*f,
                 static_cast<std::int32_t>(cx * kChunkSizeCells) - 1,
                 static_cast<std::int32_t>(cy * kChunkSizeCells) - 1,
                 static_cast<std::int32_t>(cx * kChunkSizeCells + cw),
                 static_cast<std::int32_t>(cy * kChunkSizeCells + ch),
                 /*next=*/false);
        ++transfer_epoch_;   // I2-01: residency change mutates observable cells
        return true;
    }

    bool PixelFieldRuntime::chunkResident(PixelFieldHandle h,
                                          std::uint32_t cx, std::uint32_t cy) const noexcept
    {
        const Field* f = resolve(h);
        if (!f || cx >= f->chunks_x || cy >= f->chunks_y) return false;
        return f->chunks[static_cast<std::size_t>(cy) * f->chunks_x + cx].resident;
    }

    // ── I2-00: collision occupancy ───────────────────────────────────────────

    void PixelFieldRuntime::recountChunkOccupancy(Field& f, std::uint32_t cx, std::uint32_t cy) const
    {
        const std::uint32_t cw = std::min(kChunkSizeCells, f.desc.cells_w - cx * kChunkSizeCells);
        const std::uint32_t ch = std::min(kChunkSizeCells, f.desc.cells_h - cy * kChunkSizeCells);
        const std::uint32_t gx0 = cx * kChunkSizeCells, gy0 = cy * kChunkSizeCells;
        for (std::uint32_t ty = gy0 / kTileSize; ty <= (gy0 + ch - 1) / kTileSize; ++ty)
            for (std::uint32_t tx = gx0 / kTileSize; tx <= (gx0 + cw - 1) / kTileSize; ++tx)
            {
                std::uint16_t n = 0;
                const std::uint32_t x0 = tx * kTileSize, y0 = ty * kTileSize;
                const std::uint32_t x1 = std::min(x0 + kTileSize, f.desc.cells_w);
                const std::uint32_t y1 = std::min(y0 + kTileSize, f.desc.cells_h);
                for (std::uint32_t y = y0; y < y1; ++y)
                    for (std::uint32_t x = x0; x < x1; ++x)
                        n += isBlocking(materials_, f.cellAt(x, y)) ? 1u : 0u;
                f.tile_blocking[static_cast<std::size_t>(ty) * f.tiles_x + tx] = n;
            }
    }

    bool PixelFieldRuntime::regionBlocked(PixelFieldHandle h,
                                          Eigen::Vector2i cell_min,
                                          Eigen::Vector2i cell_max) const noexcept
    {
        const Field* f = resolve(h);
        if (!f) return false;
        f->cells_probed_last = 0;
        const std::int32_t x0 = std::max(cell_min.x(), 0);
        const std::int32_t y0 = std::max(cell_min.y(), 0);
        const std::int32_t x1 = std::min(cell_max.x(), static_cast<std::int32_t>(f->desc.cells_w) - 1);
        const std::int32_t y1 = std::min(cell_max.y(), static_cast<std::int32_t>(f->desc.cells_h) - 1);
        if (x0 > x1 || y0 > y1) return false;

        const std::uint32_t T = kTileSize;
        for (std::uint32_t ty = static_cast<std::uint32_t>(y0) / T;
             ty <= static_cast<std::uint32_t>(y1) / T; ++ty)
            for (std::uint32_t tx = static_cast<std::uint32_t>(x0) / T;
                 tx <= static_cast<std::uint32_t>(x1) / T; ++tx)
            {
                // Unloaded chunks block entirely (the CA's solid-wall contract).
                if (!f->chunkAt(tx * T, ty * T).resident)
                    return true;
                const std::uint16_t cnt =
                    f->tile_blocking[static_cast<std::size_t>(ty) * f->tiles_x + tx];
                if (cnt == 0) continue;                       // empty tile: skip

                const std::uint32_t bx0 = std::max<std::uint32_t>(x0, tx * T);
                const std::uint32_t by0 = std::max<std::uint32_t>(y0, ty * T);
                const std::uint32_t bx1 = std::min<std::uint32_t>(x1, tx * T + T - 1);
                const std::uint32_t by1 = std::min<std::uint32_t>(y1, ty * T + T - 1);
                const bool full_tile =
                    bx0 == tx * T && by0 == ty * T &&
                    bx1 == std::min(tx * T + T - 1, f->desc.cells_w - 1) &&
                    by1 == std::min(ty * T + T - 1, f->desc.cells_h - 1);
                if (full_tile)
                    return true;                              // cnt>0 and fully covered
                // Partial overlap: scan just the intersection.
                for (std::uint32_t y = by0; y <= by1; ++y)
                    for (std::uint32_t x = bx0; x <= bx1; ++x)
                    {
                        ++f->cells_probed_last;
                        if (isBlocking(materials_, f->cellAt(x, y)))
                            return true;
                    }
            }
        return false;
    }

    std::uint32_t PixelFieldRuntime::cellsProbedLast(PixelFieldHandle h) const noexcept
    {
        const Field* f = resolve(h);
        return f ? f->cells_probed_last : 0;
    }

    // ── I2-01: transactional Field → Entity extraction ───────────────────────
    //
    // The epoch discipline: transfer_epoch_ increments on EVERY field mutation
    // (step, command batch, chunk load/unload, a committed extract). A ticket
    // stamps the epoch at prepare; extractData/commitExtract demand equality.
    // Consequence: prepare→commit must happen with no mutation in between
    // (in practice: inside one fixed-step phase), and the FIRST commit in a
    // window invalidates every other outstanding ticket — their snapshots
    // could overlap the cells it just cleared. Cheaper and stricter than a
    // phase-window flag, and it can never clear drifted cells.

    PixelExtractTicket PixelFieldRuntime::prepareExtract(PixelFieldHandle h,
                                                         Eigen::Vector2i min,
                                                         Eigen::Vector2i size,
                                                         std::uint32_t max_cells)
    {
        PixelExtractTicket t{};
        const Field* f = resolve(h);
        if (!f || size.x() <= 0 || size.y() <= 0)
            return t;
        if (static_cast<std::uint64_t>(size.x()) * static_cast<std::uint64_t>(size.y()) > max_cells)
            return t;   // refuse oversized rects up front (the caller's budget)

        std::uint32_t idx;
        if (!extract_free_.empty()) { idx = extract_free_.back(); extract_free_.pop_back(); }
        else { idx = static_cast<std::uint32_t>(extracts_.size()); extracts_.emplace_back(); }

        ExtractSlot& s = extracts_[idx];
        s.alive = true;
        ++s.gen;
        s.field = h;
        s.epoch = transfer_epoch_;
        s.data.min  = min;
        s.data.size = size;
        s.data.cells.assign(static_cast<std::size_t>(size.x()) * size.y(), kEmptyMaterial);
        s.data.nonempty = 0;

        // Snapshot only (NEVER mutates): out-of-bounds / unloaded cells read
        // empty — the same lie every read path tells (C2-02b).
        const std::int32_t w = static_cast<std::int32_t>(f->desc.cells_w);
        const std::int32_t hgt = static_cast<std::int32_t>(f->desc.cells_h);
        for (std::int32_t y = min.y(); y < min.y() + size.y(); ++y)
        {
            if (y < 0 || y >= hgt) continue;
            for (std::int32_t x = min.x(); x < min.x() + size.x(); ++x)
            {
                if (x < 0 || x >= w) continue;
                const std::uint32_t ux = static_cast<std::uint32_t>(x);
                const std::uint32_t uy = static_cast<std::uint32_t>(y);
                if (!f->chunkAt(ux, uy).resident) continue;
                const MaterialId id = f->cellAt(ux, uy);
                if (id == kEmptyMaterial) continue;
                s.data.cells[static_cast<std::size_t>(y - min.y()) * size.x()
                             + static_cast<std::size_t>(x - min.x())] = id;
                ++s.data.nonempty;
            }
        }

        t.index = idx;
        t.gen   = s.gen;
        return t;
    }

    const PixelExtractData* PixelFieldRuntime::extractData(PixelExtractTicket t) const noexcept
    {
        if (t.index >= extracts_.size()) return nullptr;
        const ExtractSlot& s = extracts_[t.index];
        if (!s.alive || s.gen != t.gen || s.epoch != transfer_epoch_)
            return nullptr;   // stale = the field moved on; the snapshot is a lie
        return &s.data;
    }

    bool PixelFieldRuntime::commitExtract(PixelExtractTicket t)
    {
        if (t.index >= extracts_.size()) return false;
        ExtractSlot& s = extracts_[t.index];
        if (!s.alive || s.gen != t.gen) return false;

        const auto release = [&]
        {
            s.alive = false;
            s.data.cells.clear();   // keep capacity for slot reuse
            extract_free_.push_back(t.index);
        };

        Field* f = resolve(s.field);
        if (!f || s.epoch != transfer_epoch_)
        {
            release();     // stale ticket dies, the FIELD IS NOT TOUCHED
            return false;
        }

        // Epoch equality ⇒ the field is bit-identical to the snapshot inside
        // the rect — clearing exactly the snapshot's non-empty cells conserves
        // matter by construction (cleared == nonempty, asserted below).
        std::uint32_t cleared = 0;
        const Eigen::Vector2i min = s.data.min, size = s.data.size;
        for (std::int32_t y = min.y(); y < min.y() + size.y(); ++y)
            for (std::int32_t x = min.x(); x < min.x() + size.x(); ++x)
            {
                const MaterialId id =
                    s.data.cells[static_cast<std::size_t>(y - min.y()) * size.x()
                                 + static_cast<std::size_t>(x - min.x())];
                if (id == kEmptyMaterial) continue;
                const std::uint32_t ux = static_cast<std::uint32_t>(x);
                const std::uint32_t uy = static_cast<std::uint32_t>(y);
                assert(f->chunkAt(ux, uy).resident && f->cellAt(ux, uy) == id &&
                       "epoch guard broken: field drifted under a live ticket");
                f->cellAt(ux, uy) = kEmptyMaterial;
                if (isBlocking(materials_, id))   // I2-00 occupancy stays exact
                    --f->tile_blocking[static_cast<std::size_t>(uy / kTileSize) * f->tiles_x
                                       + ux / kTileSize];
                ++cleared;
            }
        assert(cleared == s.data.nonempty);

        if (cleared)
        {
            // Same dirt/wake shape as a StampRect erase: exact-rect upload dirt
            // + the dispersion-widened sim wake band.
            const std::int32_t x0 = std::max(min.x(), 0);
            const std::int32_t y0 = std::max(min.y(), 0);
            const std::int32_t x1 = std::min(min.x() + size.x(),
                                             static_cast<std::int32_t>(f->desc.cells_w));
            const std::int32_t y1 = std::min(min.y() + size.y(),
                                             static_cast<std::int32_t>(f->desc.cells_h));
            markRectDirty(*f, static_cast<std::uint32_t>(x0), static_cast<std::uint32_t>(y0),
                          static_cast<std::uint32_t>(x1 - x0), static_cast<std::uint32_t>(y1 - y0));
            wakeSpan(*f, x0 - kLiquidDispersion, y0 - 1,
                         x1 - 1 + kLiquidDispersion, y1, /*next=*/false);
        }

        ++transfer_epoch_;   // a commit IS a mutation — sibling tickets die
        release();
        return true;
    }

    void PixelFieldRuntime::rollbackExtract(PixelExtractTicket t) noexcept
    {
        if (t.index >= extracts_.size()) return;
        ExtractSlot& s = extracts_[t.index];
        if (!s.alive || s.gen != t.gen) return;
        s.alive = false;
        s.data.cells.clear();
        extract_free_.push_back(t.index);
    }

    // ── C2-02a: save-state blob codec ────────────────────────────────────────
    //
    // Layout (little-endian, no implicit padding):
    //   u32 magic='LPSV'  u32 endian=0x01020304  u32 schema=1
    //   u32 cells_w  u32 cells_h  u32 channels_mask  u32 chunk_size
    //   per chunk, cy-major then cx:
    //     u64 fnv1a64(all following payload bytes of THIS chunk)
    //     u16[clip_w*clip_h]   material plane (clipped rows, row 0 = chunk bottom)
    //     f32[clip_w*clip_h]   temperature    (iff channel enabled)
    //     f32[clip_w*clip_h]   lifetime       (iff channel enabled)
    // Version / extent / checksum mismatch = explicit failure (no migration).

    std::vector<std::byte> PixelFieldRuntime::captureState(PixelFieldHandle h) const
    {
        const Field* f = resolve(h);
        std::vector<std::byte> out;
        if (!f) return out;

        for (const Chunk& pc : f->chunks)
            if (!pc.resident) return {};   // C2-02b: whole-field save needs full residency
        putU32(out, kSaveMagic);
        putU32(out, kSaveEndian);
        putU32(out, kSaveSchema);
        putU32(out, f->desc.cells_w);
        putU32(out, f->desc.cells_h);
        putU32(out, f->desc.channels_mask);
        putU32(out, kChunkSizeCells);

        const bool has_temp = (f->desc.channels_mask & channelBit(ECellChannel::Temperature)) != 0;
        const bool has_life = (f->desc.channels_mask & channelBit(ECellChannel::Lifetime)) != 0;

        std::vector<std::byte> payload;   // per-chunk scratch (checksummed as a unit)
        for (std::uint32_t cy = 0; cy < f->chunks_y; ++cy)
            for (std::uint32_t cx = 0; cx < f->chunks_x; ++cx)
            {
                const Chunk& c = f->chunks[static_cast<std::size_t>(cy) * f->chunks_x + cx];
                const std::uint32_t cw =
                    std::min(kChunkSizeCells, f->desc.cells_w - cx * kChunkSizeCells);
                const std::uint32_t ch =
                    std::min(kChunkSizeCells, f->desc.cells_h - cy * kChunkSizeCells);

                payload.clear();
                const auto putRows = [&](const auto* plane, std::size_t elem)
                {
                    for (std::uint32_t row = 0; row < ch; ++row)
                    {
                        const auto* src = reinterpret_cast<const std::byte*>(plane)
                                        + (static_cast<std::size_t>(row) << kChunkShift) * elem;
                        payload.insert(payload.end(), src, src + static_cast<std::size_t>(cw) * elem);
                    }
                };
                putRows(c.cells.data(), sizeof(MaterialId));
                if (has_temp) putRows(c.temperature.data(), sizeof(float));
                if (has_life) putRows(c.lifetime.data(), sizeof(float));

                putU64(out, fnv1a64Append(kFnvBasis, payload.data(), payload.size()));
                out.insert(out.end(), payload.begin(), payload.end());
            }
        return out;
    }

    PixelFieldHandle PixelFieldRuntime::restoreState(std::span<const std::byte> blob,
                                                     std::string* error_out)
    {
        const auto fail = [&](const char* what) -> PixelFieldHandle
        {
            if (error_out) *error_out = what;
            return {};
        };

        SaveReader r{blob};
        if (r.u32() != kSaveMagic)  return fail("wrong save magic");
        if (r.u32() != kSaveEndian) return fail("wrong endianness");
        if (r.u32() != kSaveSchema) return fail("unsupported save schema (no v1 migration)");
        PixelFieldDesc desc{};
        desc.cells_w       = r.u32();
        desc.cells_h       = r.u32();
        desc.channels_mask = r.u32();
        const std::uint32_t chunk_size = r.u32();
        if (!r.ok)                          return fail("truncated header");
        if (chunk_size != kChunkSizeCells)  return fail("chunk size mismatch (schema-incompatible build)");
        if (desc.cells_w == 0 || desc.cells_h == 0 ||
            desc.cells_w > (1u << 20) || desc.cells_h > (1u << 20))
            return fail("implausible field extent");

        const PixelFieldHandle h = create(desc);
        Field* f = resolve(h);
        if (!f) return fail("field creation failed");

        const bool has_temp = (desc.channels_mask & channelBit(ECellChannel::Temperature)) != 0;
        const bool has_life = (desc.channels_mask & channelBit(ECellChannel::Lifetime)) != 0;

        std::vector<std::byte> payload;
        for (std::uint32_t cy = 0; cy < f->chunks_y; ++cy)
            for (std::uint32_t cx = 0; cx < f->chunks_x; ++cx)
            {
                Chunk& c = f->chunks[static_cast<std::size_t>(cy) * f->chunks_x + cx];
                const std::uint32_t cw = std::min(kChunkSizeCells, desc.cells_w - cx * kChunkSizeCells);
                const std::uint32_t ch = std::min(kChunkSizeCells, desc.cells_h - cy * kChunkSizeCells);
                std::size_t bytes = static_cast<std::size_t>(cw) * ch * sizeof(MaterialId);
                if (has_temp) bytes += static_cast<std::size_t>(cw) * ch * sizeof(float);
                if (has_life) bytes += static_cast<std::size_t>(cw) * ch * sizeof(float);

                const std::uint64_t want = r.u64();
                payload.resize(bytes);
                if (!r.bytes(payload.data(), bytes))
                { destroy(h); return fail("truncated chunk payload"); }
                if (fnv1a64Append(kFnvBasis, payload.data(), payload.size()) != want)
                { destroy(h); return fail("chunk checksum mismatch (corrupt save)"); }

                std::size_t off = 0;
                const auto getRows = [&](auto* plane, std::size_t elem)
                {
                    for (std::uint32_t row = 0; row < ch; ++row)
                    {
                        std::memcpy(reinterpret_cast<std::byte*>(plane)
                                        + (static_cast<std::size_t>(row) << kChunkShift) * elem,
                                    payload.data() + off,
                                    static_cast<std::size_t>(cw) * elem);
                        off += static_cast<std::size_t>(cw) * elem;
                    }
                };
                getRows(c.cells.data(), sizeof(MaterialId));
                if (has_temp) getRows(c.temperature.data(), sizeof(float));
                if (has_life) getRows(c.lifetime.data(), sizeof(float));

                // The restored content must reach the GPU mirror + the CA:
                // full-chunk upload dirt + a full wake (settles in a few steps).
                c.planner.markDirty(0, 0, cw, ch);
                recountChunkOccupancy(*f, cx, cy);   // I2-00
            }
        if (r.pos != blob.size()) { destroy(h); return fail("trailing bytes after the last chunk"); }
        wakeSpan(*f, 0, 0, static_cast<std::int32_t>(desc.cells_w) - 1,
                 static_cast<std::int32_t>(desc.cells_h) - 1, /*next=*/false);
        return h;
    }

    // ── C2-03: world-space query routing ─────────────────────────────────────

    void PixelFieldRuntime::queryFields(const Eigen::Vector2f& min, const Eigen::Vector2f& max,
                                        std::vector<PixelFieldQueryEntry>& out) const
    {
        out.clear();
        for (std::size_t i = 0; i < fields_.size(); ++i)
        {
            const Field& f = fields_[i];
            if (!f.alive || !f.frame_valid) continue;
            const Eigen::Vector2f f_max = f.frame.origin +
                Eigen::Vector2f(static_cast<float>(f.desc.cells_w),
                                static_cast<float>(f.desc.cells_h)) * f.frame.cell_size;
            if (f.frame.origin.x() < max.x() && f_max.x() > min.x() &&
                f.frame.origin.y() < max.y() && f_max.y() > min.y())
            {
                out.push_back({PixelFieldHandle{static_cast<std::uint32_t>(i), f.gen},
                               f.frame, f.frame_priority});
            }
        }
        // Priority DESCENDING, ties by slot (already ascending from the scan) —
        // stable_sort keeps the deterministic tie order.
        std::stable_sort(out.begin(), out.end(),
                         [](const PixelFieldQueryEntry& a, const PixelFieldQueryEntry& b)
                         { return a.priority > b.priority; });
    }

    bool PixelFieldRuntime::fieldFrame(PixelFieldHandle h, PixelFieldFrame& out) const noexcept
    {
        const Field* f = resolve(h);
        if (!f || !f->frame_valid) return false;
        out = f->frame;
        return true;
    }

    // ── commands (F2-06: ALL writes land here, FIFO, never mid-scan) ─────────

    void PixelFieldRuntime::markRectDirty(Field& f, std::uint32_t x0, std::uint32_t y0,
                                          std::uint32_t w, std::uint32_t h) noexcept
    {
        // Clamp to the field, then split across the owning chunks' planners in
        // CHUNK-LOCAL coordinates.
        if (x0 >= f.desc.cells_w || y0 >= f.desc.cells_h || w == 0 || h == 0) return;
        const std::uint32_t x1 = std::min(x0 + w, f.desc.cells_w);    // exclusive
        const std::uint32_t y1 = std::min(y0 + h, f.desc.cells_h);
        for (std::uint32_t cy = y0 >> kChunkShift; cy <= (y1 - 1) >> kChunkShift; ++cy)
            for (std::uint32_t cx = x0 >> kChunkShift; cx <= (x1 - 1) >> kChunkShift; ++cx)
            {
                Chunk& c = f.chunks[static_cast<std::size_t>(cy) * f.chunks_x + cx];
                if (!c.resident) continue;   // C2-02b: no mirror to dirty
                const std::uint32_t bx0 = std::max(x0, cx << kChunkShift);
                const std::uint32_t by0 = std::max(y0, cy << kChunkShift);
                const std::uint32_t bx1 = std::min(x1, (cx + 1u) << kChunkShift);
                const std::uint32_t by1 = std::min(y1, (cy + 1u) << kChunkShift);
                c.planner.markDirty(bx0 & kChunkMask, by0 & kChunkMask,
                                    bx1 - bx0, by1 - by0);
            }
    }

    void PixelFieldRuntime::applyCommands()
    {
        if (!commands_.empty())
            ++transfer_epoch_;   // I2-01: a command batch is a field mutation

        for (const PixelFieldCommand& cmd : commands_)
        {
            Field* f = resolve(cmd.field);
            if (!f) continue;   // stale handle → inert

            const bool stamp_cells = cmd.kind == PixelFieldCommand::EKind::StampCells;
            if (stamp_cells &&
                (!cmd.cells || cmd.size.x() <= 0 || cmd.size.y() <= 0 ||
                 cmd.cells->size() != static_cast<std::size_t>(cmd.size.x()) *
                                      static_cast<std::size_t>(cmd.size.y())))
            {
                // Malformed payload → inert but still evented (the producer's
                // ack loop must not hang on a bug).
                pushEvent(PixelFieldEvent{PixelFieldEvent::EKind::CommandsApplied,
                                          cmd.field, 0});
                continue;
            }

            const std::int32_t w = static_cast<std::int32_t>(f->desc.cells_w);
            const std::int32_t h = static_cast<std::int32_t>(f->desc.cells_h);
            const std::int32_t x0 = std::max(cmd.min.x(), 0);
            const std::int32_t y0 = std::max(cmd.min.y(), 0);
            const std::int32_t x1 = std::min(cmd.min.x() + cmd.size.x(), w);
            const std::int32_t y1 = std::min(cmd.min.y() + cmd.size.y(), h);
            if (x0 >= x1 || y0 >= y1) continue;

            std::uint32_t changed = 0;
            for (std::int32_t y = y0; y < y1; ++y)
                for (std::int32_t x = x0; x < x1; ++x)
                {
                    if (!f->chunkAt(static_cast<std::uint32_t>(x),
                                    static_cast<std::uint32_t>(y)).resident)
                        continue;   // C2-02b: command space = resident space
                    auto& cell = f->cellAt(static_cast<std::uint32_t>(x),
                                           static_cast<std::uint32_t>(y));
                    MaterialId want = cmd.material;
                    if (stamp_cells)
                    {
                        // I2-01 Entity→Field: only non-empty payload cells land,
                        // and NEVER on occupied field cells — matter is neither
                        // created from nothing nor destroyed; the event count
                        // below tells the producer how much actually landed.
                        want = (*cmd.cells)[static_cast<std::size_t>(y - cmd.min.y()) * cmd.size.x()
                                            + static_cast<std::size_t>(x - cmd.min.x())];
                        if (want == kEmptyMaterial || cell != kEmptyMaterial)
                            continue;
                    }
                    if (cell != want)
                    {
                        // I2-00: incremental occupancy.
                        const std::size_t tb = static_cast<std::size_t>(y / static_cast<std::int32_t>(kTileSize)) * f->tiles_x
                                             + static_cast<std::size_t>(x / static_cast<std::int32_t>(kTileSize));
                        const bool was = isBlocking(materials_, cell);
                        const bool now = isBlocking(materials_, want);
                        if (was && !now) --f->tile_blocking[tb];
                        else if (!was && now) ++f->tile_blocking[tb];
                        cell = want;
                        ++changed;
                    }
                }
            if (changed)
            {
                // Exact-rect upload dirt (per-chunk planners coalesce); sim wake
                // = the stamp ±1, widened by the liquid dispersion reach
                // horizontally (an ERASE opens holes distant surface water must
                // notice; a conservative constant band, cheap either way).
                markRectDirty(*f, static_cast<std::uint32_t>(x0), static_cast<std::uint32_t>(y0),
                              static_cast<std::uint32_t>(x1 - x0), static_cast<std::uint32_t>(y1 - y0));
                wakeSpan(*f, x0 - kLiquidDispersion, y0 - 1,
                             x1 - 1 + kLiquidDispersion, y1, /*next=*/false);
            }
            pushEvent(PixelFieldEvent{PixelFieldEvent::EKind::CommandsApplied,
                                      cmd.field, changed});
        }
        commands_.clear();
    }

    // ── the deterministic CA step (F2-04/F2-05) ──────────────────────────────

    void PixelFieldRuntime::wakeSpan(Field& f, std::int32_t x0, std::int32_t y0,
                                     std::int32_t x1, std::int32_t y1, bool next)
    {
        x0 = std::max(x0, 0);
        y0 = std::max(y0, 0);
        x1 = std::min(x1, static_cast<std::int32_t>(f.desc.cells_w) - 1);
        y1 = std::min(y1, static_cast<std::int32_t>(f.desc.cells_h) - 1);
        if (x0 > x1 || y0 > y1) return;

        auto& mask = next ? f.active_next : f.active;
        auto& mnx = next ? f.nrmin_x : f.rmin_x;   auto& mny = next ? f.nrmin_y : f.rmin_y;
        auto& mxx = next ? f.nrmax_x : f.rmax_x;   auto& mxy = next ? f.nrmax_y : f.rmax_y;

        const std::uint32_t T = kTileSize;
        for (std::uint32_t ty = static_cast<std::uint32_t>(y0) / T;
             ty <= static_cast<std::uint32_t>(y1) / T; ++ty)
            for (std::uint32_t tx = static_cast<std::uint32_t>(x0) / T;
                 tx <= static_cast<std::uint32_t>(x1) / T; ++tx)
            {
                const std::size_t t = static_cast<std::size_t>(ty) * f.tiles_x + tx;
                // The span's slice inside THIS tile, in within-tile coords.
                const std::uint8_t lx0 = static_cast<std::uint8_t>(std::max<std::int32_t>(x0, tx * T) - tx * T);
                const std::uint8_t ly0 = static_cast<std::uint8_t>(std::max<std::int32_t>(y0, ty * T) - ty * T);
                const std::uint8_t lx1 = static_cast<std::uint8_t>(std::min<std::int32_t>(x1, tx * T + T - 1) - tx * T);
                const std::uint8_t ly1 = static_cast<std::uint8_t>(std::min<std::int32_t>(y1, ty * T + T - 1) - ty * T);
                if (!mask[t])
                {
                    mask[t] = 1;
                    mnx[t] = lx0; mny[t] = ly0;
                    mxx[t] = lx1; mxy[t] = ly1;
                }
                else
                {
                    mnx[t] = std::min(mnx[t], lx0); mny[t] = std::min(mny[t], ly0);
                    mxx[t] = std::max(mxx[t], lx1); mxy[t] = std::max(mxy[t], ly1);
                }
            }
    }

    bool PixelFieldRuntime::tryMove(Field& f, std::uint32_t x, std::uint32_t y,
                                    std::int32_t nx, std::int32_t ny, bool displace_liquid,
                                    std::uint32_t& moved_counter)
    {
        if (nx < 0 || ny < 0 ||
            nx >= static_cast<std::int32_t>(f.desc.cells_w) ||
            ny >= static_cast<std::int32_t>(f.desc.cells_h))
            return false;

        const std::uint32_t ux = static_cast<std::uint32_t>(nx);
        const std::uint32_t uy = static_cast<std::uint32_t>(ny);
        if (!f.chunkAt(ux, uy).resident)
            return false;   // C2-02b: an unloaded chunk is a SOLID WALL
        MaterialId& src_cell = f.cellAt(x, y);
        MaterialId& dst_cell = f.cellAt(ux, uy);
        const MaterialId src = src_cell;
        const MaterialId dst = dst_cell;

        if (dst == kEmptyMaterial)
        {
            dst_cell = src;
            src_cell = kEmptyMaterial;
        }
        else if (displace_liquid &&
                 materials_.at(dst).phase == EMaterialPhase::Liquid &&
                 materials_.at(src).density > materials_.at(dst).density)
        {
            dst_cell = src;                       // heavier sinks…
            src_cell = dst;                       // …lighter liquid swaps up
            f.movedAt(x, y) = 1;                  // the displaced liquid moved too
        }
        else
            return false;

        f.movedAt(ux, uy) = 1;
        ++moved_counter;

        const std::uint32_t stx = x / kTileSize, sty = y / kTileSize;
        const std::uint32_t dtx = ux / kTileSize;
        const std::uint32_t dty = uy / kTileSize;
        f.changed[static_cast<std::size_t>(sty) * f.tiles_x + stx] = 1;
        f.changed[static_cast<std::size_t>(dty) * f.tiles_x + dtx] = 1;
        // I2-00: the moving material carries its blocking count across tiles
        // (the empty-dst AND the liquid-swap case both leave the source cell
        // non-blocking and the destination blocking iff `src` blocks).
        if ((stx != dtx || sty != dty) && isBlocking(materials_, src))
        {
            --f.tile_blocking[static_cast<std::size_t>(sty) * f.tiles_x + stx];
            ++f.tile_blocking[static_cast<std::size_t>(dty) * f.tiles_x + dtx];
        }

        // Wake the ±1 band around both cells for the NEXT step (anything whose
        // support/neighbourhood this move touched sits inside it)…
        const std::int32_t xi = static_cast<std::int32_t>(x), yi = static_cast<std::int32_t>(y);
        // ±1 wake expansion. NOTE (mutation analysis, 2026-07-06): for moves that
        // EMPTY their source this overlaps the dispersion band below (which spans
        // y..y+1) — the ±1 is then belt-and-braces. Its EXCLUSIVE job is the SWAP
        // move (powder displacing liquid: source stays occupied, no band fires),
        // where it is the only thing waking the displaced neighbourhood.
        wakeSpan(f, std::min(xi, nx) - 1, std::min(yi, ny) - 1,
                    std::max(xi, nx) + 1, std::max(yi, ny) + 1, /*next=*/true);
        // …and when the SOURCE became a hole, the liquid dispersion rule can see
        // it from up to kLiquidDispersion cells away along the row above — wake
        // that whole band, or distant surface water would never notice the drain.
        if (src_cell == kEmptyMaterial)
            wakeSpan(f, xi - kLiquidDispersion, yi,
                        xi + kLiquidDispersion, yi + 1, /*next=*/true);
        return true;
    }

    void PixelFieldRuntime::stepField(Field& f, std::uint32_t chunk_cx, std::uint32_t chunk_cy,
                                      std::uint32_t& moved, std::uint32_t& scanned)
    {
        const std::uint32_t w = f.desc.cells_w, h = f.desc.cells_h;

        // This chunk's tile slice. Tiles bottom-up; rows bottom-up inside each
        // tile row band, and only WITHIN the tile's dirty RECT (the Noita-style
        // refinement: an active tile scans its ±1-expanded activity band, never
        // its settled bulk). Chunk borders are crossed by plain global
        // addressing (cellAt), no halo copies.
        constexpr std::uint32_t kTilesPerChunk = kChunkSizeCells / kTileSize;
        const std::uint32_t cty0 = chunk_cy * kTilesPerChunk;
        const std::uint32_t cty1 = std::min(cty0 + kTilesPerChunk, f.tiles_y);
        const std::uint32_t ctx0 = chunk_cx * kTilesPerChunk;
        const std::uint32_t ctx1 = std::min(ctx0 + kTilesPerChunk, f.tiles_x);

        for (std::uint32_t ty = cty0; ty < cty1; ++ty)
        {
            for (std::uint32_t tx = ctx0; tx < ctx1; ++tx)
            {
                const std::size_t t = static_cast<std::size_t>(ty) * f.tiles_x + tx;
                if (!f.active[t])
                    continue;
                const std::uint32_t y0 = std::min(ty * kTileSize + f.rmin_y[t], h - 1);
                const std::uint32_t y1 = std::min(ty * kTileSize + f.rmax_y[t] + 1u, h);
                const std::uint32_t x0 = std::min(tx * kTileSize + f.rmin_x[t], w - 1);
                const std::uint32_t x1 = std::min(tx * kTileSize + f.rmax_x[t] + 1u, w);
                for (std::uint32_t y = y0; y < y1; ++y)
                {
                    const bool rtl = ((f.steps + y) & 1ull) != 0ull;
                    const std::int32_t first = ((f.steps + y) & 1ull) ? 1 : -1;   // diag/lateral side parity
                    for (std::uint32_t k = 0; k < x1 - x0; ++k)
                    {
                        const std::uint32_t x = rtl ? (x1 - 1u - k) : (x0 + k);
                        ++scanned;
                        const MaterialId    id = f.cellAt(x, y);
                        if (id == kEmptyMaterial || f.movedAt(x, y))
                            continue;

                        const EMaterialPhase phase = materials_.at(id).phase;
                        const std::int32_t xi = static_cast<std::int32_t>(x);
                        const std::int32_t yi = static_cast<std::int32_t>(y);
                        if (phase == EMaterialPhase::Powder || phase == EMaterialPhase::Liquid)
                        {
                            // Terminal-velocity free fall: drop through up to
                            // kMaxFallCells EMPTY cells in one step. The scan
                            // stops at any wall (non-empty / border / unloaded
                            // chunk); the landing cell is empty by scan, so
                            // tryMove is the plain empty-dst move.
                            std::int32_t d = 0;
                            while (d < kMaxFallCells)
                            {
                                const std::int32_t ny = yi - (d + 1);
                                if (ny < 0) break;
                                const std::uint32_t uy = static_cast<std::uint32_t>(ny);
                                if (!f.chunkAt(x, uy).resident) break;   // C2-02b wall
                                if (f.cellAt(x, uy) != kEmptyMaterial) break;
                                ++d;
                            }
                            if (d > 0 && tryMove(f, x, y, xi, yi - d, false, moved)) continue;
                        }
                        if (phase == EMaterialPhase::Powder)
                        {
                            // below is occupied (fall found no gap): sink into a
                            // lighter liquid at drag speed, else pile diagonally.
                            if (tryMove(f, x, y, xi, yi - 1, true, moved)) continue;
                            if (tryMove(f, x, y, xi + first, yi - 1, true, moved)) continue;
                            if (tryMove(f, x, y, xi - first, yi - 1, true, moved)) continue;
                        }
                        else if (phase == EMaterialPhase::Liquid)
                        {
                            if (tryMove(f, x, y, xi + first, yi - 1, false, moved)) continue;
                            if (tryMove(f, x, y, xi - first, yi - 1, false, moved)) continue;
                            // Lateral DISPERSION rule: step sideways ONLY toward a
                            // visible drainage hole — scan up to kDispersion cells
                            // along this row (path must be empty; walls/liquid stop
                            // the scan) for a column whose cell BELOW is empty, and
                            // move ONE cell toward it. Motion is hole-directed, so a
                            // flat surface has no legal lateral move and settled
                            // water genuinely SLEEPS (a plain lateral rule shuffles
                            // in place forever — the classic falling-sand livelock).
                            if (yi - 1 >= 0)
                            {
                                const auto flowToward = [&](std::int32_t dir) noexcept -> bool
                                {
                                    for (std::int32_t k2 = 1; k2 <= kLiquidDispersion; ++k2)
                                    {
                                        const std::int32_t nx = xi + dir * k2;
                                        if (nx < 0 || nx >= static_cast<std::int32_t>(w))
                                            return false;   // wall
                                        const std::uint32_t unx = static_cast<std::uint32_t>(nx);
                                        if (!f.chunkAt(unx, y).resident)
                                            return false;   // C2-02b: wall
                                        if (f.cellAt(unx, y) != kEmptyMaterial)
                                            return false;   // path blocked on this row
                                        if (f.cellAt(unx, static_cast<std::uint32_t>(yi - 1)) == kEmptyMaterial)
                                            return tryMove(f, x, y, xi + dir, yi, false, moved);
                                    }
                                    return false;
                                };
                                if (flowToward(first)) continue;
                                if (flowToward(-first)) continue;
                            }
                        }
                        // Solid / Empty: static.
                    }
                }
            }
        }
    }

    void PixelFieldRuntime::stepField(Field& f)
    {
        const auto t0 = std::chrono::steady_clock::now();

        for (Chunk& c : f.chunks)
            if (c.resident)
                std::memset(c.moved.data(), 0, c.moved.size());
        std::fill(f.changed.begin(), f.changed.end(), std::uint8_t{0});
        std::fill(f.active_next.begin(), f.active_next.end(), std::uint8_t{0});
        f.moved_cells_last = 0;
        f.cells_scanned_last = 0;

        // I2-02: four-colour (cx&1, cy&1) chunk schedule. Same-colour chunks
        // are ≥ 2 chunks (512 cells) apart; a chunk's scan writes at most
        // kLiquidDispersion+1 cells past its own border (tryMove reach 1 +
        // wakeSpan band) and reads at most kLiquidDispersion — so two
        // same-colour chunks can NEVER touch the same cell or the same tile
        // bookkeeping entry, and each chunk's result is independent of the
        // execution order inside its colour group. That makes 1 thread ≡ N
        // threads by construction (the frozen determinism model: the hash is
        // a function of (cells, step index, schedule), never of thread count).
        // The static_assert is the rule-evolution hard guard: any future rule
        // whose reach exceeds one tile breaks the colour argument and must
        // widen the colouring instead of silently racing.
        static_assert(kLiquidDispersion + 1 <= static_cast<std::int32_t>(kTileSize) &&
                      kMaxFallCells + 1 <= static_cast<std::int32_t>(kTileSize),
                      "four-colour schedule invariant: cross-chunk write reach (lateral "
                      "dispersion band AND multi-cell fall) must stay within the first "
                      "tile of a neighbour chunk");

        struct WorkItem { std::uint32_t cx, cy, moved{0}, scanned{0}; };
        std::vector<WorkItem> items;
        items.reserve((static_cast<std::size_t>(f.chunks_x) * f.chunks_y + 3) / 4);

        for (std::uint32_t colour = 0; colour < 4; ++colour)
        {
            const std::uint32_t px = colour & 1u, py = colour >> 1;
            items.clear();
            for (std::uint32_t cy = py; cy < f.chunks_y; cy += 2)
                for (std::uint32_t cx = px; cx < f.chunks_x; cx += 2)
                    if (f.chunks[static_cast<std::size_t>(cy) * f.chunks_x + cx].resident)
                        items.push_back(WorkItem{cx, cy});
            if (items.empty()) continue;

            if (pool_ == nullptr || worker_threads_ <= 1 || items.size() <= 1)
            {
                // Serial path: the SAME colour-major order — 1 ≡ N.
                for (WorkItem& it : items)
                    stepField(f, it.cx, it.cy, it.moved, it.scanned);
            }
            else
            {
                pool_->run(static_cast<std::uint32_t>(items.size()),
                           [&](std::uint32_t i)
                           {
                               WorkItem& it = items[i];
                               stepField(f, it.cx, it.cy, it.moved, it.scanned);
                           });
            }
            for (const WorkItem& it : items)
            {
                f.moved_cells_last += it.moved;
                f.cells_scanned_last += it.scanned;
            }
        }

        // Upload dirt: one rect per changed tile (per-chunk planners coalesce;
        // markRectDirty splits tiles straddling a chunk border — with 256 % 32
        // == 0 a tile never straddles, but the split is correct regardless).
        for (std::uint32_t ty = 0; ty < f.tiles_y; ++ty)
            for (std::uint32_t tx = 0; tx < f.tiles_x; ++tx)
                if (f.changed[static_cast<std::size_t>(ty) * f.tiles_x + tx])
                    markRectDirty(f, tx * kTileSize, ty * kTileSize, kTileSize, kTileSize);

        f.active.swap(f.active_next);
        f.rmin_x.swap(f.nrmin_x); f.rmin_y.swap(f.nrmin_y);
        f.rmax_x.swap(f.nrmax_x); f.rmax_y.swap(f.nrmax_y);
        ++f.steps;
        f.step_ms_last =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    }

    void PixelFieldRuntime::step()
    {
        ++transfer_epoch_;   // I2-01: a CA step is a field mutation
        for (Field& f : fields_)
            if (f.alive)
                stepField(f);
    }

    // ── queries (F2-06: const, never mutate) ─────────────────────────────────

    MaterialId PixelFieldRuntime::samplePoint(PixelFieldHandle h, Eigen::Vector2i cell) const noexcept
    {
        const Field* f = resolve(h);
        if (!f || cell.x() < 0 || cell.y() < 0 ||
            cell.x() >= static_cast<std::int32_t>(f->desc.cells_w) ||
            cell.y() >= static_cast<std::int32_t>(f->desc.cells_h))
            return kEmptyMaterial;
        if (!f->chunkAt(static_cast<std::uint32_t>(cell.x()),
                        static_cast<std::uint32_t>(cell.y())).resident)
            return kEmptyMaterial;   // C2-02b: unloaded reads empty
        return f->cellAt(static_cast<std::uint32_t>(cell.x()),
                         static_cast<std::uint32_t>(cell.y()));
    }

    void PixelFieldRuntime::sampleRegion(PixelFieldHandle h, Eigen::Vector2i min, Eigen::Vector2i size,
                                         std::vector<MaterialId>& out) const
    {
        out.clear();
        if (size.x() <= 0 || size.y() <= 0) return;
        out.reserve(static_cast<std::size_t>(size.x()) * size.y());
        for (std::int32_t y = min.y(); y < min.y() + size.y(); ++y)
            for (std::int32_t x = min.x(); x < min.x() + size.x(); ++x)
                out.push_back(samplePoint(h, {x, y}));
    }

    PixelRaycastHit PixelFieldRuntime::raycast(PixelFieldHandle h, Eigen::Vector2f from_cell,
                                               Eigen::Vector2f dir, float max_cells) const
    {
        PixelRaycastHit out{};
        const Field* f = resolve(h);
        const float len = dir.norm();
        if (!f || len <= 1e-6f || max_cells <= 0.f)
            return out;
        const Eigen::Vector2f d = dir / len;

        constexpr float kStep = 0.45f;   // < half a cell → cannot tunnel through a cell
        Eigen::Vector2i last{INT32_MIN, INT32_MIN};
        for (float t = 0.f; t <= max_cells; t += kStep)
        {
            const Eigen::Vector2f p = from_cell + d * t;
            const Eigen::Vector2i c{static_cast<std::int32_t>(std::floor(p.x())),
                                    static_cast<std::int32_t>(std::floor(p.y()))};
            if (c == last) continue;
            last = c;
            const MaterialId id = samplePoint(h, c);
            if (id != kEmptyMaterial)
            {
                out.hit      = true;
                out.cell     = c;
                out.material = id;
                return out;
            }
        }
        return out;
    }

    // ── observability ────────────────────────────────────────────────────────

    std::uint64_t PixelFieldRuntime::determinismHash(PixelFieldHandle h) const noexcept
    {
        // Logical global row-major hash: a pure function of the field's CONTENT
        // (chunk layout invisible), fed row-segment-wise (contiguous per chunk).
        const Field* f = resolve(h);
        if (!f) return 0;
        std::uint64_t hash = kFnvBasis;
        const std::uint32_t w = f->desc.cells_w;
        for (std::uint32_t y = 0; y < f->desc.cells_h; ++y)
            for (std::uint32_t x = 0; x < w; )
            {
                const std::uint32_t seg = std::min(kChunkSizeCells - (x & kChunkMask), w - x);
                const Chunk& c = f->chunkAt(x, y);
                if (c.resident)   // C2-02b: the fingerprint covers RESIDENT content
                    hash = fnv1a64Append(hash, &c.cells[Field::localIndex(x, y)],
                                         static_cast<std::size_t>(seg) * sizeof(MaterialId));
                x += seg;
            }
        return hash;
    }
    std::uint32_t PixelFieldRuntime::movedCellsLastStep(PixelFieldHandle h) const noexcept
    {
        const Field* f = resolve(h);
        return f ? f->moved_cells_last : 0;
    }
    std::uint32_t PixelFieldRuntime::cellsScannedLastStep(PixelFieldHandle h) const noexcept
    {
        const Field* f = resolve(h);
        return f ? f->cells_scanned_last : 0;
    }
    std::uint32_t PixelFieldRuntime::activeTiles(PixelFieldHandle h) const noexcept
    {
        const Field* f = resolve(h);
        if (!f) return 0;
        std::uint32_t n = 0;
        for (const auto a : f->active) n += a ? 1u : 0u;
        return n;
    }
    double PixelFieldRuntime::stepMillisLast(PixelFieldHandle h) const noexcept
    {
        const Field* f = resolve(h);
        return f ? f->step_ms_last : 0.0;
    }

    // ── C2-00: chunk geometry + per-chunk export seam ────────────────────────

    std::uint32_t PixelFieldRuntime::chunksX(PixelFieldHandle h) const noexcept
    {
        const Field* f = resolve(h);
        return f ? f->chunks_x : 0;
    }
    std::uint32_t PixelFieldRuntime::chunksY(PixelFieldHandle h) const noexcept
    {
        const Field* f = resolve(h);
        return f ? f->chunks_y : 0;
    }
    Eigen::Vector2i PixelFieldRuntime::chunkCells(PixelFieldHandle h,
                                                  std::uint32_t cx, std::uint32_t cy) const noexcept
    {
        const Field* f = resolve(h);
        if (!f || cx >= f->chunks_x || cy >= f->chunks_y) return {0, 0};
        return {static_cast<std::int32_t>(std::min(kChunkSizeCells, f->desc.cells_w - cx * kChunkSizeCells)),
                static_cast<std::int32_t>(std::min(kChunkSizeCells, f->desc.cells_h - cy * kChunkSizeCells))};
    }

    lux::render::RegionUploadPlanner* PixelFieldRuntime::uploadPlanner(
        PixelFieldHandle h, std::uint32_t cx, std::uint32_t cy) noexcept
    {
        Field* f = resolve(h);
        if (!f || cx >= f->chunks_x || cy >= f->chunks_y) return nullptr;
        return &f->chunks[static_cast<std::size_t>(cy) * f->chunks_x + cx].planner;
    }

    PixelFieldRenderExport PixelFieldRuntime::exportDirty(PixelFieldHandle h,
                                                          std::uint32_t cx, std::uint32_t cy,
                                                          const lux::render::RegionUploadBudget& budget)
    {
        PixelFieldRenderExport out{};
        Field* f = resolve(h);
        if (!f || cx >= f->chunks_x || cy >= f->chunks_y) return out;
        Chunk& c = f->chunks[static_cast<std::size_t>(cy) * f->chunks_x + cx];
        if (!c.resident) return out;   // C2-02b: no mirror content while unloaded

        auto plan = c.planner.takeBatch(sizeof(MaterialId), budget);
        if (plan.empty()) return out;

        // Pack tight rows from the authoritative chunk cells at each region's
        // assigned data_offset — an OWNED copy, immutable from here on. Region
        // coords are chunk-local; a chunk row is CONTIGUOUS in its block.
        auto pixels = std::shared_ptr<std::byte[]>(new std::byte[plan.pixel_bytes]);
        for (const auto& r : plan.regions)
        {
            const std::size_t row_bytes = static_cast<std::size_t>(r.width) * sizeof(MaterialId);
            for (std::uint32_t row = 0; row < r.height; ++row)
                std::memcpy(pixels.get() + r.data_offset + static_cast<std::size_t>(row) * row_bytes,
                            &c.cells[(static_cast<std::size_t>(r.y + row) << kChunkShift) + r.x],
                            row_bytes);
        }

        out.regions          = std::move(plan.regions);
        out.pixels           = std::move(pixels);
        out.pixel_bytes      = plan.pixel_bytes;
        out.content_revision = plan.content_revision;
        return out;
    }

    void PixelFieldRuntime::confirmExport(PixelFieldHandle h, std::uint32_t cx, std::uint32_t cy,
                                          std::uint64_t revision,
                                          lux::render::ERegionUploadStatus status)
    {
        if (auto* planner = uploadPlanner(h, cx, cy))
            planner->onAck(revision, status);
    }

    std::uint64_t PixelFieldRuntime::uploadedRevision(PixelFieldHandle h) const noexcept
    {
        const Field* f = resolve(h);
        if (!f) return 0;
        std::uint64_t sum = 0;
        for (const Chunk& c : f->chunks)
            sum += c.planner.uploadedRevision();
        return sum;
    }

} // namespace lux::pack
