#pragma once
/**
 * @file IRenderableBridge.hpp
 * @brief Common base of the three generic ECS->renderer bridges.
 *
 * `RenderableSystem` drives a list of these in three phases per tick
 * (`drive` -> `finalize` -> `reap`), plus a one-shot `shutdown` at teardown
 * (explicit + builder-live: a dtor cannot reliably send GPU delete commands, so a
 * skipped shutdown leaks). Each concrete bridge carries the invariant lifecycle of
 * one renderable KIND — view query, asset ensure/refcount, instance creation, state
 * push, removal — once:
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

        // ── Two-phase teardown (drain, don't cancel) ──────────────────────────
        // A pending async create (addMeshInstance / createLight) whose reply has NOT
        // arrived cannot be simply cancelled at teardown: `RenderRequest::cancel()`
        // only detaches the CLIENT continuation — the server still processes the
        // command and creates the object, which then leaks in a REUSED scene (editor
        // scene swaps keep the same render scene; nothing reaps it). So teardown
        // DRAINS instead: keep the pending continuations, pump replies until they
        // settle, then destroy whatever the late replies created.

        /// Phase 1 (builder LIVE): destroy every KNOWN live object + release its asset
        /// refcounts, then enter "stopping" mode — do NOT cancel pending creates; their
        /// late replies route the server-created handle into an orphan list (destroyed
        /// in phase 2) instead of becoming live. Pure virtual: a bridge must not leak by
        /// forgetting to tear down. Destructors must NOT send render commands.
        virtual void beginShutdown(RenderableBridgeContext& ctx) = 0;

        /// True while async creates started before beginShutdown are still in flight.
        /// The caller submits+pumps until this is false across ALL bridges (and the
        /// context has no inflight uploads) BEFORE calling flushShutdownCleanup.
        [[nodiscard]] virtual bool hasPendingShutdownWork() const = 0;

        /// Phase 2 (builder LIVE): destroy the orphan objects the drained late replies
        /// created while stopping. Call once, after the drain reports no pending work.
        virtual void flushShutdownCleanup(RenderableBridgeContext& ctx) = 0;

    protected:
        IRenderableBridge() = default;
    };

} // namespace lux::gameplay
