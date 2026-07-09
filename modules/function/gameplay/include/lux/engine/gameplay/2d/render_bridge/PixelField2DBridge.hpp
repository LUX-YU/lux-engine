#pragma once
// ============================================================================
//  PixelField2DBridge.hpp — ECS PixelField2DComponent → GPU-resident canvas
//  field instance + persistent id-mirror texture (lux::gameplay::d2, F2-08).
//
//  RETAINED bridge (the Sprite2DBridge/InstanceBridge shape) with a TWO-STAGE
//  first sight: ① create the field's persistent R16_UNORM id-mirror texture
//  (reply → bindless index), ② create the canvas PixelField instance carrying
//  that index (reply → instance handle, G-05 validated). One scene-shared
//  256×1 RGBA8 PALETTE texture (premultiplied material colours; entry 0 =
//  transparent) is created once and re-uploaded only when the material table
//  GROWS — material colour changes touch ONLY the palette (F2-09 acceptance).
//
//  Steady state per frame:
//    - CONTENT: runtime.exportDirty() → OwnedTextureUploadBatch (pixels ride
//      the export's own shared_ptr — attachment channel, not the command ring)
//      → session.updateTextureRegions → the reply's status routes back through
//      runtime.confirmExport (Ok advances uploadedRevision, anything else
//      re-dirties — defer-never-lose). A settled field exports NOTHING.
//    - PLACEMENT: the world affine (WorldTransform2D translation = min corner,
//      scale = cells × cell_size, unit quad centring folded in) is re-baked and
//      VALUE-compared; only a change emits UpdatePixelField2DTransform.
//    - priority/visible value-diff → UpdatePixelField2DKey.
//
//  The bridge never reads CA internals — only the F2-07 export ticket and the
//  component. Reap removes the canvas instance AND destroys the field texture;
//  teardown drains pending creates into orphans (two-phase contract).
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
#include <unordered_map>
#include <utility>
#include <vector>

namespace lux::gameplay::d2
{
    class PixelField2DBridge final : public lux::gameplay::IRenderableBridge
    {
        struct Live
        {
            lux::render::PixelFieldInstanceHandle instance{};
            lux::render::RTextureHandle           texture{};     ///< the field's id mirror
            PixelFieldHandle                      field{};       ///< runtime handle (export source)
            float                                 m[6]{};        ///< last sent affine
            float                                 priority{0.f};
            bool                                  visible{true};
        };

    public:
        explicit PixelField2DBridge(PixelFieldRuntime* runtime) noexcept : runtime_(runtime) {}

