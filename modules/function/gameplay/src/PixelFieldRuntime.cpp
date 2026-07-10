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

#include <lux/engine/gameplay/2d/pixel/PixelFieldRuntime.hpp>
#include <lux/engine/gameplay/2d/pixel/PixelField2DComponent.hpp>
#include <lux/engine/gameplay/2d/world/components/WorldTransform2DComponent.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace lux::gameplay::d2
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
    } // namespace

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
        // Wake the chunk (+1 border cell so neighbours re-evaluate the seam).
        wakeSpan(*f,
                 static_cast<std::int32_t>(cx * kChunkSizeCells) - 1,
                 static_cast<std::int32_t>(cy * kChunkSizeCells) - 1,
                 static_cast<std::int32_t>(cx * kChunkSizeCells + cw),
                 static_cast<std::int32_t>(cy * kChunkSizeCells + ch),
                 /*next=*/false);
        return true;
    }

    bool PixelFieldRuntime::chunkResident(PixelFieldHandle h,
                                          std::uint32_t cx, std::uint32_t cy) const noexcept
    {
        const Field* f = resolve(h);
        if (!f || cx >= f->chunks_x || cy >= f->chunks_y) return false;
        return f->chunks[static_cast<std::size_t>(cy) * f->chunks_x + cx].resident;
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
        for (const PixelFieldCommand& cmd : commands_)
        {
            Field* f = resolve(cmd.field);
            if (!f) continue;   // stale handle → inert

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
                    if (cell != cmd.material) { cell = cmd.material; ++changed; }
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
                                    std::int32_t nx, std::int32_t ny, bool displace_liquid)
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
        ++f.moved_cells_last;

        const std::uint32_t stx = x / kTileSize, sty = y / kTileSize;
        const std::uint32_t dtx = ux / kTileSize;
        const std::uint32_t dty = uy / kTileSize;
        f.changed[static_cast<std::size_t>(sty) * f.tiles_x + stx] = 1;
        f.changed[static_cast<std::size_t>(dty) * f.tiles_x + dtx] = 1;

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

    void PixelFieldRuntime::stepField(Field& f)
    {
        const auto t0 = std::chrono::steady_clock::now();

        for (Chunk& c : f.chunks)
            if (c.resident)
                std::memset(c.moved.data(), 0, c.moved.size());
        std::fill(f.changed.begin(), f.changed.end(), std::uint8_t{0});
        std::fill(f.active_next.begin(), f.active_next.end(), std::uint8_t{0});
        f.moved_cells_last = 0;

        const std::uint32_t w = f.desc.cells_w, h = f.desc.cells_h;
        f.cells_scanned_last = 0;

        // Tiles bottom-up; rows bottom-up inside each tile row band, and only
        // WITHIN the tile's dirty RECT (the Noita-style refinement: an active
        // tile scans its ±1-expanded activity band, never its settled bulk).
        // Cell order stays a FIXED total order — a pure function of
        // (cells, step index) — so determinism is untouched. Chunk borders are
        // crossed by plain global addressing (cellAt), no special cases.
        for (std::uint32_t ty = 0; ty < f.tiles_y; ++ty)
        {
            for (std::uint32_t tx = 0; tx < f.tiles_x; ++tx)
            {
                const std::size_t t = static_cast<std::size_t>(ty) * f.tiles_x + tx;
                if (!f.active[t])
                    continue;
                if (!f.chunkAt(tx * kTileSize, ty * kTileSize).resident)
                    continue;   // C2-02b: unloaded chunks are never scanned
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
                        ++f.cells_scanned_last;
                        const MaterialId    id = f.cellAt(x, y);
                        if (id == kEmptyMaterial || f.movedAt(x, y))
                            continue;

                        const EMaterialPhase phase = materials_.at(id).phase;
                        const std::int32_t xi = static_cast<std::int32_t>(x);
                        const std::int32_t yi = static_cast<std::int32_t>(y);
                        if (phase == EMaterialPhase::Powder)
                        {
                            if (tryMove(f, x, y, xi, yi - 1, true)) continue;
                            if (tryMove(f, x, y, xi + first, yi - 1, true)) continue;
                            if (tryMove(f, x, y, xi - first, yi - 1, true)) continue;
                        }
                        else if (phase == EMaterialPhase::Liquid)
                        {
                            if (tryMove(f, x, y, xi, yi - 1, false)) continue;
                            if (tryMove(f, x, y, xi + first, yi - 1, false)) continue;
                            if (tryMove(f, x, y, xi - first, yi - 1, false)) continue;
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
                                            return tryMove(f, x, y, xi + dir, yi, false);
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

} // namespace lux::gameplay::d2
