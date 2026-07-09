#pragma once
/**
 * @file RenderableBridgeContext.hpp
 *
 * Bridge-facing shared state owned by `RenderableSystem`. A PUBLIC header: the
 * generic bridges (Param/Pool/Instance) instantiate at their registration call
 * sites (the editor's bringUp, a kit's `d3::registerRenderables`), so this header
 * and its `ensure*` / `acquire*` / `release*` API must be visible — and compile —
 * outside this module's `.cpp`.
 *
 * Holds:
 *   - non-owning references to `RenderSession`, `AssetManager`, the bound
 *     `RenderSceneId` and `ViewHandle`,
 *   - the three asset caches (mesh / material / texture) plus their refcount +
 *     cascade-release machinery (the API every bridge uses to bring assets up +
 *     tear them down),
 *   - the feature op-ids + the highlight set the bridges read each tick.
 */

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <lux/engine/meta/LuxObject.hpp>   // lux::meta::entity_id
#include <lux/engine/function/visibility.h>   // LUX_FUNCTION_PUBLIC (cross-module bridge calls)

#include <lux/engine/asset/MaterialAsset.hpp>
#include <lux/engine/asset/MaterialInstanceAsset.hpp>
#include <lux/engine/asset/MeshAsset.hpp>
#include <lux/engine/asset/TextureAsset.hpp>
// (asset/MaterialAsset.hpp + description/Material.hpp dropped — the builtin closure
//  material path was retired in W5a; the bridge handles only graph materials now.)
#include <lux/engine/gameplay/world/systems/AssetLoadFn.hpp>   // injected async-load hook
#include <lux/engine/render/core/RenderResourceHandle.hpp>
#include <lux/engine/render/core/ResourceHandle.hpp>        // ShaderHandle
#include <lux/engine/render/core/RenderSceneId.hpp>
#include <lux/engine/render/core/FeatureHandle.hpp>          // ViewHandle
#include <lux/engine/render/resources/material/GraphMaterialData.hpp> // render::GraphMaterialData
#include <lux/engine/render/renderer/features/skinning/SkinningOperation.hpp> // SkinningOperationIds
#include <lux/engine/render/renderer/features/meshstack/MeshStackOperation.hpp> // MeshStackOperationIds / MeshStackProxy
#include <lux/engine/render/renderer/features/material/MaterialOperation.hpp>  // MaterialOperationIds / MaterialProxy
#include <lux/engine/render/renderer/features/canvas2d/Canvas2DFeatureOps.hpp> // Canvas2DOperationIds / Canvas2DProxy (2D)
#include <lux/engine/render/comm/server/FeatureRegistry.hpp>                  // name → {handle, ops} (PARAM/POOL bridges)
#include <lux/engine/render/comm/client/RenderRequest.hpp>                    // in-flight upload/compile cancel (teardown)

namespace lux::asset  { class AssetManager; }
namespace lux::render { class RenderSession; }

namespace lux::gameplay
{
    /// Shared state + behaviour every `IRenderableBridge` consumes.
    /// One instance lives inside `RenderableSystem::Impl` and is passed to
    /// each bridge's `drive` / `finalize` / `reap` calls.
    class RenderableBridgeContext
    {
    public:
        /// @param request_load Injected "materialize this asset asynchronously"
        ///        hook (app wires EngineExecutor::requestLoad here). When an
        ///        ensure* call resolves @p id to an absent/data-less shell it
        ///        fires this and returns a null handle for now; the upload
        ///        proceeds on a later frame once the data has been injected.
        ///        A null fn installs a synchronous `mgr.ensureAsset` fallback,
        ///        so un-wired headless/tool processes still load.
        RenderableBridgeContext(lux::render::RenderSession& session,
                                lux::asset::AssetManager&   mgr,
                                lux::render::RenderSceneId  scene_id,
                                lux::render::ViewHandle     view,
                                lux::gameplay::AssetLoadFn                 request_load) noexcept;

