#pragma once
// ============================================================================
//  Sprite2DBridge.hpp — ECS SpriteComponent → GPU-resident Canvas2D instance
//  (lux::gameplay::d2, v2 — .internal/2d-gpu-driven-rewrite.md §3.5).
//
//  RETAINED bridge (the InstanceBridge shape): each sprite entity owns ONE
//  GPU-resident instance in the scene's Canvas2D arena, created on first sight
//  (AddSprite2D, reply-validated per G-05), updated by DELTAS and removed on
//  reap. The wire carries change, never content:
//    - transform: the world affine is re-baked each frame (world × size ×
//      pivot — 2D, ~12 FLOPs) and VALUE-compared against the last-sent bake;
//      only a difference emits a batch entry. Value-based on purpose (the
//      no-trust axiom): a direct field write to Transform2D / size / pivot
//      with no dirty flag still registers.
//    - visual (uv/tint/texture) and key (priority/visible): value-compared
//      against the last-sent copy; changes emit their low-frequency op.
//  A fully static sprite set therefore sends NOTHING — no heartbeat, no
//  producer staging; those v1 concepts are gone.
//
//  No per-frame camera gate here: instances persist server-side, and DRAWING
//  is gated by the retained scene bit (SetCanvas2DEnabled), which the
//  Camera2DUploadBridge flips edge-triggered on the publishable-camera state.
//
//  Teardown follows the two-phase drain contract: beginShutdown removes every
//  known live instance and routes late create replies into an orphan list that
//  flushShutdownCleanup destroys (never cancel-and-leak — G-05).
// ============================================================================

#include <lux/engine/gameplay/render_bridge/IRenderableBridge.hpp>
#include <lux/engine/gameplay/render_bridge/RenderableBridgeContext.hpp>       // ctx.canvas2d() / scene()
#include <lux/engine/gameplay/2d/world/components/SpriteComponent.hpp>
#include <lux/engine/gameplay/2d/world/components/WorldTransform2DComponent.hpp>
#include <lux/engine/gameplay/2d/world/components/Order2DComponents.hpp>       // A2-03: Y-sort / parallax
#include <lux/engine/gameplay/2d/world/components/Camera2DComponent.hpp>       // ActiveCamera2DTag (parallax origin)
#include <lux/engine/render/renderer/features/canvas2d/Canvas2DOperation.hpp>  // Sprite2DInstanceData / Sprite2DHandle
#include <lux/engine/render/comm/client/RenderRequest.hpp>                      // in-flight create handle
#include <lux/engine/meta/LuxObject.hpp>   // EntityRegistry / entity_id

