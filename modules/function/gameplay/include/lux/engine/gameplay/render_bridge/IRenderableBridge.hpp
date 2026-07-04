#pragma once
/**
 * @file IRenderableBridge.hpp
 * @brief Common base of the three generic ECS->renderer bridges.
 *
 * `RenderableSystem` drives a list of these in three phases per tick
 * (`drive` -> `finalize` -> `reap`). Each concrete bridge carries the
 * invariant lifecycle of one renderable KIND — view query, asset
 * ensure/refcount, instance creation, state push, removal — once:
 *   ParamBridge<C>    (Grid, Skybox),
 *   PoolBridge<C>     (Directional/Point/Spot Light),
 *   InstanceBridge<C> (Static/Skeletal Mesh).
 *
 * You do NOT subclass this to add a renderable component. Instead specialize
 * `EcsRenderTraits<C>` (declaring the component's KIND + a few hooks) and call
 * `RenderableSystem::registerComponent<C>()`, which selects the matching bridge
 * at compile time. This interface is the runtime seam the phase loop iterates;
 * the bridge templates are the only implementations.
 *
 * `RenderableBridgeContext` — the shared services every bridge consumes — is a
 * PUBLIC header, so the bridge templates instantiate cleanly outside the gameplay
 * module (the editor's bringUp, a kit's install()).
 */

#include <lux/engine/function/visibility.h>

namespace lux::meta     { class EntityRegistry; }

namespace lux::gameplay
{
    /// Shared bridge-facing services owned by `RenderableSystem` (asset
    /// ensure/refcount, scene/view, feature op-ids). Defined in the public
    /// header `RenderableBridgeContext.hpp`.
    class RenderableBridgeContext;

    class LUX_FUNCTION_PUBLIC IRenderableBridge
    {
    public:
        virtual ~IRenderableBridge() = default;

        IRenderableBridge(const IRenderableBridge&)            = delete;
        IRenderableBridge& operator=(const IRenderableBridge&) = delete;

        /// Pass 1 — iterate this bridge's ECS view, ensure assets, emit
        /// instance-creation / visibility / transform commands, accumulate
        /// any per-frame transient data (e.g. bone palettes). Runs inside
        /// `RenderableSystem::update()` with the FrameProgram builder live.
        virtual void drive(lux::meta::EntityRegistry& registry, RenderableBridgeContext& ctx) = 0;

        /// Pass 2 — batched flush after every bridge's `drive` has run.
        /// Used by the skeletal mesh bridge to emit one `uploadBoneBatch`
        /// covering every skinned instance. Most bridges leave this no-op.
        virtual void finalize(RenderableBridgeContext& /*ctx*/) {}

        /// Pass 3 — D1 reap. For each instance whose entity left the view
        /// (died, shed its component, or gained an exclusion tag), emit
        /// `removeMeshInstance` and release the asset refcounts (which
        /// destroys the GPU resource at refcount==0 via `RenderSession::
        /// destroy*`). Runs with the builder still live.
        virtual void reap(lux::meta::EntityRegistry& registry, RenderableBridgeContext& ctx) = 0;

        /// Explicit teardown — release EVERY live render object, asset refcount, and
        /// pending create continuation this bridge holds, IN PLACE. Called once by
        /// `RenderableSystem::shutdown()` with the FrameProgram builder LIVE (it emits
        /// destroy / removeMeshInstance builder commands, same as `reap`), BEFORE the
        /// bridge — and the RenderSession / scene it targets — are destroyed.
        /// Pure virtual on purpose: a bridge must not silently leak by forgetting to
        /// tear down. Destructors must NOT send render commands (a dtor may run after
        /// the frame / session is already gone); `shutdown` is the single seam for it.
        virtual void shutdown(RenderableBridgeContext& ctx) = 0;

    protected:
        IRenderableBridge() = default;
    };

} // namespace lux::gameplay