        ~RenderableBridgeContext()
        {
            // Cancel every in-flight upload/compile: its reply may be pumped AFTER
            // this context is destroyed (replies share one queue — an unrelated
            // pumpReplies elsewhere can dispatch it), and the continuation captures
            // `this` to touch a *_resources_ map → use-after-free. Cancelling
            // detaches the continuation; an upload whose wire op already ran is an
            // orphan reclaimed by scene destruction (the reap / scene-destroy
            // fallback), not recorded here.
            for (auto& cancel : inflight_cancels_) cancel();
        }

        // ── Plain accessors ────────────────────────────────────────────
        [[nodiscard]] lux::render::RenderSession&  session()       noexcept { return *session_; }
        [[nodiscard]] lux::asset::AssetManager&    assets()        noexcept { return *mgr_; }
        [[nodiscard]] lux::render::RenderSceneId   scene()   const noexcept { return scene_; }
        [[nodiscard]] lux::render::ViewHandle      view()    const noexcept { return view_; }
        /// Client-side feature catalogue (name → {handle, ops}). PARAM/POOL bridges
        /// resolve their target feature's handle + op-ids by name through this. Set
        /// by the app after the scene's features are registered (editor wires it from
        /// EditorRenderInfra). Precondition for any PARAM/POOL registerComponent.
        /// Null-object before setFeatures(): a host that drives bridges without ever
        /// wiring a registry gets an EMPTY catalogue — every name lookup yields invalid
        /// ops and the caller's proxy no-ops — instead of UB on a null deref.
        [[nodiscard]] const lux::render::FeatureRegistry& features() const noexcept
        {
            static const lux::render::FeatureRegistry kEmptyFeatures{};
            return features_ ? *features_ : kEmptyFeatures;
        }
        void setFeatures(const lux::render::FeatureRegistry& reg) noexcept { features_ = &reg; }
        /// Feature-scoped skinning op-ids (for SkinningProxy); invalid when no
        /// SkinningFeature is present in the scene. Set by the app after the scene's
        /// SkinningFeature is registered (editor wires it from the factory reply).
        [[nodiscard]] lux::render::SkinningOperationIds skinningOps() const noexcept { return skinning_ops_; }
        void setSkinningOps(lux::render::SkinningOperationIds ops) noexcept { skinning_ops_ = ops; }
        /// Feature-scoped mesh-stack op-ids (for MeshStackProxy); invalid when the
        /// scene has no StandardMeshStackFeature. Set by the app after the scene's
        /// StandardMeshStackFeature is registered. The mesh bridge addresses every
        /// instance command (add / remove / visibility / flags / transform) through
        /// these — the core protocol no longer names mesh instances.
        [[nodiscard]] lux::render::MeshStackOperationIds meshStackOps() const noexcept { return mesh_stack_ops_; }
        void setMeshStackOps(lux::render::MeshStackOperationIds ops) noexcept { mesh_stack_ops_ = ops; }
        /// A fresh MeshStackProxy bound to this context's session + ops. Cheap
        /// (a pointer + a small id array) — construct per call site, like
        /// SkinningProxy; no-ops if no StandardMeshStackFeature.
        [[nodiscard]] lux::render::MeshStackProxy meshStack() noexcept { return {*session_, mesh_stack_ops_}; }
        /// Feature-scoped material op-ids (for MaterialProxy); invalid when the scene
        /// has no StandardMaterialFeature. Set by the app after the scene's
        /// StandardMaterialFeature is registered. The bridge addresses material upload
        /// / modify / destroy through these — the core protocol no longer names them.
        [[nodiscard]] lux::render::MaterialOperationIds materialOps() const noexcept { return material_ops_; }
        void setMaterialOps(lux::render::MaterialOperationIds ops) noexcept { material_ops_ = ops; }
        /// A fresh MaterialProxy bound to this context's session + ops (cheap;
        /// construct per call site; no-ops if no StandardMaterialFeature).
        [[nodiscard]] lux::render::MaterialProxy material() noexcept { return {*session_, material_ops_}; }
        /// A fresh Canvas2DProxy for the scene's Canvas2DFeature — the retained 2D
        /// bridges issue their instance commands (create/remove/deltas) through it.
        /// Unlike meshStack() (which caches ops the app sets), this resolves the ops
        /// from the already-wired `features_` registry BY NAME each call, so it needs
        /// no extra wiring and is INVALID exactly when the scene has no Canvas2DFeature
        /// (a pure-3D scene, or before setFeatures) → every proxy op then no-ops, so a
        /// 3D path pays nothing (R2-04 contract). Cheap (a pointer + a name lookup).
        [[nodiscard]] lux::render::Canvas2DProxy canvas2d() noexcept
        {
            return { *session_, features().ops<lux::render::Canvas2DOperationIds>("Canvas2D") };
        }

