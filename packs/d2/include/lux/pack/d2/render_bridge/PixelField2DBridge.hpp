#pragma once
// ============================================================================
//  PixelField2DBridge.hpp — ECS PixelField2DComponent → GPU-resident canvas
//  field instances over ONE scene ATLAS texture (lux::pack, F2-08;
//  per-chunk mirrors C2-00; atlas + camera-driven residency C2-01).
//
//  RETAINED bridge. The render mirror is ONE big R16_UNORM ATLAS per bridge
//  (kAtlasSlots² slots of 256² texels = one chunk each) + one canvas
//  PixelField instance PER RESIDENT CHUNK carrying its slot origin
//  (atlas_x/y — texelFetch reads atlas_origin + cell; exact texels, no
//  filtering, so slots need no gutters).
//
//  RESIDENCY (C2-01): a chunk holds a slot + instance only while its world
//  rect intersects the ACTIVE CAMERA's view rect, with hysteresis (enter
//  margin < keep margin, so the boundary never thrashes). Leaving the keep
//  band evicts: instance removed, slot freed. Reviving is FREE-of-loss by
//  construction: an unslotted chunk's planner keeps accumulating dirt
//  (defer-never-lose), and acquisition marks the whole chunk dirty anyway —
//  the first exports after revival repaint the slot completely. A full
//  allocator degrades by SKIPPING new acquisitions (cap — farthest content
//  simply stays undrawn; never a crash or flicker of what IS drawn). Scenes
//  with no active camera treat every chunk as visible (budget-capped).
//
//  Steady state per frame (per live field):
//    - CONTENT: for each RESIDENT chunk, runtime.exportDirty(field, cx, cy)
//      → regions offset by the slot origin → updateTextureRegions(atlas) →
//      the reply's status routes back via runtime.confirmExport (per-chunk
//      revision; non-Ok re-dirties). A settled chunk exports NOTHING.
//    - PLACEMENT/KEY: field affine + priority/visible value-diffed and
//      broadcast to the chunks that HAVE instances.
//
//  MVP constraint (记档): the slot origin is baked into the instance (no
//  visual-update op for the field kind) — a slot change = instance
//  remove+recreate. Evict/revive is exactly that, and it is rare by design
//  (hysteresis); a hot path would justify a new op, not sooner.
// ============================================================================

#include <lux/engine/render_bridge/IRenderableBridge.hpp>
#include <lux/engine/render_bridge/RenderableBridgeContext.hpp>
#include <lux/pack/d2/pixel/PixelField2DComponent.hpp>
#include <lux/pack/d2/pixel/PixelFieldRuntime.hpp>
#include <lux/pack/d2/world/components/WorldTransform2DComponent.hpp>
#include <lux/pack/d2/world/components/Camera2DComponent.hpp>
#include <lux/engine/render/renderer/features/canvas2d/Canvas2DOperation.hpp>
#include <lux/engine/render/comm/client/RenderSession.hpp>
#include <lux/engine/render/comm/client/RenderRequest.hpp>
#include <lux/engine/meta/LuxObject.hpp>

