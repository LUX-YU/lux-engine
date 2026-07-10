#pragma once
// ============================================================================
//  PixelField2DBridge.hpp — ECS PixelField2DComponent → GPU-resident canvas
//  field instances + persistent id-mirror textures (lux::gameplay::d2,
//  F2-08; PER CHUNK since C2-00).
//
//  RETAINED bridge. Since C2-00 a field is a grid of 256²-cell chunks and the
//  render mirror is one R16_UNORM texture + one canvas PixelField instance PER
//  CHUNK (the arena's per-field-chunk semantics, now actually exercised).
//  First sight is a BUILDING state machine: ① create ALL chunk mirror
//  textures (each reply fills its slot), ② once every texture (and the
//  scene-shared palette) is ready, create ALL canvas instances (G-05
//  validated), ③ promote to Live. Any failure orphans what was created and
//  backs off (the F2-08 FailRecord discipline).
//
//  Steady state per frame (per live field):
//    - CONTENT: for each chunk, runtime.exportDirty(field, cx, cy, budget) →
//      OwnedTextureUploadBatch against THAT chunk's texture → the reply's
//      status routes back through runtime.confirmExport(field, cx, cy, …)
//      (Ok advances that chunk's uploadedRevision, anything else re-dirties —
//      defer-never-lose). A settled chunk exports NOTHING.
//    - PLACEMENT: the field affine is value-compared; a change re-sends every
//      chunk instance's transform (fields are FEW and mostly static).
//    - priority/visible value-diff → per-chunk UpdatePixelField2DKey.
//
//  The bridge never reads CA internals — only the export ticket, the chunk
//  geometry queries and the component. Reap removes every chunk instance AND
//  destroys every mirror texture; teardown drains pending creates into
//  orphans (two-phase contract).
// ============================================================================

#include <lux/engine/gameplay/render_bridge/IRenderableBridge.hpp>
#include <lux/engine/gameplay/render_bridge/RenderableBridgeContext.hpp>
#include <lux/engine/gameplay/2d/pixel/PixelField2DComponent.hpp>
#include <lux/engine/gameplay/2d/pixel/PixelFieldRuntime.hpp>
#include <lux/engine/gameplay/2d/world/components/WorldTransform2DComponent.hpp>
#include <lux/engine/render/renderer/features/canvas2d/Canvas2DOperation.hpp>
#include <lux/engine/render/comm/client/RenderSession.hpp>   // createPersistentTexture2D / updateTextureRegions / destroyTexture
#include <lux/engine/render/comm/client/RenderRequest.hpp>
#include <lux/engine/meta/LuxObject.hpp>