        // ── Highlight bridge ────────────────────────────────────────────
        // The set of entities to highlight (a multi-mesh root expands to root +
        // all descendant mesh entities). The mesh bridges fold
        // kInstanceFlagHighlight into their per-instance flags for entities in
        // this set — the SINGLE-writer flags path, so the outline rides the same
        // updateInstanceFlags diff as cast_shadow / visible. Publish each frame
        // BEFORE RenderableSystem::update(); empty = nothing highlighted. Pure
        // data (entity ids); the editor's selection is one client that drives it.
        void setHighlighted(std::unordered_set<lux::meta::entity_id> set) noexcept
        {
            highlighted_ = std::move(set);
        }
        [[nodiscard]] bool isHighlighted(lux::meta::entity_id e) const noexcept
        {
            return highlighted_.find(e) != highlighted_.end();
        }

        // ── Asset upload (idempotent, async-safe) ──────────────────────
        // Returns the GPU handle once the upload reply has arrived. Returns
        // a null handle while pending OR if the entry settled as known-bad
        // (failed upload / unsupported format). MUST be called from
        // `update()` — these methods push builder commands.
        // LUX_FUNCTION_PUBLIC: a bridge instantiated in ANOTHER module (a 2D kit /
        // plugin, or a headless test) compiles its drive() there and calls these —
        // so the non-inline asset ensure/refcount methods must be exported (the
        // "cross-module bridge calls" the header's visibility.h include anticipates).
        [[nodiscard]] LUX_FUNCTION_PUBLIC lux::render::RMeshHandle     ensureMesh(const lux::asset::asset_id_t& id);
        // ensureMaterial transparently dispatches MATERIAL assets to the
        // graph-material path (multi-stage: compile baked frags -> resolve
        // textures -> uploadGraphMaterial); bridges call ensureMaterial only.
        [[nodiscard]] LUX_FUNCTION_PUBLIC lux::render::RMaterialHandle ensureMaterial(const lux::asset::asset_id_t& id);
        [[nodiscard]] LUX_FUNCTION_PUBLIC lux::render::RTextureHandle  ensureTexture(const lux::asset::asset_id_t& id);

        // ── Refcount lifecycle ─────────────────────────────────────────
        // acquireMesh / acquireMaterial bump the asset's refcount (and, for
        // materials at 0->1, transitively bump each of its textures).
        // release* decrement; at 0 the GPU resource is destroyed via the
        // matching `session->destroy*` (FIF-safe under the server-side
        // DeferredDestroyQueue) and the cache entry erased.
        LUX_FUNCTION_PUBLIC void acquireMesh    (const lux::asset::asset_id_t& mesh_id);
        LUX_FUNCTION_PUBLIC void acquireMaterial(const lux::asset::asset_id_t& mat_id);
        LUX_FUNCTION_PUBLIC void releaseMesh    (const lux::asset::asset_id_t& mesh_id);
        LUX_FUNCTION_PUBLIC void releaseMaterial(const lux::asset::asset_id_t& mat_id);
        void releaseTexture (const lux::asset::asset_id_t& tex_id);

        // ── W2c eviction guard ──────────────────────────────────────────
        // True if the bridge currently tracks @p id in ANY cache (mesh /
        // graph-material / material-instance / texture) — i.e. a live instance
        // (or a pending/failed upload) still needs its data. Streaming CPU-data
        // eviction (AssetManager::unloadData) MUST skip referenced assets: a
        // shared mesh/material can be live in an active cell even after some
        // dormant cell's entities were reaped. Entries are erased at refcount→0
        // by release*/reap, so "no entry" == "unreferenced" == safe to evict.
        [[nodiscard]] bool isAssetReferenced(const lux::asset::asset_id_t& id) const noexcept
        {
            return mesh_resources_.contains(id)
                || graph_material_resources_.contains(id)
                || material_instance_resources_.contains(id)
                || texture_resources_.contains(id);
        }

