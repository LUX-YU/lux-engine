#pragma once
// ============================================================================
//  Tilemap2DBridge.hpp — ECS TilemapComponent → GPU-resident canvas Tile
//  instance + persistent tile-index texture (lux::gameplay::d2, A2-02).
//
//  RETAINED bridge, the PixelField2DBridge shape with a THREE-way first sight:
//  ① resolve the tileset atlas texture (ctx.ensureTexture — the same bindless
//  path sprites use; null until the asset upload settles → retry next frame),
//  ② create the map's persistent R16_UNORM tile-INDEX texture, ③ create the
//  canvas Tile instance carrying both bindless indices (G-05 validated reply).
//
//  Steady state per frame:
//    - CONTENT: component.revision changed → upload the component's dirty rect
//      (u16 ordinals → R16_UNORM texels, tight rows) via updateTextureRegions;
//      the ack clears the rect IFF no further edit bumped the revision since
//      (defer-never-lose: a failed upload leaves the rect dirty → retried).
//      A settled map sends NOTHING.
//    - PLACEMENT: world affine value-compared → UpdateTile2DTransform.
//    - priority/visible value-diff → UpdateTile2DKey.
//
//  MVP constraint (documented): the tileset bindless index is baked into the
//  instance at creation — the tile kind has no visual-update op. The bridge
//  holds the tileset via ensureTexture's cache for the map's lifetime, so the
//  index stays stable; swapping the tileset asset at runtime = remove+recreate
//  (a content-tool flow, not a per-frame path).
// ============================================================================

#include <lux/engine/gameplay/render_bridge/IRenderableBridge.hpp>
#include <lux/engine/gameplay/render_bridge/RenderableBridgeContext.hpp>
#include <lux/engine/gameplay/2d/world/components/TilemapComponent.hpp>
#include <lux/engine/gameplay/2d/world/components/WorldTransform2DComponent.hpp>
#include <lux/engine/render/renderer/features/canvas2d/Canvas2DOperation.hpp>
#include <lux/engine/render/comm/client/RenderSession.hpp>
#include <lux/engine/render/comm/client/RenderRequest.hpp>
#include <lux/engine/meta/LuxObject.hpp>