#include <cstdint>
#include <cstring>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lux::gameplay::d2
{
    class PixelField2DBridge final : public lux::gameplay::IRenderableBridge
    {
        struct ChunkSlot
        {
            lux::render::PixelFieldInstanceHandle instance{};
            lux::render::RTextureHandle           texture{};
        };

        struct Live
        {
            PixelFieldHandle       field{};
            std::uint32_t          chunks_x{0}, chunks_y{0};
            std::vector<ChunkSlot> slots;          ///< chunks_x*chunks_y, all populated
            float                  m[6]{};         ///< last sent FIELD affine (chunk affines derive)
            float                  priority{0.f};
            bool                   visible{true};
        };

        /// The per-entity first-sight state machine (shared_ptr so the async
        /// reply continuations keep it alive across frames).
        struct Building
        {
            PixelFieldHandle field{};
            std::uint32_t    chunks_x{0}, chunks_y{0};
            std::vector<lux::render::RTextureHandle>           textures;    ///< filled by replies
            std::vector<lux::render::PixelFieldInstanceHandle> instances;   ///< filled by replies
            std::uint32_t    textures_pending{0};
            std::uint32_t    instances_pending{0};
            bool             instances_issued{false};
            bool             failed{false};
            bool             failed_permanent{false};
            std::vector<lux::render::RenderRequest<lux::render::Texture2DCreatedReply>> tex_reqs;
            std::vector<lux::render::RenderRequest<lux::render::PixelFieldSlotReply>>   add_reqs;
        };

    public:
        explicit PixelField2DBridge(PixelFieldRuntime* runtime) noexcept : runtime_(runtime) {}

        ~PixelField2DBridge() override
        {
            for (auto& [e, b] : building_)
            {
                for (auto& r : b->tex_reqs) r.cancel();
                for (auto& r : b->add_reqs) r.cancel();
            }
            palette_pending_.cancel();   // idempotent; no-op on a stateless request
        }

        void drive(lux::meta::EntityRegistry& registry, RenderableBridgeContext& ctx) override
        {
            auto canvas = ctx.canvas2d();
            if (!canvas.valid() || runtime_ == nullptr) return;

            ensurePalette(ctx);

            registry.view<PixelField2DComponent, WorldTransform2DComponent>().each(
                [&](lux::meta::entity_id e, const PixelField2DComponent& pc,
                    const WorldTransform2DComponent& wt)
                {
                    if (!runtime_->isAlive(pc.field)) return;   // field not created yet / died
                    const auto d = runtime_->desc(pc.field);

                    // The FIELD affine (min corner = world translation, F2-02);
                    // per-chunk affines derive from it below.
                    float m[6];
                    m[0] = static_cast<float>(d.cells_w) * pc.cell_size;
                    m[1] = 0.f;
                    m[2] = 0.f;
                    m[3] = static_cast<float>(d.cells_h) * pc.cell_size;
                    m[4] = wt.world(0, 3);
                    m[5] = wt.world(1, 3);

                    if (auto it = live_.find(e); it != live_.end())
                    {
                        driveLive(ctx, canvas, it->second, pc, m);
                        return;
                    }
                    if (auto bit = building_.find(e); bit != building_.end())
                    {
                        advanceBuilding(ctx, canvas, e, *bit->second, pc, m);
                        return;
                    }
                    if (auto fit = failed_.find(e); fit != failed_.end())
                    {
                        if (fit->second.permanent) return;
                        if (--fit->second.retry_in > 0) return;
                        failed_.erase(fit);
                    }
                    startBuilding(ctx, e, pc);
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
                    destroyLive(ctx, canvas, it->second);
                    it = live_.erase(it);
                }
                else ++it;
            }
            std::erase_if(building_, [&](auto& kv) {
                if (registry.valid(kv.first) &&
                    registry.all_of<PixelField2DComponent, WorldTransform2DComponent>(kv.first))
                    return false;
                abandonBuilding(ctx, *kv.second);   // owner died mid-build → orphan the parts
                return true;
            });
            std::erase_if(failed_, [&](const auto& kv) {
                return !registry.valid(kv.first) ||
                       !registry.all_of<PixelField2DComponent, WorldTransform2DComponent>(kv.first);
            });
        }

        void beginShutdown(RenderableBridgeContext& ctx) override
        {
            auto canvas = ctx.canvas2d();
            for (auto& [e, L] : live_)
                destroyLive(ctx, canvas, L);
            live_.clear();
            for (auto& [e, b] : building_)
                abandonBuilding(ctx, *b);   // collected parts → destroyed now; late replies → orphans
            // keep building_ entries: their pending replies still route via stopping_
            if (!palette_texture_.is_null())
            {
                ctx.session().destroyTexture(palette_texture_);
                palette_texture_  = {};
                palette_bindless_ = lux::render::kNoTexture;
            }
            failed_.clear();
            stopping_ = true;
        }

        [[nodiscard]] bool hasPendingShutdownWork() const override
        {
            for (const auto& [e, b] : building_)
                if (b->textures_pending > 0 || b->instances_pending > 0)
                    return true;
            return false;
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
            building_.clear();
        }

    private:
        struct FailRecord
        {
            bool permanent{false};
            int  retry_in{0};
        };
        static constexpr int kRetryDrives = 120;
        /// Per-frame PER-CHUNK content budget. Pixels ride the ATTACHMENT
        /// channel (owned shared_ptr block), not the 64 KiB command ring, so
        /// the cap is about pacing PCIe, not wire limits.
        static constexpr lux::render::RegionUploadBudget kUploadBudget{1u << 20, 64};

        // ── live steady state ────────────────────────────────────────────────
        void driveLive(RenderableBridgeContext& ctx, lux::render::Canvas2DProxy& canvas,
                       Live& L, const PixelField2DComponent& pc, const float m[6])
        {
            if (std::memcmp(m, L.m, sizeof(float) * 6) != 0)
            {
                std::memcpy(L.m, m, sizeof(float) * 6);
                for (std::uint32_t cy = 0; cy < L.chunks_y; ++cy)
                    for (std::uint32_t cx = 0; cx < L.chunks_x; ++cx)
                    {
                        float cm[6];
                        chunkAffine(L.field, pc, m, cx, cy, cm);
                        canvas.updatePixelFieldTransform(
                            ctx.scene(), L.slots[cy * L.chunks_x + cx].instance, cm);
                    }
            }
            if (pc.priority != L.priority || pc.visible != L.visible)
            {
                for (auto& s : L.slots)
                    canvas.updatePixelFieldKey(ctx.scene(), s.instance, pc.priority, pc.visible);
                L.priority = pc.priority;
                L.visible  = pc.visible;
            }

            // CONTENT: one budgeted export per chunk per frame; the reply's
            // status routes straight back into that chunk's planner.
            for (std::uint32_t cy = 0; cy < L.chunks_y; ++cy)
                for (std::uint32_t cx = 0; cx < L.chunks_x; ++cx)
                {
                    auto ex = runtime_->exportDirty(L.field, cx, cy, kUploadBudget);
                    if (ex.empty()) continue;
                    lux::render::OwnedTextureUploadBatch batch{};
                    batch.dst              = L.slots[cy * L.chunks_x + cx].texture;
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

        // ── first-sight state machine ────────────────────────────────────────
        void startBuilding(RenderableBridgeContext& ctx, lux::meta::entity_id e,
                           const PixelField2DComponent& pc)
        {
            auto b = std::make_shared<Building>();
            b->field    = pc.field;
            b->chunks_x = runtime_->chunksX(pc.field);
            b->chunks_y = runtime_->chunksY(pc.field);
            const std::uint32_t n = b->chunks_x * b->chunks_y;
            if (n == 0) return;
            b->textures.resize(n);
            b->instances.resize(n);
            b->textures_pending = n;
            b->tex_reqs.reserve(n);

            for (std::uint32_t cy = 0; cy < b->chunks_y; ++cy)
                for (std::uint32_t cx = 0; cx < b->chunks_x; ++cx)
                {
                    const auto cc = runtime_->chunkCells(pc.field, cx, cy);
                    lux::render::PersistentTexture2DDesc td{};
                    td.width  = static_cast<std::uint32_t>(cc.x());
                    td.height = static_cast<std::uint32_t>(cc.y());
                    td.format = lux::render::EPixelFormat::R16_UNORM;
                    // First upload = the whole chunk (the export path carries it).
                    if (auto* planner = runtime_->uploadPlanner(pc.field, cx, cy))
                        planner->markDirty(0, 0, td.width, td.height);
                    auto req = ctx.session().createPersistentTexture2D(td);
                    const std::uint32_t idx = cy * b->chunks_x + cx;
                    std::weak_ptr<Building> wb = b;
                    req.then([this, wb, idx](const lux::render::Texture2DCreatedReply& r)
                    {
                        auto sb = wb.lock();
                        if (!sb) return;
                        --sb->textures_pending;
                        if (r.status != 0 || r.handle.is_null())
                        {
                            sb->failed = true;
                            sb->failed_permanent = true;
                            return;
                        }
                        if (stopping_) { orphan_textures_.push_back(r.handle); return; }
                        sb->textures[idx] = r.handle;
                    });
                    b->tex_reqs.push_back(std::move(req));
                }
            building_.emplace(e, std::move(b));
        }

        void advanceBuilding(RenderableBridgeContext& ctx, lux::render::Canvas2DProxy& canvas,
                             lux::meta::entity_id e, Building& b,
                             const PixelField2DComponent& pc, const float m[6])
        {
            if (b.failed)
            {
                if (b.textures_pending > 0 || b.instances_pending > 0)
                    return;   // let the stragglers land first (they self-orphan)
                abandonBuilding(ctx, b);
                if (!stopping_)
                    failed_[e] = FailRecord{b.failed_permanent, kRetryDrives};
                building_.erase(e);
                return;
            }
            if (b.textures_pending > 0)
                return;
            if (!b.instances_issued)
            {
                if (palette_bindless_ == lux::render::kNoTexture)
                    return;   // palette still in flight — next frame
                b.instances_issued  = true;
                b.instances_pending = b.chunks_x * b.chunks_y;
                b.add_reqs.reserve(b.instances_pending);
                for (std::uint32_t cy = 0; cy < b.chunks_y; ++cy)
                    for (std::uint32_t cx = 0; cx < b.chunks_x; ++cx)
                    {
                        const auto cc = runtime_->chunkCells(pc.field, cx, cy);
                        lux::render::PixelField2DInstanceData data{};
                        chunkAffine(pc.field, pc, m, cx, cy, data.m);
                        const std::uint32_t idx = cy * b.chunks_x + cx;
                        data.field_texture   = b.textures[idx].index;
                        data.palette_texture = palette_bindless_;
                        data.cells_w         = static_cast<std::uint32_t>(cc.x());
                        data.cells_h         = static_cast<std::uint32_t>(cc.y());

                        auto req = canvas.addPixelField(ctx.scene(), data, pc.priority, pc.visible);
                        auto bit = building_.find(e);
                        std::weak_ptr<Building> wb = (bit != building_.end())
                            ? std::weak_ptr<Building>(bit->second) : std::weak_ptr<Building>{};
                        req.then([this, wb, idx](const lux::render::PixelFieldSlotReply& r)
                        {
                            auto sb = wb.lock();
                            if (!sb) { if (r.handle.valid()) orphan_instances_.push_back(r.handle); return; }
                            --sb->instances_pending;
                            if (r.status != lux::render::ECanvas2DCreateStatus::Ok || r.handle.is_null())
                            {
                                if (r.handle.valid()) orphan_instances_.push_back(r.handle);
                                sb->failed = true;
                                sb->failed_permanent =
                                    r.status != lux::render::ECanvas2DCreateStatus::CapacityExhausted;
                                return;
                            }
                            if (stopping_) { orphan_instances_.push_back(r.handle); return; }
                            sb->instances[idx] = r.handle;
                        });
                        b.add_reqs.push_back(std::move(req));
                    }
                return;
            }
            if (b.instances_pending > 0)
                return;

            // Everything landed → promote to Live.
            Live L{};
            L.field    = b.field;
            L.chunks_x = b.chunks_x;
            L.chunks_y = b.chunks_y;
            L.slots.resize(b.textures.size());
            for (std::size_t i = 0; i < b.textures.size(); ++i)
            {
                L.slots[i].texture  = b.textures[i];
                L.slots[i].instance = b.instances[i];
            }
            std::memcpy(L.m, m, sizeof(float) * 6);
            L.priority = pc.priority;
            L.visible  = pc.visible;
            live_.emplace(e, std::move(L));
            building_.erase(e);
        }

        // ── teardown helpers ─────────────────────────────────────────────────
        void destroyLive(RenderableBridgeContext& ctx, lux::render::Canvas2DProxy& canvas, Live& L)
        {
            for (auto& s : L.slots)
            {
                canvas.removePixelField(ctx.scene(), s.instance);
                ctx.session().destroyTexture(s.texture);
            }
            L.slots.clear();
        }

        /// Destroy every ALREADY-COLLECTED part of a build; pending replies keep
        /// routing through the shared_ptr continuations (self-orphaning).
        void abandonBuilding(RenderableBridgeContext& ctx, Building& b)
        {
            auto canvas = ctx.canvas2d();
            for (auto& t : b.textures)
                if (!t.is_null()) { ctx.session().destroyTexture(t); t = {}; }
            for (auto& i : b.instances)
                if (i.valid()) { canvas.removePixelField(ctx.scene(), i); i = {}; }
        }

        /// The affine of chunk (cx,cy): its clipped extent placed at the field's
        /// min corner + the chunk offset (unit-quad centring folded in).
        void chunkAffine(PixelFieldHandle field, const PixelField2DComponent& pc,
                         const float field_m[6], std::uint32_t cx, std::uint32_t cy,
                         float out[6]) const
        {
            const auto cc = runtime_->chunkCells(field, cx, cy);
            const float sx = static_cast<float>(cc.x()) * pc.cell_size;
            const float sy = static_cast<float>(cc.y()) * pc.cell_size;
            out[0] = sx;  out[1] = 0.f;
            out[2] = 0.f; out[3] = sy;
            out[4] = field_m[4]
                   + static_cast<float>(cx * PixelFieldRuntime::kChunkSizeCells) * pc.cell_size
                   + sx * 0.5f;
            out[5] = field_m[5]
                   + static_cast<float>(cy * PixelFieldRuntime::kChunkSizeCells) * pc.cell_size
                   + sy * 0.5f;
        }

        /// The scene-shared palette: create once, fill from the material table,
        /// re-upload only when the table GROWS (append-only registry).
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
                    if (r.status != 0 || r.handle.is_null()) return;   // retried next frame
                    if (stopping_) { orphan_textures_.push_back(r.handle); return; }
                    palette_texture_ = r.handle;
                });
                return;
            }
            if (palette_texture_.is_null())
                return;

            const std::uint32_t count = runtime_->materials().count();
            if (count == palette_uploaded_count_)
                return;   // steady state: zero work

            // (Re)upload the whole 256×1 strip — 1 KiB, and only when the table grew.
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
            (void)ctx.session().updateTextureRegions(batch);   // fire-and-forget (1 KiB, idempotent)
            palette_uploaded_count_ = count;
            palette_bindless_       = palette_texture_.index;
        }

        static constexpr std::uint32_t kPaletteWidth = 256;

        PixelFieldRuntime* runtime_{nullptr};

        std::unordered_map<lux::meta::entity_id, Live>                      live_;
        std::unordered_map<lux::meta::entity_id, std::shared_ptr<Building>> building_;
        std::unordered_map<lux::meta::entity_id, FailRecord>                failed_;

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

} // namespace lux::gameplay::d2