        // ── Teardown drain (two-phase shutdown) ─────────────────────────────
        /// True while any upload / compile is still in flight (a cache entry not yet
        /// settled). Teardown pumps until this is false so no inflight upload is
        /// cancelled mid-flight — a cancelled upload whose server worker then completes
        /// leaves a global resource nobody tracks (never destroyed).
        [[nodiscard]] bool hasInflightUploads() const noexcept;
        /// Destroy every SETTLED cached resource left at refcount 0 — uploaded but never
        /// referenced, so release*() never fired for it (no 1→0 transition) and it would
        /// leak. Call once at the teardown flush, after the drain, with the builder LIVE.
        void destroyUnreferencedResources();

    private:
        // Issue an async upload/compile whose continuation captures `this`, made
        // SAFE against this context's destruction: registers @p fn AND records a
        // canceller so ~RenderableBridgeContext detaches it (see the dtor). Every
        // ensure*() async op routes its `.then` through here. Bounded: one entry
        // per unique asset upload/compile (the caches fire each at most once via
        // try_emplace), cleared when this per-scene context dies.
        template <class Reply, class Fn>
        void trackThen(lux::render::RenderRequest<Reply> req, Fn&& fn)
        {
            req.then(std::forward<Fn>(fn));
            inflight_cancels_.emplace_back([req]() mutable { req.cancel(); });
        }

        std::vector<std::function<void()>> inflight_cancels_;

        // ── Cache entry types ──────────────────────────────────────────
        // State machine (per cached entry):
        //   pending  failed  gpu_handle
        //     0       0        null      -> never attempted; need asset to load
        //     1       0        null      -> upload command in flight
        //     0       0        valid     -> ready to draw
        //     0       1        null      -> known-bad (failed upload OR unsupported
        //                                    pixel format); do not retry
        // refcount tracks live references — for meshes/materials that's
        // live ECS instances; for textures it's the live MATERIALS that
        // hold a binding. Drops to 0 -> reap pass destroys the GPU resource.
        //
        // NOTE (async-asset redesign): entries no longer cache an asset handle/
        // pointer. The CPU asset data is re-queried from the manager via
        // `fetchAssetAs<T>(id)` on each ensure* call (a cheap hash lookup, keyed
        // by the same id the entry lives under) — this is what made AssetHandle's
        // onWillUnload lifecycle subscription unnecessary: the bridge keeps only
        // the GPU handle, never a CPU asset pointer that could dangle on eviction.
        struct CachedMesh
        {
            lux::render::RMeshHandle gpu_handle{};
            std::uint32_t refcount{0};
            bool pending{false};
            bool failed{false};
        };
        // (CachedMaterial — the builtin closure material cache — retired in W5a.
        // Only graph materials + instances are cached now.)
        struct CachedTexture
        {
            lux::render::RTextureHandle gpu_handle{};
            std::uint32_t refcount{0};   // counts MATERIALS referencing this texture
            bool pending{false};
            bool failed{false};
        };
        // A graph material is brought up in MULTIPLE async stages (each gated on
        // the previous reply): compile the baked GBuffer frag -> compile the
        // baked Forward frag (optional) -> resolve texture-slot UUIDs to bindless
        // -> uploadGraphMaterial(data, gbuffer_shader, forward_shader). The
        // per-stage *_in_flight flags prevent re-issuing a command while its
        // reply is pending; *_ready gates progression. The entry settles to a
        // valid gpu_handle (ready) or failed==true (known-bad), mirroring the
        // other caches' state machine.
        struct CachedGraphMaterial
        {
            lux::render::RMaterialHandle gpu_handle{};
            std::uint32_t refcount{0};
            bool failed{false};