#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lux::gameplay::d2
{
    class Sprite2DBridge final : public lux::gameplay::IRenderableBridge
    {
        /// One live GPU instance + the LAST-SENT state every diff compares against.
        struct Live
        {
            lux::render::Sprite2DHandle handle{};
            float                       m[6]{};      ///< last sent baked affine
            float                       uv[4]{};
            std::uint32_t               tint{0};
            std::uint32_t               tex{lux::render::kNoTexture};
            float                       priority{0.f};
            bool                        visible{true};
            std::uint8_t                group{0};    ///< A2-04 offscreen group
        };

        /// G-05 failure record: a failed create must not re-issue every frame.
        /// InvalidConfiguration / generic Unknown are permanent (the scene has no
        /// Canvas2D — futile until the scene is rebuilt); CapacityExhausted is
        /// transient and retried after a bounded backoff.
        struct FailRecord
        {
            bool permanent{false};
            int  retry_in{0};
        };
        static constexpr int kTransientRetryDrives = 120;   // ~2s @ 60fps

    public:
        ~Sprite2DBridge() override
        {
            for (auto& [e, req] : pending_) req.cancel();   // never a dangling `this`
        }

        void drive(lux::meta::EntityRegistry& registry, RenderableBridgeContext& ctx) override
        {
            auto canvas = ctx.canvas2d();
            if (!canvas.valid()) return;   // scene without Canvas2D → zero cost (R2-04 contract)

            batch_.clear();
            tex_memo_.clear();

            // Parallax origin: the active camera's world centre (A2-03). One
            // lookup per drive; scenes without a camera get a zero origin.
            Eigen::Vector2f cam_center{0.f, 0.f};
            for (auto ce : registry.view<ActiveCamera2DTag, WorldTransform2DComponent>())
            {
                const auto& cw = registry.get<WorldTransform2DComponent>(ce);
                cam_center = {cw.world(0, 3), cw.world(1, 3)};
                break;
            }

            registry.view<SpriteComponent, WorldTransform2DComponent>().each(
                [&](lux::meta::entity_id e, const SpriteComponent& sp, const WorldTransform2DComponent& wt)
                {
                    // Bake the 2D affine: world × Scale(size), pivot shift folded into
                    // the translation so the normalised pivot sits at the entity origin
                    // (pivot 0.5,0.5 = centred → zero offset). Column-major world 4x4 →
                    // 6-float 2D affine (the render side expands a unit ±0.5 quad).
                    float m[6];
                    m[0] = wt.world(0, 0) * sp.size.x();
                    m[1] = wt.world(1, 0) * sp.size.x();
                    m[2] = wt.world(0, 1) * sp.size.y();
                    m[3] = wt.world(1, 1) * sp.size.y();
                    const float px = 0.5f - sp.pivot.x(), py = 0.5f - sp.pivot.y();
                    m[4] = wt.world(0, 3) + m[0] * px + m[2] * py;
                    m[5] = wt.world(1, 3) + m[1] * px + m[3] * py;

                    // A2-03 opt-in modifiers, applied at BAKE time (never written
                    // back to the authored components):
                    //  - parallax shifts the sent translation by cam×(1−factor);
                    //    the value diff below gives the right wire economy for free;
                    //  - y-sort derives the EFFECTIVE priority from world Y.
                    if (const auto* pl = registry.try_get<Parallax2DComponent>(e))
                    {
                        m[4] += cam_center.x() * (1.f - pl->factor.x());
                        m[5] += cam_center.y() * (1.f - pl->factor.y());
                    }
                    float prio = sp.priority;
                    if (const auto* ys = registry.try_get<YSort2DComponent>(e))
                        prio = ys->effectivePriority(wt.world(1, 3));

                    // Resolve the texture every frame (advances the async upload state
                    // machine; memoised per distinct id per frame). kNoTexture while an
                    // upload is still in flight → tint-only until it settles, at which
                    // point the value diff below promotes it — no special path.
                    const std::uint32_t tex = sp.texture.is_nil()
                        ? lux::render::kNoTexture
                        : resolveBindless(ctx, sp.texture);

                    if (auto it = live_.find(e); it != live_.end())
                    {
                        Live& L = it->second;

                        if (std::memcmp(m, L.m, sizeof(m)) != 0)
                        {
                            lux::render::Sprite2DTransformEntry ent{};
                            ent.scene  = ctx.scene();
                            ent.handle = L.handle;
                            std::memcpy(ent.m, m, sizeof(m));
                            batch_.push_back(ent);
                            std::memcpy(L.m, m, sizeof(m));
                        }
                        const float uv[4] = {sp.uv_rect.x(), sp.uv_rect.y(),
                                             sp.uv_rect.z(), sp.uv_rect.w()};
                        if (std::memcmp(uv, L.uv, sizeof(uv)) != 0 ||
                            sp.tint != L.tint || tex != L.tex)
                        {
                            canvas.updateVisual(ctx.scene(), L.handle, uv, sp.tint, tex);
                            std::memcpy(L.uv, uv, sizeof(uv));
                            L.tint = sp.tint;
                            L.tex  = tex;
                        }
                        if (prio != L.priority || sp.visible != L.visible || sp.group != L.group)
                        {
                            canvas.updateKey(ctx.scene(), L.handle, prio, sp.visible, sp.group);
                            L.priority = prio;
                            L.visible  = sp.visible;
                            L.group    = sp.group;
                        }
                        return;
                    }
                    if (pending_.count(e)) return;   // create reply still in flight

                    if (auto fit = failed_.find(e); fit != failed_.end())
                    {
                        if (fit->second.permanent) return;
                        if (--fit->second.retry_in > 0) return;
                        failed_.erase(fit);          // backoff elapsed → retry below
                    }

                    // First sight: create with the FULL current state.
                    lux::render::Sprite2DInstanceData d{};
                    std::memcpy(d.m, m, sizeof(m));
                    d.uv[0] = sp.uv_rect.x(); d.uv[1] = sp.uv_rect.y();
                    d.uv[2] = sp.uv_rect.z(); d.uv[3] = sp.uv_rect.w();
                    d.tint             = sp.tint;
                    d.texture_bindless = tex;

                    Live snap{};
                    std::memcpy(snap.m, d.m, sizeof(snap.m));
                    std::memcpy(snap.uv, d.uv, sizeof(snap.uv));
                    snap.tint     = d.tint;
                    snap.tex      = d.texture_bindless;
                    snap.priority = prio;
                    snap.visible  = sp.visible;
                    snap.group    = sp.group;

                    auto req = canvas.addSprite(ctx.scene(), d, prio, sp.visible, sp.group);
                    req.then([this, e, snap](const lux::render::Sprite2DSlotReply& r)
                    {
                        pending_.erase(e);
                        if (r.status != lux::render::ECanvas2DCreateStatus::Ok || r.handle.is_null())
                        {
                            // Failed create never becomes live (G-05). A non-Ok reply
                            // that nonetheless carries a handle must be reclaimed, not
                            // dropped — route it to the orphan set.
                            if (r.handle.valid())
                                orphans_.push_back(r.handle);
                            if (!stopping_)
                            {
                                const bool transient =
                                    r.status == lux::render::ECanvas2DCreateStatus::CapacityExhausted;
                                failed_[e] = FailRecord{!transient, kTransientRetryDrives};
                            }
                            return;
                        }
                        if (stopping_)                       // late reply during teardown →
                        { orphans_.push_back(r.handle); return; }   // destroyed in flushShutdownCleanup
                        failed_.erase(e);
                        Live L = snap;
                        L.handle = r.handle;
                        live_.emplace(e, L);
                    });
                    pending_.emplace(e, std::move(req));
                });
        }

        void finalize(RenderableBridgeContext& ctx) override
        {
            if (!batch_.empty())
                ctx.canvas2d().updateTransforms(batch_);   // ONE bulk op for every dirty sprite
        }

        void reap(lux::meta::EntityRegistry& registry, RenderableBridgeContext& ctx) override
        {
            auto canvas = ctx.canvas2d();
            for (auto it = live_.begin(); it != live_.end(); )
            {
                const auto e = it->first;
                if (!registry.valid(e) || !registry.all_of<SpriteComponent, WorldTransform2DComponent>(e))
                {
                    canvas.removeSprite(ctx.scene(), it->second.handle);
                    it = live_.erase(it);
                }
                else ++it;
            }
            std::erase_if(failed_, [&](const auto& kv) {
                return !registry.valid(kv.first) ||
                       !registry.all_of<SpriteComponent, WorldTransform2DComponent>(kv.first);
            });
        }

        void beginShutdown(RenderableBridgeContext& ctx) override
        {
            auto canvas = ctx.canvas2d();
            for (auto& [e, L] : live_)
                canvas.removeSprite(ctx.scene(), L.handle);
            live_.clear();
            failed_.clear();
            stopping_ = true;   // keep pending_; late replies route into orphans_
        }

        [[nodiscard]] bool hasPendingShutdownWork() const override { return !pending_.empty(); }

        void flushShutdownCleanup(RenderableBridgeContext& ctx) override
        {
            auto canvas = ctx.canvas2d();
            for (const auto& h : orphans_)
                canvas.removeSprite(ctx.scene(), h);
            orphans_.clear();
        }

    private:
        /// Per-FRAME texture-resolution memo (a scene draws from a handful of
        /// atlases; resolve once per distinct id per frame). Frame-scoped on
        /// purpose: ensureTexture must run once per id per frame to advance the
        /// async upload state machine — a cross-frame memo would serve stale indices.
        std::uint32_t resolveBindless(RenderableBridgeContext& ctx, const lux::asset::asset_id_t& id)
        {
            for (const auto& [mid, idx] : tex_memo_)
                if (mid == id) return idx;
            std::uint32_t idx = lux::render::kNoTexture;   // upload in flight → tint-only this frame
            const auto th = ctx.ensureTexture(id);
            if (!th.is_null())
                idx = th.index;                            // RTextureHandle::index IS the bindless index
            tex_memo_.emplace_back(id, idx);
            return idx;
        }

        std::unordered_map<lux::meta::entity_id, Live> live_;
        std::unordered_map<lux::meta::entity_id,
                           lux::render::RenderRequest<lux::render::Sprite2DSlotReply>> pending_;
        std::unordered_map<lux::meta::entity_id, FailRecord> failed_;
        bool                                          stopping_{false};
        std::vector<lux::render::Sprite2DHandle>      orphans_;
        std::vector<lux::render::Sprite2DTransformEntry> batch_;   // dirty transforms this frame
        std::vector<std::pair<lux::asset::asset_id_t, std::uint32_t>> tex_memo_;
    };

} // namespace lux::gameplay::d2