        ~PixelField2DBridge() override
        {
            for (auto& [e, req] : tex_pending_) req.cancel();
            for (auto& [e, req] : add_pending_) req.cancel();
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

                    // Bake the placement affine: min corner = world translation
                    // (F2-02 contract), extent = cells × cell_size; the unit quad
                    // is centred, so the centre offset folds into the translation.
                    const float sx = static_cast<float>(d.cells_w) * pc.cell_size;
                    const float sy = static_cast<float>(d.cells_h) * pc.cell_size;
                    float m[6];
                    m[0] = sx;  m[1] = 0.f;
                    m[2] = 0.f; m[3] = sy;
                    m[4] = wt.world(0, 3) + sx * 0.5f;
                    m[5] = wt.world(1, 3) + sy * 0.5f;

                    if (auto it = live_.find(e); it != live_.end())
                    {
                        Live& L = it->second;
                        if (std::memcmp(m, L.m, sizeof(m)) != 0)
                        {
                            canvas.updatePixelFieldTransform(ctx.scene(), L.instance, m);
                            std::memcpy(L.m, m, sizeof(m));
                        }
                        if (pc.priority != L.priority || pc.visible != L.visible)
                        {
                            canvas.updatePixelFieldKey(ctx.scene(), L.instance, pc.priority, pc.visible);
                            L.priority = pc.priority;
                            L.visible  = pc.visible;
                        }

                        // CONTENT: one budgeted export per frame; the reply's status
                        // routes straight back into the planner (defer-never-lose).
                        auto ex = runtime_->exportDirty(L.field, kUploadBudget);
                        if (!ex.empty())
                        {
                            lux::render::OwnedTextureUploadBatch batch{};
                            batch.dst              = L.texture;
                            batch.content_revision = ex.content_revision;
                            batch.regions          = std::move(ex.regions);
                            batch.pixels           = ex.pixels;
                            batch.pixel_bytes      = ex.pixel_bytes;
                            auto req = ctx.session().updateTextureRegions(batch);
                            PixelFieldRuntime* rt = runtime_;
                            const PixelFieldHandle fh = L.field;
                            const std::uint64_t rev = ex.content_revision;
                            req.then([rt, fh, rev](const lux::render::TextureRegionsAppliedReply& r)
                            {
                                rt->confirmExport(fh, rev,
                                    static_cast<lux::render::ERegionUploadStatus>(r.status));
                            });
                        }
                        return;
                    }

                    // ── two-stage first sight ──
                    if (add_pending_.count(e) || tex_pending_.count(e) || failed_.count(e))
                    {
                        if (auto fit = failed_.find(e); fit != failed_.end())
                        {
                            if (fit->second.permanent) return;
                            if (--fit->second.retry_in > 0) return;
                            failed_.erase(fit);
                        }
                        else return;
                    }

                    if (auto tit = tex_ready_.find(e); tit != tex_ready_.end())
                    {
                        // ② the mirror exists → create the canvas instance.
                        if (palette_bindless_ == lux::render::kNoTexture)
                            return;   // palette still in flight — next frame
                        lux::render::PixelField2DInstanceData data{};
                        std::memcpy(data.m, m, sizeof(m));
                        data.field_texture   = tit->second.index;   // RTextureHandle::index IS the bindless index
                        data.palette_texture = palette_bindless_;
                        data.cells_w         = d.cells_w;
                        data.cells_h         = d.cells_h;

                        Live snap{};
                        snap.texture = tit->second;
                        snap.field   = pc.field;
                        std::memcpy(snap.m, m, sizeof(m));
                        snap.priority = pc.priority;
                        snap.visible  = pc.visible;

                        auto req = canvas.addPixelField(ctx.scene(), data, pc.priority, pc.visible);
                        req.then([this, e, snap](const lux::render::PixelFieldSlotReply& r)
                        {
                            add_pending_.erase(e);
                            if (r.status != lux::render::ECanvas2DCreateStatus::Ok || r.handle.is_null())
                            {
                                if (r.handle.valid())
                                    orphan_instances_.push_back(r.handle);
                                if (!stopping_)
                                    failed_[e] = FailRecord{
                                        r.status != lux::render::ECanvas2DCreateStatus::CapacityExhausted,
                                        kRetryDrives};
                                return;
                            }
                            if (stopping_) { orphan_instances_.push_back(r.handle); return; }
                            Live L = snap;
                            L.instance = r.handle;
                            live_.emplace(e, L);
                        });
                        add_pending_.emplace(e, std::move(req));
                        tex_ready_.erase(tit);
                        return;
                    }

                    // ① create the persistent id-mirror texture. Full initial
                    // content lands via the export path (create marks the WHOLE
                    // surface dirty below, so the first exports carry everything).
                    lux::render::PersistentTexture2DDesc td{};
                    td.width  = d.cells_w;
                    td.height = d.cells_h;
                    td.format = lux::render::EPixelFormat::R16_UNORM;
                    auto req = ctx.session().createPersistentTexture2D(td);
                    if (auto* planner = runtime_->uploadPlanner(pc.field))
                        planner->markDirty(0, 0, d.cells_w, d.cells_h);   // first upload = full surface
                    req.then([this, e](const lux::render::Texture2DCreatedReply& r)
                    {
                        tex_pending_.erase(e);
                        if (r.status != 0 || r.handle.is_null())
                        {
                            if (!stopping_)
                                failed_[e] = FailRecord{/*permanent=*/true, 0};
                            return;
                        }
                        if (stopping_) { orphan_textures_.push_back(r.handle); return; }
                        tex_ready_[e] = r.handle;
                    });
                    tex_pending_.emplace(e, std::move(req));
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
                    canvas.removePixelField(ctx.scene(), it->second.instance);
                    ctx.session().destroyTexture(it->second.texture);
                    it = live_.erase(it);
                }
                else ++it;
            }
            std::erase_if(tex_ready_, [&](auto& kv) {
                if (registry.valid(kv.first) &&
                    registry.all_of<PixelField2DComponent, WorldTransform2DComponent>(kv.first))
                    return false;
                ctx.session().destroyTexture(kv.second);   // mirror created, owner gone
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
            {
                canvas.removePixelField(ctx.scene(), L.instance);
                ctx.session().destroyTexture(L.texture);
            }
            live_.clear();
            for (auto& [e, t] : tex_ready_)
                ctx.session().destroyTexture(t);
            tex_ready_.clear();
            if (!palette_texture_.is_null())
            {
                ctx.session().destroyTexture(palette_texture_);
                palette_texture_  = {};
                palette_bindless_ = lux::render::kNoTexture;
            }
            failed_.clear();
            stopping_ = true;   // keep pendings; late replies route into the orphan lists
        }

        [[nodiscard]] bool hasPendingShutdownWork() const override
        {
            return !tex_pending_.empty() || !add_pending_.empty();
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

    private:
        struct FailRecord
        {
            bool permanent{false};
            int  retry_in{0};
        };
        static constexpr int kRetryDrives = 120;
        /// Per-frame content budget. Generous on purpose: pixels ride the
        /// ATTACHMENT channel (an owned shared_ptr block), not the 64 KiB
        /// command ring, so the cap is about pacing PCIe, not wire limits.
        static constexpr lux::render::RegionUploadBudget kUploadBudget{1u << 20, 64};

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

        std::unordered_map<lux::meta::entity_id, Live> live_;
        std::unordered_map<lux::meta::entity_id,
                           lux::render::RenderRequest<lux::render::Texture2DCreatedReply>> tex_pending_;
        std::unordered_map<lux::meta::entity_id, lux::render::RTextureHandle> tex_ready_;
        std::unordered_map<lux::meta::entity_id,
                           lux::render::RenderRequest<lux::render::PixelFieldSlotReply>> add_pending_;
        std::unordered_map<lux::meta::entity_id, FailRecord> failed_;

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
