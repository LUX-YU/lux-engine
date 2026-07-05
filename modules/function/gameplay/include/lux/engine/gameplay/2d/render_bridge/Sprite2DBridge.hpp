#pragma once
// ============================================================================
//  Sprite2DBridge.hpp — ECS SpriteComponent → Canvas2DFeature (lux::gameplay::d2).
//
//  A BESPOKE IRenderableBridge (design §0R V1/V2: a custom bridge, NOT the generic
//  INSTANCE/POOL — those are MeshStack-specific). Each frame it reads every visible
//  (SpriteComponent, WorldTransform2DComponent) entity, builds a SpriteDraw batch and
//  submits it to the scene's single Canvas2DFeature via Canvas2DProxy.
//
//  Submission is a fire-and-forget Blob (TRANSIENT — the whole batch is rebuilt +
//  re-uploaded each frame; there is no retained SpriteBatchHandle and no async create),
//  so the two-phase teardown-drain contract is trivial: nothing is ever in flight
//  (hasPendingShutdownWork() is always false, reap/flushShutdownCleanup are no-ops).
//
//  If the scene has no Canvas2DFeature (a pure-3D scene never registers it), ctx.canvas2d()
//  yields an invalid proxy and submitSprites() no-ops — so registering this bridge on a
//  3D scene costs only the view iteration (R2-04 contract).
// ============================================================================

#include <lux/engine/gameplay/render_bridge/IRenderableBridge.hpp>
#include <lux/engine/gameplay/render_bridge/RenderableBridgeContext.hpp>       // ctx.canvas2d() / scene()
#include <lux/engine/gameplay/2d/world/components/SpriteComponent.hpp>
#include <lux/engine/gameplay/2d/world/components/WorldTransform2DComponent.hpp>
#include <lux/engine/render/renderer/features/canvas2d/Canvas2DOperation.hpp>  // SpriteDraw / DrawOrderKey / Rect2D
#include <lux/engine/meta/LuxObject.hpp>   // EntityRegistry / entity_id / entt::to_integral

#include <cstdint>
#include <cstring>
#include <vector>

namespace lux::gameplay::d2
{
    class Sprite2DBridge final : public lux::gameplay::IRenderableBridge
    {
    public:
        /// @p producer_order distinguishes this producer in the DrawOrderKey when several
        /// bridges share a layer (design §3.3). Unique per registered 2D producer.
        explicit Sprite2DBridge(std::uint32_t producer_order = 0) noexcept
            : producer_order_(producer_order) {}

        void drive(lux::meta::EntityRegistry& registry, RenderableBridgeContext& ctx) override
        {
            if (stopping_) return;
            batch_.clear();

            registry.view<SpriteComponent, WorldTransform2DComponent>().each(
                [&](lux::meta::entity_id e, const SpriteComponent& sp, const WorldTransform2DComponent& wt)
                {
                    if (!sp.visible) return;

                    // World quad = WorldTransform2D * Scale(sprite size). The render side
                    // expands a UNIT quad centred at the origin; `size` scales it and
                    // wt.world places/rotates it. (M * Scale(sx,sy,1) == columns 0/1 of M
                    // scaled by sx/sy — Eigen is column-major.)
                    Eigen::Matrix4f m = wt.world;
                    m.col(0) *= sp.size.x();
                    m.col(1) *= sp.size.y();

                    lux::render::SpriteDraw d{};
                    std::memcpy(d.transform, m.data(), 16 * sizeof(float));
                    d.uv               = lux::render::Rect2D{ sp.uv_rect.x(), sp.uv_rect.y(),
                                                             sp.uv_rect.z(), sp.uv_rect.w() };
                    d.tint             = sp.tint;
                    // Resolve the sprite's texture to a bindless set-2 index. Null asset →
                    // tint-only (kNoTexture, the SpriteDraw default). Upload still in flight
                    // → handle null → stays tint-only THIS frame, textures once it settles.
                    //
                    // LIFECYCLE: ensureTexture is idempotent (uploads once, cached by id — no
                    // per-frame re-upload). A transient producer keeps no per-instance state,
                    // so it does NOT acquire/release per sprite: a used texture is SCENE-SCOPED
                    // (freed at scene teardown via destroyUnreferencedResources), not per-frame
                    // leaked. Bounded over-retention is fine for the MVP; per-sprite eviction
                    // (release when no live sprite references a texture) is a streaming-era
                    // refinement (needs retained texture tracking), deliberately deferred.
                    d.texture_bindless = lux::render::kNoTexture;
                    if (!sp.texture.is_nil())
                    {
                        const auto th = ctx.ensureTexture(sp.texture);
                        if (!th.is_null())
                            d.texture_bindless = th.index;   // RTextureHandle::index IS the bindless index
                    }
                    d.key              = lux::render::DrawOrderKey{
                        sp.layer, /*sublayer=*/0, sp.order, producer_order_,
                        static_cast<std::uint64_t>(entt::to_integral(e)) };  // entity id = deterministic tie-break
                    batch_.push_back(d);
                });

            if (!batch_.empty())
                ctx.canvas2d().submitSprites(ctx.scene(), batch_);   // no-ops if the scene has no Canvas2DFeature
        }

        // Transient producer: nothing is retained across frames, so reap has nothing to
        // release and shutdown has nothing to drain.
        void reap(lux::meta::EntityRegistry& /*registry*/, RenderableBridgeContext& /*ctx*/) override {}
        void beginShutdown(RenderableBridgeContext& /*ctx*/) override { stopping_ = true; batch_.clear(); }
        [[nodiscard]] bool hasPendingShutdownWork() const override { return false; }
        void flushShutdownCleanup(RenderableBridgeContext& /*ctx*/) override {}

    private:
        std::uint32_t                        producer_order_{0};
        bool                                 stopping_{false};
        std::vector<lux::render::SpriteDraw> batch_;   // reused across frames
    };

} // namespace lux::gameplay::d2