            // Stage 1/2 — per-pass shader compilation.
            lux::render::ShaderHandle gbuffer_shader{};
            lux::render::ShaderHandle forward_shader{};
            bool gbuffer_ready{false};
            bool forward_ready{false};
            bool gbuffer_in_flight{false};
            bool forward_in_flight{false};
            // compileShader BORROWS the SPIR-V words AND the serialized
            // ShaderInfo bytes until submitFrame(). The asset's vectors are NOT
            // pinned against eviction (the bridge only re-queries the asset, never
            // holds it), so snapshot BOTH into the entry to outlive the borrow —
            // mirroring the builtin path's CachedMaterial::upload_copy.
            std::vector<std::uint32_t> gbuffer_spirv_copy;
            std::vector<std::uint32_t> forward_spirv_copy;
            std::vector<std::byte>     gbuffer_info_bytes;
            std::vector<std::byte>     forward_info_bytes;

            // Stage 4 — uploadGraphMaterial borrows the data until submitFrame().
            lux::render::GraphMaterialData upload_copy{};
            bool upload_in_flight{false};

            // Refcount cascade (mirrors CachedMaterial): texture ids this
            // material acquired refs on at its 0->1 transition.
            std::vector<lux::asset::asset_id_t> texture_ids;
        };

        // A material INSTANCE shares its PARENT graph material's shader/PSO and only
        // overrides params/textures (and optionally render-state). Brought up by
        // gating on ensureGraphMaterial(parent) — which compiles the parent's shaders
        // ONCE — then merging the instance's overrides into a per-instance
        // GraphMaterialData uploaded WITH THE PARENT'S ShaderHandles, so the
        // shader_key (= handle bits) matches the parent + all siblings -> ONE shared
        // bucket/PSO, while submitGraph still gives this instance its own SSBO lane.
        struct CachedMaterialInstance
        {
            lux::render::RMaterialHandle gpu_handle{};
            std::uint32_t refcount{0};
            bool failed{false};
            bool upload_in_flight{false};
            // Parent MATERIAL — refcounted (acquire/releaseMaterial) while this
            // instance is live so the parent's shaders/PSO aren't reaped beneath it.
            lux::asset::asset_id_t parent_id{};
            // uploadGraphMaterial borrows the data until submitFrame().
            lux::render::GraphMaterialData upload_copy{};
            // EFFECTIVE (parent ⊕ overrides) texture-slot ids — for the 1->0 cascade.
            std::vector<lux::asset::asset_id_t> texture_ids;
        };

        // MATERIAL path behind ensureMaterial's type dispatch.
        [[nodiscard]] lux::render::RMaterialHandle
        ensureGraphMaterial(const lux::asset::asset_id_t& id);

        // MATERIAL_INSTANCE path behind ensureMaterial's type dispatch.
        [[nodiscard]] lux::render::RMaterialHandle
        ensureMaterialInstance(const lux::asset::asset_id_t& id);

        lux::render::RenderSession* session_{nullptr};
        lux::asset::AssetManager*   mgr_{nullptr};
        lux::render::RenderSceneId  scene_{};
        lux::render::ViewHandle     view_{};
        lux::render::SkinningOperationIds skinning_ops_{};   // skeletal mesh bridge → SkinningProxy
        lux::render::MeshStackOperationIds mesh_stack_ops_{}; // mesh bridge → MeshStackProxy
        lux::render::MaterialOperationIds  material_ops_{};   // ensureGraphMaterial → MaterialProxy
        const lux::render::FeatureRegistry* features_{nullptr}; // name → {handle, ops} (PARAM/POOL + 2D bridges)
        // Injected async loader (never null after the ctor — falls back to a
        // synchronous mgr.ensureAsset when the caller passes none). Fired from
        // ensure* when an id resolves to an absent/data-less shell.
        lux::gameplay::AssetLoadFn                 request_load_{};

        std::unordered_map<lux::asset::asset_id_t, CachedMesh>          mesh_resources_;
        std::unordered_map<lux::asset::asset_id_t, CachedGraphMaterial>     graph_material_resources_;
        std::unordered_map<lux::asset::asset_id_t, CachedMaterialInstance>  material_instance_resources_;
        std::unordered_map<lux::asset::asset_id_t, CachedTexture>           texture_resources_;

        // Highlight set (see setHighlighted/isHighlighted). Empty by default —
        // headless/tool processes never publish, so nothing is ever highlighted.
        std::unordered_set<lux::meta::entity_id>                           highlighted_;
    };

} // namespace lux::gameplay
