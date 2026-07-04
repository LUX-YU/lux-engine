/**
 * @file RenderableSystem.cpp
 * @brief ECS -> renderer bridge. Thin orchestrator that drives a list of
 *        IRenderableBridge implementations.
 *
 * Per tick the system runs three phases over its registered bridges:
 *
 *   1. `drive`    — each bridge iterates its ECS view, ensures assets are
 *                   uploaded (cached by id), creates first-sight instances,
 *                   refreshes visibility, pushes transforms, and accumulates
 *                   per-frame transient data. Builder is live.
 *   2. `finalize` — each bridge flushes any batched work that needed to
 *                   wait for every other bridge's `drive` to complete
 *                   (used today only by the skeletal mesh bridge for the
 *                   one-shot uploadBoneBatch covering every skinned instance).
 *   3. `reap`     — each bridge walks its instance map and removes /
 *                   releases refcounts for instances whose entity died or
 *                   shed its renderable component. `removeMeshInstance` is
 *                   a builder command so this must run inside `update()`,
 *                   not in a reply continuation.
 *
 * Asset upload / refcount lifecycle is shared across all bridges via
 * `RenderableBridgeContext`. Per-type instance state (live-instance maps,
 * bone-batch scratch) lives inside the bridge.
 *
 * Async model: mid-frame `RenderRequest::result()` would deadlock (replies
 * only resolve after submitFrame), so commands are fire-and-forget and
 * completion via `.then`. Those continuations run during `pumpReplies()`
 * — BEFORE `beginFrame()` in the tick loop — so the frame builder is NOT
 * valid inside them. Therefore continuations may only mutate local maps;
 * every render command (uploadMesh / uploadMaterial / addMeshInstance /
 * makeInstanceVisibleForView / updateTransform / uploadBoneBatch /
 * destroyMesh / destroyMaterial / destroyTexture) is issued from
 * `update()`, where the builder is live.
 */

#include "lux/engine/gameplay/render_bridge/RenderableSystem.hpp"
#include "lux/engine/gameplay/render_bridge/IRenderableBridge.hpp"

#include <cassert>
#include <memory>
#include <utility>
#include <vector>

#include "lux/engine/gameplay/render_bridge/RenderableBridgeContext.hpp"

namespace lux::gameplay
{
    struct RenderableSystem::Impl
    {
        std::unique_ptr<RenderableBridgeContext>          ctx;
        std::vector<std::unique_ptr<IRenderableBridge>>   bridges;
        bool                                              update_started{false};
        bool                                              shutdown_done{false};
    };

    RenderableSystem::RenderableSystem(lux::render::RenderSession& session,
                                       lux::asset::AssetManager&   mgr,
                                       lux::render::RenderSceneId  scene_id,
                                       lux::render::ViewHandle     view,
                                       lux::gameplay::AssetLoadFn                 request_load)
        : impl_(std::make_unique<Impl>())
    {
        impl_->ctx = std::make_unique<RenderableBridgeContext>(
            session, mgr, scene_id, view, std::move(request_load));
        // No components registered by default — render_bridge is component-agnostic.
        // A dimension kit registers its renderables (e.g. d3::registerRenderables
        // does the standard 3D set) before the first update().
    }

    RenderableSystem::~RenderableSystem() = default;

    void RenderableSystem::addBridge(std::unique_ptr<IRenderableBridge> bridge)
    {
        assert(!impl_->update_started &&
               "RenderableSystem::addBridge must be called before the first update()");
        if (!bridge) return;
        impl_->bridges.push_back(std::move(bridge));
    }

    void RenderableSystem::setHighlighted(std::unordered_set<lux::meta::entity_id> highlighted)
    {
        impl_->ctx->setHighlighted(std::move(highlighted));
    }

    void RenderableSystem::setSkinningOps(lux::render::SkinningOperationIds ops)
    {
        impl_->ctx->setSkinningOps(ops);
    }

    void RenderableSystem::setMeshStackOps(lux::render::MeshStackOperationIds ops)
    {
        impl_->ctx->setMeshStackOps(ops);
    }

    void RenderableSystem::setMaterialOps(lux::render::MaterialOperationIds ops)
    {
        impl_->ctx->setMaterialOps(ops);
    }

    void RenderableSystem::setFeatures(const lux::render::FeatureRegistry& features)
    {
        impl_->ctx->setFeatures(features);
    }

    bool RenderableSystem::isAssetReferenced(const lux::asset::asset_id_t& id) const
    {
        return impl_->ctx->isAssetReferenced(id);
    }

    void RenderableSystem::shutdown()
    {
        if (impl_->shutdown_done) return;          // idempotent
        impl_->shutdown_done = true;
        // Builder must be LIVE here — bridge shutdown emits destroy / removeMeshInstance
        // commands, the same discipline as reap. The caller (EditorScene teardown, G-03)
        // opens a frame, calls this, then pumps replies before resetting session/scene.
        for (auto& b : impl_->bridges) b->shutdown(*impl_->ctx);
    }

    void RenderableSystem::update(lux::meta::EntityRegistry& registry, float /*dt*/)
    {
        if (impl_->shutdown_done)   // torn down: ctx/bridges are empty and the scene it
        {                           // targeted may be gone — reject rather than re-drive.
            assert(false && "RenderableSystem::update() called after shutdown()");
            return;
        }
        impl_->update_started = true;

        // Three-phase loop. See file header for invariants.
        for (auto& b : impl_->bridges) b->drive(registry, *impl_->ctx);
        for (auto& b : impl_->bridges) b->finalize(*impl_->ctx);
        for (auto& b : impl_->bridges) b->reap(registry, *impl_->ctx);
    }

} // namespace lux::gameplay
