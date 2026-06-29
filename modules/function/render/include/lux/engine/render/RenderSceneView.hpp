#pragma once
/**
 * @file RenderSceneView.hpp
 * @brief Per-scene render-services facade handed to a RenderFeature.
 *
 * RenderSceneView is a NARROW, public facade over the engine-internal
 * RenderScene. `RenderFeature::sceneView()` hands a feature one of these, so an
 * (eventually external) feature sees ONLY the curated per-scene surface below —
 * never RenderScene's full engine API (view management, render-graph compile,
 * material pipeline, …).
 *
 * Same design as RenderContextView: COMPOSITION, not inheritance — a
 * RenderSceneView HOLDS a RenderScene& and forwards to it. RenderScene was
 * designed independently and stays OBLIVIOUS to features/facades; the dependency
 * flows one way (feature → facade → RenderScene). ZERO virtual; the facade is a
 * lightweight value (one pointer). The forwarding bodies live in
 * RenderSceneView.cpp where RenderScene is complete, so this public header
 * pulls no internal RenderScene header onto a consumer.
 *
 * `contextView()` (global) vs `sceneView()` (this, single-scene) stay two distinct narrow
 * facades — faithful to their differing lifetimes/ownership.
 */

#include <lux/engine/render/resources/lifecycle/ResourceRegistry.hpp> // ResourceRegistryBase (public)
#include <lux/engine/render/FrameRetireScheduler.hpp>                 // FrameRetireScheduler::OwnerToken (public)
#include <lux/engine/function/visibility.h>

namespace lux::render
{
    class RenderScene;            // held subject (forwarded to; defined in an internal header)
    class SceneDescriptorArena;   // returned by-reference; defined in an internal header
    class TransferScheduler;      // returned by-reference; defined in an internal header

    class LUX_FUNCTION_PUBLIC RenderSceneView
    {
    public:
        /// Wrap a subject. The facade is a non-owning view; @p scene must outlive
        /// it (it always does — the scene outlives every feature it serves).
        explicit RenderSceneView(RenderScene& scene) noexcept : scene_(&scene) {}

        // ── Per-scene resource registry (find<T> instantiates on caller's T) ──
        [[nodiscard]] ResourceRegistryBase&       sceneRegistry() noexcept;
        [[nodiscard]] const ResourceRegistryBase& sceneRegistry() const noexcept;

        // ── Per-scene descriptor-pool chain + transfer scheduler ────────
        [[nodiscard]] SceneDescriptorArena& descriptorArena() noexcept;
        [[nodiscard]] TransferScheduler&    transferScheduler() noexcept;

        // ── Frame-retire owner token (this scene's GPU-resource owner id) ──
        [[nodiscard]] FrameRetireScheduler::OwnerToken retireOwnerToken() const noexcept;

        // ── Mark the scene's render graph for recompilation ─────────────
        void invalidateGraph() noexcept;

    private:
        RenderScene* scene_;   // the subject this facade forwards to (never null)
    };

} // namespace lux::render
