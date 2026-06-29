#pragma once
/**
 * @file RenderableSystem.hpp
 * @brief ECS -> renderer bridge. Submits MeshComponent entities to the
 *        render thread via RenderSession.
 *
 * Iterates entities carrying (MeshComponent + WorldTransformComponent):
 *   - first sight  -> resolve mesh/material assets, upload to the renderer
 *                     (cached by asset id), create a render instance, make
 *                     it visible in the bound view, remember its handle.
 *   - every frame  -> push the current world transform (fire-and-forget).
 *
 * Design notes:
 *   - This is an *app-level* system, NOT a World built-in: it needs a
 *     RenderSession + scene + view that World has no business owning.
 *     Wire it explicitly:
 *         world.addSystem(std::make_unique<RenderableSystem>(
 *             session, asset_manager, scene_id, view));
 *   - pImpl hides every render type from this header, so gameplay's public
 *     interface stays render-free and gameplay only needs a PRIVATE link to
 *     lux::engine::function::render (RenderableSystem.cpp).
 *   - The entity -> RenderObjectHandle map and asset-id -> GPU-handle caches
 *     live inside the impl, not on components — components stay pure data.
 *
 * A sibling path covers skeletal meshes:
 * SkeletalMeshComponent + AnimatorComponent -> geometry_kind=SkeletalMesh +
 * a transient-vertex-pool source filled by the skinning compute pass.
 */

#include <cstdint>
#include <memory>
#include <unordered_set>

#include <lux/engine/gameplay/world/systems/ISystem.hpp>      // ISystem base (neutral gameplay core)
#include <lux/engine/gameplay/world/systems/AssetLoadFn.hpp>  // injected async-load hook
#include <lux/engine/function/visibility.h>

// RenderSceneId / ViewHandle are now lux::cxx::SlotKey<Tag> aliases (generational
// handles), which cannot be forward-declared — so we include their header-only
// type headers (same as the sibling RenderableBridgeContext.hpp). These pull no
// render link symbols, so the render_bridge->render link stays PRIVATE; every
// other render type below remains forward-declared only.
#include <lux/engine/render/core/RenderSceneId.hpp>   // lux::render::RenderSceneId (SlotKey alias)
#include <lux/engine/render/core/FeatureHandle.hpp>   // lux::render::ViewHandle (SlotKey alias)

// Forward declarations for the remaining render types — no other render headers
// leak into render_bridge's public interface.
namespace lux::asset  { class AssetManager; }
namespace lux::render {
    class RenderSession;
    struct SkinningOperationIds;   // feature-scoped skinning op-ids (by value below)
    struct MeshStackOperationIds;  // feature-scoped mesh-stack op-ids (by value below)
    struct MaterialOperationIds;   // feature-scoped material op-ids (by value below)
    class  FeatureRegistry;        // name → {handle, ops} (setFeatures, for PARAM/POOL bridges)
}

namespace lux::gameplay
{
    class IRenderableBridge;

    class LUX_FUNCTION_PUBLIC RenderableSystem final : public lux::gameplay::ISystem
    {
    public:
        /// @param session    render command session (non-owning; must outlive this)
        /// @param mgr        asset source for mesh/material resolution (non-owning)
        /// @param scene_id   render scene to submit instances into
        /// @param view       view in which submitted instances are made visible
        ///
        /// Constructs with the two default INSTANCE bridges (static + skeletal
        /// mesh) pre-registered via registerComponent<> so existing apps require
        /// no wiring change. Additional components register via
        /// `registerComponent<C>()` BEFORE the first `update()` call.
        ///
        /// @param request_load Injected async-load hook forwarded to the bridge
        ///        context (app wires EngineExecutor::requestLoad). Null = the
        ///        bridge installs a synchronous mgr.ensureAsset fallback.
        RenderableSystem(lux::render::RenderSession& session,
                         lux::asset::AssetManager&   mgr,
                         lux::render::RenderSceneId  scene_id,
                         lux::render::ViewHandle     view,
                         lux::gameplay::AssetLoadFn                 request_load = {});
        ~RenderableSystem() override;

        RenderableSystem(const RenderableSystem&)            = delete;
        RenderableSystem& operator=(const RenderableSystem&) = delete;

        /// Add a bespoke `IRenderableBridge` directly (the escape hatch; prefer
        /// `registerComponent<C>` below). Must be called BEFORE the first `update()`
        /// invocation; a debug build asserts on later registration. Bridges are
        /// invoked in registration order (drive -> finalize -> reap).
        void addBridge(std::unique_ptr<IRenderableBridge> bridge);

        /// Register a renderable component declaratively via its `EcsRenderTraits<C>`:
        /// picks the matching generic bridge (Param / Pool / Instance) at compile time
        /// and adds it — the declarative replacement for hand-written bridges. The
        /// definition lives in `render_bridge/RegisterComponent.hpp` (include it at the
        /// call site). Must be called BEFORE the first update(), like addBridge.
        template <class C> void registerComponent();

        /// Publish the set of entities to highlight into the bridge context.
        /// The mesh bridges fold kInstanceFlagHighlight into their per-instance
        /// flags for these entities (single-writer path), driving the
        /// HighlightFeature. Call each frame BEFORE update(); empty clears the
        /// highlight. Entity ids only — no editor type leaks in. The editor's
        /// selection is one client; hover/debug/gameplay can drive it too.
        void setHighlighted(std::unordered_set<lux::meta::entity_id> highlighted);

        /// Publish the scene's SkinningFeature op-ids into the bridge context so
        /// the skeletal mesh bridge can address its bone-upload commands. The render
        /// core no longer names skinning — the feature registers its two ops with
        /// dynamic TypeIds, the app forwards them here. Default-empty (feature
        /// absent) makes the bridge's bone upload a graceful no-op. Set once
        /// after the scene's features are registered.
        void setSkinningOps(lux::render::SkinningOperationIds ops);

        /// Publish the scene's StandardMeshStackFeature op-ids into the bridge
        /// context so the mesh bridges address every instance command (add /
        /// remove / visibility / flags / transform) via MeshStackProxy. The render
        /// core no longer names mesh instances — the feature registers its ops with
        /// dynamic TypeIds, the app forwards them here. Default-empty (feature
        /// absent) makes the mesh bridges' commands graceful no-ops. Set once
        /// after the scene's features are registered.
        void setMeshStackOps(lux::render::MeshStackOperationIds ops);

        /// Publish the scene's StandardMaterialFeature op-ids into the bridge context
        /// so ensureGraphMaterial addresses material upload/modify/destroy via
        /// MaterialProxy. Default-empty (feature absent) makes them graceful no-ops.
        void setMaterialOps(lux::render::MaterialOperationIds ops);

        /// Publish the scene's client feature catalogue (name → {handle, ops}) so
        /// PARAM/POOL bridges resolve their target feature by name. Set once after the
        /// scene's features are registered (editor wires it from EditorRenderInfra).
        /// Precondition for any PARAM/POOL registerComponent.
        void setFeatures(const lux::render::FeatureRegistry& features);

        /// W2c eviction guard: true if any live render instance (or a pending /
        /// failed upload) in the bridge still needs @p id's data — so streaming
        /// CPU-data eviction skips assets shared with an active cell. Cheap (a
        /// few hash lookups). See RenderableBridgeContext::isAssetReferenced.
        [[nodiscard]] bool isAssetReferenced(const lux::asset::asset_id_t& id) const;

        void update(lux::meta::EntityRegistry& registry, float dt) override;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

} // namespace lux::gameplay