#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lux::gameplay::d2
{
    class Tilemap2DBridge final : public lux::gameplay::IRenderableBridge
    {
        struct Live
        {
            lux::render::Tile2DInstanceHandle instance{};
            lux::render::RTextureHandle       index_texture{};
            std::uint64_t                     uploaded_revision{0};
            std::uint64_t                     inflight_revision{0};   ///< 0 = no upload in flight
            float                             m[6]{};
            float                             priority{0.f};
            bool                              visible{true};
        };

    public:
        Tilemap2DBridge() noexcept = default;

        ~Tilemap2DBridge() override
        {
            for (auto& [e, req] : tex_pending_) req.cancel();
            for (auto& [e, req] : add_pending_) req.cancel();
        }

        void drive(lux::meta::EntityRegistry& registry, RenderableBridgeContext& ctx) override
        {
            auto canvas = ctx.canvas2d();
            if (!canvas.valid()) return;

            applyPendingClears(registry);   // acked dirty-rect clears from last frame

            registry.view<TilemapComponent, WorldTransform2DComponent>().each(
                [&](lux::meta::entity_id e, TilemapComponent& tc,
                    const WorldTransform2DComponent& wt)
                {
                    if (tc.tiles_w == 0 || tc.tiles_h == 0 ||
                        tc.tiles.size() != static_cast<std::size_t>(tc.tiles_w) * tc.tiles_h)
                        return;   // unconfigured / inconsistent grid — nothing to mirror

                    // Placement: min corner = world translation, extent = tiles ×
                    // tile_size, unit-quad centring folded in (the field recipe).
                    const float sx = static_cast<float>(tc.tiles_w) * tc.tile_size;
                    const float sy = static_cast<float>(tc.tiles_h) * tc.tile_size;
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
                            canvas.updateTilemapTransform(ctx.scene(), L.instance, m);
                            std::memcpy(L.m, m, sizeof(m));
                        }
                        if (tc.priority != L.priority || tc.visible != L.visible)
                        {
                            canvas.updateTilemapKey(ctx.scene(), L.instance, tc.priority, tc.visible);
                            L.priority = tc.priority;
                            L.visible  = tc.visible;
                        }
                        // CONTENT: at most one region upload in flight per map.
                        if (tc.revision != L.uploaded_revision && L.inflight_revision == 0 &&
                            tc.hasDirty())
                            submitDirtyRect(ctx, e, L, tc);
                        return;
                    }

                    // ── three-way first sight ──
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

                    // ① tileset atlas — the sprite bindless path; null until settled.
                    const auto tileset = ctx.ensureTexture(tc.tileset_texture);
                    if (tileset.is_null())
                        return;   // asset still uploading — next frame

                    if (auto tit = tex_ready_.find(e); tit != tex_ready_.end())
                    {
                        // ③ index texture exists → create the canvas instance.
                        lux::render::Tile2DInstanceData data{};
                        std::memcpy(data.m, m, sizeof(m));
                        data.tileset_texture = tileset.index;
                        data.index_texture   = tit->second.index;
                        data.tiles_w         = tc.tiles_w;
                        data.tiles_h         = tc.tiles_h;
                        data.tileset_grid    = lux::render::packTilesetGrid(tc.tileset_cols, tc.tileset_rows);
                        data.tint            = tc.tint;

                        Live snap{};
                        snap.index_texture = tit->second;
                        std::memcpy(snap.m, m, sizeof(m));
                        snap.priority = tc.priority;
                        snap.visible  = tc.visible;

                        auto req = canvas.addTilemap(ctx.scene(), data, tc.priority, tc.visible);
                        req.then([this, e, snap](const lux::render::Tile2DSlotReply& r)
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

                    // ② create the persistent tile-index texture. The full grid
                    // lands via the dirty-rect path: mark EVERYTHING dirty so the
                    // first upload carries the whole map.
                    lux::render::PersistentTexture2DDesc td{};
                    td.width  = tc.tiles_w;
                    td.height = tc.tiles_h;
                    td.format = lux::render::EPixelFormat::R16_UNORM;
                    auto req = ctx.session().createPersistentTexture2D(td);
                    ++tc.revision;   // force a content pass once live
                    tc.dirty_min_x = 0; tc.dirty_min_y = 0;
                    tc.dirty_max_x = static_cast<std::int32_t>(tc.tiles_w) - 1;
                    tc.dirty_max_y = static_cast<std::int32_t>(tc.tiles_h) - 1;
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
                    !registry.all_of<TilemapComponent, WorldTransform2DComponent>(e))
                {
                    canvas.removeTilemap(ctx.scene(), it->second.instance);
                    ctx.session().destroyTexture(it->second.index_texture);
                    it = live_.erase(it);
                }
                else ++it;
            }
            std::erase_if(tex_ready_, [&](auto& kv) {
                if (registry.valid(kv.first) &&
                    registry.all_of<TilemapComponent, WorldTransform2DComponent>(kv.first))
                    return false;
                ctx.session().destroyTexture(kv.second);
                return true;
            });
            std::erase_if(failed_, [&](const auto& kv) {
                return !registry.valid(kv.first) ||
                       !registry.all_of<TilemapComponent, WorldTransform2DComponent>(kv.first);
            });
        }

        void beginShutdown(RenderableBridgeContext& ctx) override
        {
            auto canvas = ctx.canvas2d();
            for (auto& [e, L] : live_)
            {
                canvas.removeTilemap(ctx.scene(), L.instance);
                ctx.session().destroyTexture(L.index_texture);
            }
            live_.clear();
            for (auto& [e, t] : tex_ready_)
                ctx.session().destroyTexture(t);
            tex_ready_.clear();
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
                canvas.removeTilemap(ctx.scene(), h);
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

        /// Pack the component's dirty rect (u16 ordinals are ALREADY the
        /// R16_UNORM texel encoding — ordinal/65535) and send ONE region update.
        /// Ack semantics: Ok at revision R clears the rect iff the component is
        /// still at R (no later edit); any failure leaves the rect dirty.
        void submitDirtyRect(RenderableBridgeContext& ctx, lux::meta::entity_id e,
                             Live& L, TilemapComponent& tc)
        {
            const std::uint32_t x0 = static_cast<std::uint32_t>(tc.dirty_min_x);
            const std::uint32_t y0 = static_cast<std::uint32_t>(tc.dirty_min_y);
            const std::uint32_t w  = static_cast<std::uint32_t>(tc.dirty_max_x - tc.dirty_min_x + 1);
            const std::uint32_t h  = static_cast<std::uint32_t>(tc.dirty_max_y - tc.dirty_min_y + 1);

            const std::uint64_t bytes = static_cast<std::uint64_t>(w) * h * 2u;
            auto pixels = std::shared_ptr<std::byte[]>(new std::byte[bytes]);
            auto* dst   = reinterpret_cast<std::uint16_t*>(pixels.get());
            for (std::uint32_t row = 0; row < h; ++row)
                std::memcpy(dst + static_cast<std::size_t>(row) * w,
                            tc.tiles.data() + static_cast<std::size_t>(y0 + row) * tc.tiles_w + x0,
                            static_cast<std::size_t>(w) * 2u);

            lux::render::OwnedTextureUploadBatch batch{};
            batch.dst              = L.index_texture;
            batch.content_revision = tc.revision;
            lux::render::TextureRegionDesc r{};
            r.x = x0; r.y = y0; r.width = w; r.height = h;
            batch.regions.push_back(r);
            batch.pixels      = pixels;
            batch.pixel_bytes = bytes;

            L.inflight_revision = tc.revision;
            auto req = ctx.session().updateTextureRegions(batch);
            const std::uint64_t rev = tc.revision;
            req.then([this, e, rev](const lux::render::TextureRegionsAppliedReply& reply)
            {
                auto it = live_.find(e);
                if (it == live_.end()) return;   // reaped while in flight — texture already destroyed
                Live& live = it->second;
                live.inflight_revision = 0;
                if (reply.status != 0) return;   // rect stays dirty → resent next drive
                live.uploaded_revision = rev;
                // Clearing the rect needs the component — deferred to the next
                // drive() (clear_pending_ marks it), keeping this continuation
                // registry-free (the .then contract).
                clear_pending_.push_back({e, rev});
            });
        }

        std::unordered_map<lux::meta::entity_id, Live> live_;
        std::unordered_map<lux::meta::entity_id,
                           lux::render::RenderRequest<lux::render::Texture2DCreatedReply>> tex_pending_;
        std::unordered_map<lux::meta::entity_id, lux::render::RTextureHandle> tex_ready_;
        std::unordered_map<lux::meta::entity_id,
                           lux::render::RenderRequest<lux::render::Tile2DSlotReply>> add_pending_;
        std::unordered_map<lux::meta::entity_id, FailRecord> failed_;

        struct PendingClear { lux::meta::entity_id e; std::uint64_t revision; };
        std::vector<PendingClear> clear_pending_;

        bool stopping_{false};
        std::vector<lux::render::Tile2DInstanceHandle> orphan_instances_;
        std::vector<lux::render::RTextureHandle>       orphan_textures_;

        /// Called at the top of drive(): apply acked clears against the LIVE
        /// component state (only when no later edit bumped the revision).
        void applyPendingClears(lux::meta::EntityRegistry& registry)
        {
            for (const auto& pc : clear_pending_)
            {
                if (!registry.valid(pc.e) || !registry.all_of<TilemapComponent>(pc.e))
                    continue;
                auto& tc = registry.get<TilemapComponent>(pc.e);
                if (tc.revision == pc.revision)
                    tc.clearDirty();
            }
            clear_pending_.clear();
        }
    };

} // namespace lux::gameplay::d2