#include <cstdint>
#include <cstring>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lux::pack
{
    using lux::render_bridge::RenderableBridgeContext;

    class PixelField2DBridge final : public lux::render_bridge::IRenderableBridge
    {
        static constexpr std::uint32_t kNoSlot      = 0xFFFFFFFFu;
        static constexpr std::uint32_t kAtlasSlotsX = 8;    ///< 8×8 slots of 256² = 2048² R16 (8 MiB)
        static constexpr std::uint32_t kAtlasSlotsY = 8;
        static constexpr float kEnterMarginChunks   = 1.f;  ///< acquire when this close
        static constexpr float kKeepMarginChunks    = 2.f;  ///< release only past this (hysteresis)

        struct ChunkRec
        {
            std::uint32_t                         slot{kNoSlot};
            lux::render::PixelFieldInstanceHandle instance{};
            bool                                  pending{false};   ///< create reply in flight
        };
        struct Live
        {
            PixelFieldHandle      field{};
            std::uint32_t         chunks_x{0}, chunks_y{0};
            std::vector<ChunkRec> chunks;
            float                 m[6]{};       ///< last sent FIELD affine
            float                 priority{0.f};
            bool                  visible{true};
        };

    public:
        explicit PixelField2DBridge(PixelFieldRuntime* runtime) noexcept : runtime_(runtime)
        {
            free_slots_.reserve(kAtlasSlotsX * kAtlasSlotsY);
            for (std::uint32_t i = kAtlasSlotsX * kAtlasSlotsY; i > 0; --i)
                free_slots_.push_back(i - 1);
        }

        ~PixelField2DBridge() override
        {
            atlas_pending_.cancel();
            palette_pending_.cancel();
        }

        void drive(lux::meta::EntityRegistry& registry, RenderableBridgeContext& ctx) override
        {
            auto canvas = ctx.canvas2d();
            if (!canvas.valid() || runtime_ == nullptr) return;

            ensureAtlas(ctx);
            ensurePalette(ctx);

            // The residency window: the active camera's world rect (huge when
            // no camera — every chunk then competes for the slot budget).
            Eigen::Vector2f cam_min{-1e9f, -1e9f}, cam_max{1e9f, 1e9f};
            for (auto ce : registry.view<ActiveCamera2DTag, Camera2DComponent,
                                         WorldTransform2DComponent>())
            {
                const auto& cc = registry.get<Camera2DComponent>(ce);
                const auto& wt = registry.get<WorldTransform2DComponent>(ce);
                const float hh = cc.units_per_view_height * 0.5f;
                const float hw = hh * cc.aspect;
                const Eigen::Vector2f c{wt.world(0, 3), wt.world(1, 3)};
                cam_min = c - Eigen::Vector2f(hw, hh);
                cam_max = c + Eigen::Vector2f(hw, hh);
                break;
            }

            registry.view<PixelField2DComponent, WorldTransform2DComponent>().each(
                [&](lux::meta::entity_id e, const PixelField2DComponent& pc,
                    const WorldTransform2DComponent& wt)
                {
                    if (!runtime_->isAlive(pc.field)) return;
                    const auto d = runtime_->desc(pc.field);

                    float m[6];
                    m[0] = static_cast<float>(d.cells_w) * pc.cell_size;
                    m[1] = 0.f;
                    m[2] = 0.f;
                    m[3] = static_cast<float>(d.cells_h) * pc.cell_size;
                    m[4] = wt.world(0, 3);
                    m[5] = wt.world(1, 3);

                    auto it = live_.find(e);
                    if (it == live_.end())
                    {
                        // First sight is SYNCHRONOUS now (no per-chunk textures):
                        // just the record grid; chunks acquire lazily below.
                        Live L{};
                        L.field    = pc.field;
                        L.chunks_x = runtime_->chunksX(pc.field);
                        L.chunks_y = runtime_->chunksY(pc.field);
                        L.chunks.resize(static_cast<std::size_t>(L.chunks_x) * L.chunks_y);
                        std::memcpy(L.m, m, sizeof(m));
                        L.priority = pc.priority;
                        L.visible  = pc.visible;
                        it = live_.emplace(e, std::move(L)).first;
                    }
                    driveLive(ctx, canvas, e, it->second, pc, m, cam_min, cam_max);
                });
        }

        void reap(lux::meta::EntityRegistry& registry, RenderableBridgeContext& ctx) override
        {
            auto canvas = ctx.canvas2d();
            for (auto it = live_.begin(); it != live_.end(); )
            {
                const auto e = it->first;
                if (!registry.valid(e) ||
                    !registry.all_of<PixelField2DComponent, WorldTransform2DComponent>(e))
                {
                    releaseAllChunks(ctx, canvas, it->second);
                    it = live_.erase(it);
                }
                else ++it;
            }
        }

        void beginShutdown(RenderableBridgeContext& ctx) override
        {
            auto canvas = ctx.canvas2d();
            for (auto& [e, L] : live_)
                releaseAllChunks(ctx, canvas, L);
            live_.clear();
            if (!atlas_texture_.is_null())
            {
                ctx.session().destroyTexture(atlas_texture_);
                atlas_texture_  = {};
                atlas_bindless_ = lux::render::kNoTexture;
            }
            if (!palette_texture_.is_null())
            {
                ctx.session().destroyTexture(palette_texture_);
                palette_texture_  = {};
                palette_bindless_ = lux::render::kNoTexture;
            }
            stopping_ = true;   // pending create replies route into the orphan list
        }

        [[nodiscard]] bool hasPendingShutdownWork() const override
        {
            return pending_creates_ > 0;
        }

        void flushShutdownCleanup(RenderableBridgeContext& ctx) override
        {
            auto canvas = ctx.canvas2d();
            for (const auto& h : orphan_instances_)
                canvas.removePixelField(ctx.scene(), h);
            orphan_instances_.clear();
            for (const auto& t : orphan_textures_)
                ctx.session().destroyTexture(t);
            orphan_textures_.clear();
        }

        // ── observability (tests) ──
        [[nodiscard]] std::uint32_t residentChunks() const noexcept
        {
            std::uint32_t n = 0;
            for (const auto& [e, L] : live_)
                for (const auto& c : L.chunks)
                    n += (c.slot != kNoSlot) ? 1u : 0u;
            return n;
        }
        [[nodiscard]] std::uint32_t freeSlots() const noexcept
        {
            return static_cast<std::uint32_t>(free_slots_.size());
        }

    private:
        /// Per-frame PER-CHUNK content budget (attachment channel, PCIe pacing).
        static constexpr lux::render::RegionUploadBudget kUploadBudget{1u << 20, 64};

        void driveLive(RenderableBridgeContext& ctx, lux::render::Canvas2DProxy& canvas,
                       lux::meta::entity_id e, Live& L, const PixelField2DComponent& pc,
                       const float m[6],
                       const Eigen::Vector2f& cam_min, const Eigen::Vector2f& cam_max)
        {
            const bool moved = std::memcmp(m, L.m, sizeof(float) * 6) != 0;
            if (moved) std::memcpy(L.m, m, sizeof(float) * 6);
            const bool rekey = (pc.priority != L.priority || pc.visible != L.visible);
            if (rekey) { L.priority = pc.priority; L.visible = pc.visible; }

            const float chunk_world =
                static_cast<float>(PixelFieldRuntime::kChunkSizeCells) * pc.cell_size;

            for (std::uint32_t cy = 0; cy < L.chunks_y; ++cy)
                for (std::uint32_t cx = 0; cx < L.chunks_x; ++cx)
                {
                    ChunkRec& rec = L.chunks[static_cast<std::size_t>(cy) * L.chunks_x + cx];
                    const auto cc = runtime_->chunkCells(L.field, cx, cy);
                    const Eigen::Vector2f cmin{
                        m[4] + static_cast<float>(cx) * chunk_world,
                        m[5] + static_cast<float>(cy) * chunk_world};
                    const Eigen::Vector2f cmax = cmin +
                        Eigen::Vector2f(static_cast<float>(cc.x()),
                                        static_cast<float>(cc.y())) * pc.cell_size;

                    const auto intersects = [&](float margin_chunks)
                    {
                        const float mgn = margin_chunks * chunk_world;
                        return cmin.x() < cam_max.x() + mgn && cmax.x() > cam_min.x() - mgn &&
                               cmin.y() < cam_max.y() + mgn && cmax.y() > cam_min.y() - mgn;
                    };

                    // C2-02b: a CPU-unloaded chunk has no mirror content — never
                    // acquire it, and evict a stale instance if it just unloaded.
                    const bool cpu_resident = runtime_->chunkResident(L.field, cx, cy);

                    if (rec.slot == kNoSlot)
                    {
                        if (!cpu_resident) continue;
                        if (!intersects(kEnterMarginChunks)) continue;
                        if (atlas_bindless_ == lux::render::kNoTexture ||
                            palette_bindless_ == lux::render::kNoTexture)
                            continue;   // atlas/palette still in flight
                        if (rec.pending) continue;
                        if (free_slots_.empty()) continue;   // cap: farthest stays undrawn
                        rec.slot = free_slots_.back();
                        free_slots_.pop_back();
                        // Repaint the slot completely on acquisition.
                        if (auto* planner = runtime_->uploadPlanner(L.field, cx, cy))
                            planner->markDirty(0, 0, static_cast<std::uint32_t>(cc.x()),
                                               static_cast<std::uint32_t>(cc.y()));
                        createInstance(ctx, canvas, e, L, pc, cx, cy, rec);
                        continue;
                    }

                    // Resident: evict past the keep band OR when the CPU side
                    // unloaded (never while a create is pending).
                    if (!intersects(kKeepMarginChunks) || !cpu_resident)
                    {
                        if (rec.pending) continue;   // let the create land first
                        if (rec.instance.valid())
                            canvas.removePixelField(ctx.scene(), rec.instance);
                        free_slots_.push_back(rec.slot);
                        rec = ChunkRec{};
                        continue;
                    }

                    if (rec.instance.valid())
                    {
                        if (moved)
                        {
                            float cm[6];
                            chunkAffine(pc, m, cx, cy, cc, cm);
                            canvas.updatePixelFieldTransform(ctx.scene(), rec.instance, cm);
                        }
                        if (rekey)
                            canvas.updatePixelFieldKey(ctx.scene(), rec.instance,
                                                       pc.priority, pc.visible);

                        // CONTENT: chunk-local regions → atlas coordinates.
                        auto ex = runtime_->exportDirty(L.field, cx, cy, kUploadBudget);
                        if (!ex.empty())
                        {
                            const std::uint32_t ox =
                                (rec.slot % kAtlasSlotsX) * PixelFieldRuntime::kChunkSizeCells;
                            const std::uint32_t oy =
                                (rec.slot / kAtlasSlotsX) * PixelFieldRuntime::kChunkSizeCells;
                            for (auto& r : ex.regions) { r.x += ox; r.y += oy; }

                            lux::render::OwnedTextureUploadBatch batch{};
                            batch.dst              = atlas_texture_;
                            batch.content_revision = ex.content_revision;
                            batch.regions          = std::move(ex.regions);
                            batch.pixels           = ex.pixels;
                            batch.pixel_bytes      = ex.pixel_bytes;
                            auto req = ctx.session().updateTextureRegions(batch);
                            PixelFieldRuntime* rt = runtime_;
                            const PixelFieldHandle fh = L.field;
                            const std::uint64_t rev = ex.content_revision;
                            req.then([rt, fh, cx, cy, rev](const lux::render::TextureRegionsAppliedReply& r)
                            {
                                rt->confirmExport(fh, cx, cy, rev,
                                    static_cast<lux::render::ERegionUploadStatus>(r.status));
                            });
                        }
                    }
                }
        }

        void createInstance(RenderableBridgeContext& ctx, lux::render::Canvas2DProxy& canvas,
                            lux::meta::entity_id e, Live& L, const PixelField2DComponent& pc,
                            std::uint32_t cx, std::uint32_t cy, ChunkRec& rec)
        {
            const auto cc = runtime_->chunkCells(L.field, cx, cy);
            lux::render::PixelField2DInstanceData data{};
            chunkAffine(pc, L.m, cx, cy, cc, data.m);
            data.field_texture   = atlas_bindless_;
            data.palette_texture = palette_bindless_;
            data.cells_w         = static_cast<std::uint32_t>(cc.x());
            data.cells_h         = static_cast<std::uint32_t>(cc.y());
            data.atlas_x         = (rec.slot % kAtlasSlotsX) * PixelFieldRuntime::kChunkSizeCells;
            data.atlas_y         = (rec.slot / kAtlasSlotsX) * PixelFieldRuntime::kChunkSizeCells;

            rec.pending = true;
            ++pending_creates_;
            const std::uint32_t idx = cy * L.chunks_x + cx;
            auto req = canvas.addPixelField(ctx.scene(), data, pc.priority, pc.visible);
            req.then([this, e, idx](const lux::render::PixelFieldSlotReply& r)
            {
                --pending_creates_;
                auto lit = live_.find(e);
                ChunkRec* rec2 = (lit != live_.end() && idx < lit->second.chunks.size())
                               ? &lit->second.chunks[idx] : nullptr;
                if (rec2) rec2->pending = false;
                if (r.status != lux::render::ECanvas2DCreateStatus::Ok || r.handle.is_null())
                {
                    if (r.handle.valid()) orphan_instances_.push_back(r.handle);
                    if (rec2 && rec2->slot != kNoSlot)
                    {
                        free_slots_.push_back(rec2->slot);   // retried next visibility pass
                        *rec2 = ChunkRec{};
                    }
                    return;
                }
                if (stopping_ || rec2 == nullptr)
                {
                    orphan_instances_.push_back(r.handle);
                    return;
                }
                rec2->instance = r.handle;
            });
            (void)req;   // fire-and-forget beyond the continuation
        }

        void releaseAllChunks(RenderableBridgeContext& ctx, lux::render::Canvas2DProxy& canvas,
                              Live& L)
        {
            for (auto& rec : L.chunks)
            {
                if (rec.instance.valid())
                    canvas.removePixelField(ctx.scene(), rec.instance);
                if (rec.slot != kNoSlot)
                    free_slots_.push_back(rec.slot);
                rec = ChunkRec{};
            }
        }

        void chunkAffine(const PixelField2DComponent& pc, const float field_m[6],
                         std::uint32_t cx, std::uint32_t cy, const Eigen::Vector2i& cc,
                         float out[6]) const
        {
            const float chunk_world =
                static_cast<float>(PixelFieldRuntime::kChunkSizeCells) * pc.cell_size;
            const float sx = static_cast<float>(cc.x()) * pc.cell_size;
            const float sy = static_cast<float>(cc.y()) * pc.cell_size;
            out[0] = sx;  out[1] = 0.f;
            out[2] = 0.f; out[3] = sy;
            out[4] = field_m[4] + static_cast<float>(cx) * chunk_world + sx * 0.5f;
            out[5] = field_m[5] + static_cast<float>(cy) * chunk_world + sy * 0.5f;
        }

        void ensureAtlas(RenderableBridgeContext& ctx)
        {
            if (!atlas_texture_.is_null() || atlas_creating_)
            {
                if (!atlas_texture_.is_null())
                    atlas_bindless_ = atlas_texture_.index;
                return;
            }
            atlas_creating_ = true;
            lux::render::PersistentTexture2DDesc td{};
            td.width  = kAtlasSlotsX * PixelFieldRuntime::kChunkSizeCells;
            td.height = kAtlasSlotsY * PixelFieldRuntime::kChunkSizeCells;
            td.format = lux::render::EPixelFormat::R16_UNORM;
            atlas_pending_ = ctx.session().createPersistentTexture2D(td);
            atlas_pending_.then([this](const lux::render::Texture2DCreatedReply& r)
            {
                atlas_creating_ = false;
                if (r.status != 0 || r.handle.is_null()) return;   // retried next frame
                if (stopping_) { orphan_textures_.push_back(r.handle); return; }
                atlas_texture_  = r.handle;
                atlas_bindless_ = r.handle.index;
            });
        }

        /// The scene-shared palette (unchanged from F2-09): create once, refill
        /// only when the material table grows.
        void ensurePalette(RenderableBridgeContext& ctx)
        {
            if (palette_texture_.is_null() && !palette_creating_)
            {
                palette_creating_ = true;
                lux::render::PersistentTexture2DDesc td{};
                td.width  = kPaletteWidth;
                td.height = 1;
                td.format = lux::render::EPixelFormat::RGBA8_UNORM;
                palette_pending_ = ctx.session().createPersistentTexture2D(td);
                palette_pending_.then([this](const lux::render::Texture2DCreatedReply& r)
                {
                    palette_creating_ = false;
                    if (r.status != 0 || r.handle.is_null()) return;
                    if (stopping_) { orphan_textures_.push_back(r.handle); return; }
                    palette_texture_ = r.handle;
                });
                return;
            }
            if (palette_texture_.is_null())
                return;

            const std::uint32_t count = runtime_->materials().count();
            if (count == palette_uploaded_count_)
                return;

            auto pixels = std::shared_ptr<std::byte[]>(new std::byte[kPaletteWidth * 4]);
            std::memset(pixels.get(), 0, kPaletteWidth * 4);
            auto* rgba = reinterpret_cast<std::uint32_t*>(pixels.get());
            const std::uint32_t n = count < kPaletteWidth ? count : kPaletteWidth;
            for (std::uint32_t i = 0; i < n; ++i)
                rgba[i] = runtime_->materials().at(static_cast<MaterialId>(i)).color;

            lux::render::OwnedTextureUploadBatch batch{};
            batch.dst              = palette_texture_;
            batch.content_revision = ++palette_revision_;
            lux::render::TextureRegionDesc r{};
            r.width  = kPaletteWidth;
            r.height = 1;
            batch.regions.push_back(r);
            batch.pixels      = pixels;
            batch.pixel_bytes = kPaletteWidth * 4;
            (void)ctx.session().updateTextureRegions(batch);
            palette_uploaded_count_ = count;
            palette_bindless_       = palette_texture_.index;
        }

        static constexpr std::uint32_t kPaletteWidth = 256;

        PixelFieldRuntime* runtime_{nullptr};

        std::unordered_map<lux::meta::entity_id, Live> live_;
        std::vector<std::uint32_t>                     free_slots_;
        std::uint32_t                                  pending_creates_{0};

        lux::render::RTextureHandle atlas_texture_{};
        lux::render::RenderRequest<lux::render::Texture2DCreatedReply> atlas_pending_{};
        std::uint32_t atlas_bindless_{lux::render::kNoTexture};
        bool          atlas_creating_{false};

        lux::render::RTextureHandle palette_texture_{};
        lux::render::RenderRequest<lux::render::Texture2DCreatedReply> palette_pending_{};
        std::uint32_t palette_bindless_{lux::render::kNoTexture};
        std::uint32_t palette_uploaded_count_{0};
        std::uint64_t palette_revision_{0};
        bool          palette_creating_{false};

        bool stopping_{false};
        std::vector<lux::render::PixelFieldInstanceHandle> orphan_instances_;
        std::vector<lux::render::RTextureHandle>           orphan_textures_;
    };

} // namespace lux::pack
