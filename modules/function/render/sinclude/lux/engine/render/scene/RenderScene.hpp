#pragma once
/**
 * @file RenderScene.hpp
 * @brief RenderScene — a render-module scene container.
 *
 * Owns per-scene state: features, views, render-graph infrastructure,
 * and per-scene material pipeline.  Receives a shared RenderContext
 * (GPU resources, pipelines, uploads) via constructor — RAII, no init().
 *
 * Usage:
 *   auto ctx = std::make_shared<RenderContext>(res_ctx, ci);
 *   RenderScene scene(ctx, {.scene_name = "Main"});
 *   scene.addFeature<ForwardMeshFeature>(config);
 *   // ... per-frame: beginFrame → updateView →
 *   //                renderer.render(scene, cmd, fi) → endFrame
 *
 * Thread ownership: all methods are **render-thread only** unless documented
 * otherwise (`sceneControlMailbox()` / `sceneEvents()` / `syncProducer()` are game-thread safe).
 */

#include <lux/engine/function/visibility.h>
#include <lux/engine/render/FrameStamp.hpp>

#include <iosfwd>   // std::ostream (dumpCompiledGraph)
#include <lux/engine/render/renderer/ViewFamily.hpp> // ERenderPath
#include <lux/engine/render/RenderFeature.hpp>
#include <lux/engine/render/FrameRetireScheduler.hpp>
#include <lux/engine/render/core/RenderTypes.hpp> // common::Size2D
#include <lux/engine/render/pipeline/MaterialPipeline.hpp>
#include <lux/engine/render/scene/View.hpp>
#include <lux/engine/render/renderer/PipelineConfig.hpp>
#include <lux/engine/render/renderer/RenderTargetLayout.hpp>
#include <lux/engine/render/renderer/RenderObjectExtraction.hpp>
#include <lux/engine/render/resources/lifecycle/ResourceRegistry.hpp>
#include <lux/engine/render/resources/descriptor/SceneDescriptorArena.hpp>
#include <lux/engine/render/transfer/TransferScheduler.hpp>
#include <lux/engine/math/AABB.hpp>
#include <lux/cxx/container/SparseSet.hpp>
#include <lux/cxx/container/BasicSparseSet.hpp>   // SlotKeyAutoSparseSet (generational views)

#include <Eigen/Core>

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Forward declarations — avoid pulling in heavy headers
struct VkCommandBuffer_T; // Vulkan opaque handle
using VkCommandBuffer = VkCommandBuffer_T *;

namespace lux::render
{
    class RenderContext;
    class RGVulkanRecorder;
    class RGVulkanResourceAllocator;
    struct RGCompiledGraph;
    struct RGGraphDescription;
    struct RGRecordContext;
    struct RGResourceState;
    class SceneResources;

    namespace extract
    {
        class IEntitySink;
        class ITransformSink;
        class ILightSink;
    }

    // ─────────────────────────────────────────────────────────────────────
    //  Creation / Init
    // ─────────────────────────────────────────────────────────────────────
    //  SceneGraphState — compiled render graph + record context
    // ─────────────────────────────────────────────────────────────────────

    struct SceneGraphState
    {
        std::unique_ptr<RGCompiledGraph> graph;
        RGResourceHandle final_color_handle{};
        bool valid{false};
        RenderTargetLayout last_layout; ///< stored for recompilation
        /// One-shot guard so a scene that can never compile (no SceneColor target)
        /// is reported once instead of every frame; re-armed when it recovers (M21).
        bool render_skip_warned{false};
    };

    // ─────────────────────────────────────────────────────────────────────

    struct ViewCreateInfo
    {
        common::Size2D initial_extent{};
        const char *debug_name{"View"};
    };

    // ─────────────────────────────────────────────────────────────────────
    //  RenderScene
    // ─────────────────────────────────────────────────────────────────────

    class LUX_FUNCTION_PUBLIC RenderScene
    {
    public:
        struct Config
        {
            std::string scene_name{"RenderScene"};
            PipelineConfig pipeline{};
            std::vector<ViewCreateInfo> initial_views{};
        };

        /// Construct from a fully-initialized RenderContext (RAII).
        /// Creates per-scene render-graph infrastructure internally.
        explicit RenderScene(std::shared_ptr<RenderContext> ctx);
        RenderScene(std::shared_ptr<RenderContext> ctx, const Config &cfg);
        ~RenderScene();

        RenderScene(const RenderScene &) = delete;
        RenderScene &operator=(const RenderScene &) = delete;
        RenderScene(RenderScene &&) = delete;
        RenderScene &operator=(RenderScene &&) = delete;

        // ================================================================
        //  Shutdown
        // ================================================================

        /// Explicit cleanup (features, RG infra).  Automatically called by
        /// the destructor; safe to call multiple times.
        void shutdownFull();

        // ================================================================
        //  Feature Management
        // ================================================================

        /**
         * @brief Add a feature and return its assigned feature_id.
         *
         * The caller must save the returned id for later access via
         * getFeature() / getFeatureAs<T>().
         */
        template <typename T, typename... Args>
        FeatureHandle addFeature(Args &&...args)
        {
            const uint32_t extractor_type_id =
                render_object_extractor_.template ensureTypeRegistered<T>();
            auto ptr = std::make_unique<T>(std::forward<Args>(args)...);
            return addFeatureImpl(std::move(ptr), extractor_type_id);
        }

        /// Type-erased feature install for the public SDK plugin path
        /// (lux::render::addFeature, used by a FeatureFactory create_fn).
        /// In-module callers should prefer the templated addFeature<T>() above;
        /// this exists so an external plugin's create_fn can install a feature
        /// without ever seeing the full RenderScene definition. The caller is
        /// responsible for the matching extractor_type_id (the sdk wrapper derives
        /// it from the public RenderObjectExtractor).
        FeatureHandle addFeatureErased(std::unique_ptr<RenderFeature> feature, uint32_t extractor_type_id);

        /// O(1) lookup by handle.  Returns nullptr if the handle is invalid or stale.
        [[nodiscard]] RenderFeature *getFeature(FeatureHandle feature_id) const;

        /// Typed convenience — static_cast after O(1) lookup.
        template <typename T>
        T *getFeatureAs(FeatureHandle feature_id) const
        {
            return static_cast<T *>(getFeature(feature_id));
        }

        bool removeFeature(FeatureHandle feature_id);

        /// Attach the feature's type-level descriptor (from its FeatureFactory) to
        /// the instance. Called by the server right after the factory creates it; the
        /// descriptor then drives type identity, dependency/conflict checks, and the
        /// lifecycle state machine's capability gating. No-op if the handle is stale.
        void setFeatureDescriptor(FeatureHandle feature_id, const FeatureDescriptor& descriptor) noexcept;

        /// RAII bracket around a factory create_fn call: makes the feature it
        /// constructs receive its type-level descriptor BEFORE initAndAttachTo, so the
        /// feature can observe its own descriptor (deps / conflicts / capability flags)
        /// during attach instead of only after. The comm install paths wrap create_fn
        /// in one of these; addFeatureImpl applies the pending descriptor pre-attach.
        /// Replaces the old "create then setFeatureDescriptor afterwards" two-step,
        /// which left the descriptor invisible during attach. (三-2)
        class FeatureInstallScope
        {
        public:
            FeatureInstallScope(RenderScene& scene, const FeatureDescriptor& desc) noexcept
                : scene_(scene) { scene_.pending_install_descriptor_ = &desc; }
            ~FeatureInstallScope() { scene_.pending_install_descriptor_ = nullptr; }
            FeatureInstallScope(const FeatureInstallScope&)            = delete;
            FeatureInstallScope& operator=(const FeatureInstallScope&) = delete;
        private:
            RenderScene& scene_;
        };

        /// True if any live feature in this scene has the given stable type id.
        /// (kInvalidFeatureTypeId never matches — untyped features are excluded.)
        [[nodiscard]] bool hasFeatureOfType(FeatureTypeId type) const noexcept;

        /// Remove all features from this scene.  Invalidates the render graph.
        void removeAllFeatures();

        void setFeatureEnabled(FeatureHandle feature_id, bool enabled);
        void setFeatureEnabled(std::string_view feature_name, bool enabled);

        struct FeatureInfo
        {
            std::string_view name;
            bool enabled;
        };
        std::vector<FeatureInfo> queryFeatures() const;

        /// Per-feature param descriptor for the editor's feature-settings panel.
        /// `struct_name`/`data`/`size` are empty/null/0 for features that expose
        /// no tunable params (paramStructName() == ""). All views point at
        /// render-thread-owned storage valid for the duration of the call.
        struct FeatureParamDesc
        {
            FeatureHandle    id;   // full generational handle (五-5), not just .index —
                                   // so a stale editor selection can't alias a reused slot
            bool             enabled;
            std::string_view name;
            std::string_view struct_name;
            const void*      data;
            std::size_t      size;
        };
        std::vector<FeatureParamDesc> queryFeatureParamDescs() const;

        /// Dense vector of all live features (for iteration).
        const std::vector<std::unique_ptr<RenderFeature>> &features() const noexcept;

        // ================================================================
        //  Render Path Selection
        // ================================================================

        /// Current render path.
        [[nodiscard]] ERenderPath renderPath() const noexcept { return render_path_; }

        /// Switch render path.  No-op if already on the given path.
        /// Removes all features, re-adds features appropriate for the path,
        /// and invalidates the render graph.
        void setRenderPath(ERenderPath path);

        /// Mark the render graph as invalid so it will be recompiled next frame.
        void invalidateGraph() noexcept { graph_state_.valid = false; }

        /// Debug: write a human-readable dump of the CURRENT compiled render
        /// graph (execution order + per-pass reads/writes/barriers) to @p os.
        /// No-op note printed if the graph isn't compiled yet. Render-thread.
        void dumpCompiledGraph(std::ostream& os) const;

        /// Suppress automatic graph recompilation during batch feature changes.
        /// While suppressed, `prepareSceneForRender` will skip recompile even if
        /// the graph is invalid.  Call `resumeGraphRecompile()` when done.
        void suppressGraphRecompile() noexcept { recompile_suppressed_ = true; }

        /// Resume automatic graph recompilation.
        void resumeGraphRecompile() noexcept { recompile_suppressed_ = false; }

        /// Whether graph recompilation is currently suppressed.
        [[nodiscard]] bool isGraphRecompileSuppressed() const noexcept { return recompile_suppressed_; }

        // ================================================================
        //  Pipeline Configuration
        // ================================================================

        [[nodiscard]] const PipelineConfig &pipelineConfig() const noexcept { return pipeline_config_; }

        /// Replace the pipeline configuration.  Invalidates the render graph.
        void setPipelineConfig(PipelineConfig cfg) noexcept
        {
            pipeline_config_ = cfg;
            invalidateGraph();
        }

        /// Set scene-global time (called from comm handler).
        void setSceneTime(float total, float delta, uint64_t frame_number) noexcept
        {
            scene_time_ = total;
            scene_delta_time_ = delta;
            scene_total_frames_ = frame_number;
        }

        // ================================================================
        //  Scene Service Registry
        // ================================================================

        // ================================================================
        //  View Management
        // ================================================================

        [[nodiscard]] ViewHandle addView(const ViewCreateInfo &info);
        void removeView(ViewHandle handle);
        [[nodiscard]] View *getView(ViewHandle handle) noexcept;
        [[nodiscard]] const View *getView(ViewHandle handle) const noexcept;
        [[nodiscard]] size_t activeViewCount() const noexcept;

        void forEachActiveView(auto &&fn)
        {
            rebuildActiveViewCacheIfNeeded();
            for (auto *view : active_views_dense_)
                fn(*view);
        }

        void forEachActiveView(auto &&fn) const
        {
            rebuildActiveViewCacheIfNeeded();
            for (auto *view : active_views_dense_)
                fn(*view);
        }

        void endViewFrame(uint32_t frame_slot);

        // ── Domain-neutral per-frame primitives (general; no domain knowledge) ──
        // The monotonic frame serial of the frame currently being recorded.
        [[nodiscard]] uint64_t frameSerial() const noexcept { return current_stamp_.serial; }

        /// Per-frame "instance cull-enable mask" GPU address (buffer-device-address;
        /// 0 = none). A general primitive the GPU-driven mesh cull reads to skip
        /// disabled instances — the core does NOT know who produces it. A feature
        /// (e.g. SpatialCullFeature) sets it each frame in onFrameBegin; it is reset
        /// to 0 at the start of every beginFrame, so a scene with no provider gets 0
        /// (= every instance active). This is the decoupling seam that keeps the core
        /// free of SpatialCullGrid / large-world concepts.
        void setInstanceCullMaskAddress(uint64_t addr) noexcept { instance_cull_mask_addr_ = addr; }
        [[nodiscard]] uint64_t instanceCullMaskAddress() const noexcept { return instance_cull_mask_addr_; }

        // ── View lifecycle (all View state is driven by Scene) ──────────

        /// Allocate per-view slot in SceneResources::ViewBuffer.
        void initViewUBO(View &view);

        /// Free per-view slot in SceneResources::ViewBuffer.
        void destroyViewUBO(View &view);

        /// Resize a view's render extent.
        void resizeView(View &view, common::Size2D new_extent);

        /// Reset per-frame state on a view.
        void endViewFrame(View &view);

        // ================================================================
        //  Per-Frame Lifecycle (render thread)
        // ================================================================

        void beginFrame(const FrameStamp &stamp);

        /// Record scene-local uploads for this tick (instance/point-cloud/trajectory/scene buffers).
        void recordUploads(VkCommandBuffer cmd, const FrameStamp &stamp);

        /// Convenience entry — forwards to recordUploads().
        void record(VkCommandBuffer cmd, const FrameStamp &stamp) { recordUploads(cmd, stamp); }

        // (updateView removed — View 去 3D 化: per-view camera data is a feature domain
        // now. StandardViewCamera fills ViewCameraResource + the View's neutral GPU
        // staging directly; the core scene no longer names camera matrices / frustum
        // and no longer broadcasts onFrustumUpdated.)

        void endFrame(uint64_t frame_id);
        // ================================================================
        //  Render-Graph / Pass Infrastructure
        // ================================================================
        MaterialPipeline&           materialPipeline() noexcept;
        RGVulkanResourceAllocator&  graphAllocator() noexcept;
        RGVulkanRecorder&           graphRecorder() noexcept;

        /// Access the (possibly invalid) scene-level compiled graph.
        [[nodiscard]] SceneGraphState& graphState() noexcept;
        [[nodiscard]] const SceneGraphState& graphState() const noexcept;

        /**
         * @brief Compile (or recompile) the scene-level render graph from a target layout.
         *
         * Collects addPasses() from all enabled features, produces an
         * RGCompiledGraph, and updates graph_state_.
         * Called by Renderer or by external code when the graph is
         * invalidated (feature change, resize, etc.).
         *
         * The layout describes which slots (SceneColor, SceneDepth, …) the target
         * provides and their format/usage properties.  The slot descriptors are baked
         * into the compiled graph; actual per-frame VkImage handles are supplied
         * through RenderRequest::target at record time.
         */
        void compileGraphTemplate(const RenderTargetLayout &layout);

        /// Invalidate all view resource states.  Called after graph recompilation
        /// so each view re-allocates resources on next render.
        void invalidateAllViewResources(const RGGraphDescription* source_graph) noexcept;

        /// Defer destruction of a previous per-view RG resource state.
        /// Used when resizing or swapping view resources while old GPU work may still be in flight.
        void retireViewResourceState(RGResourceState &&state, const RGGraphDescription* source_graph);

        // ================================================================
        //  Scene Data
        // ================================================================
        RenderContext &renderContext() noexcept;
        const RenderContext &renderContext() const noexcept;

        /// SoA slot index for this scene's entry in SceneResources::SceneGlobalBuffer.
        [[nodiscard]] SlotHandle sceneGlobalSlot() const noexcept { return scene_global_slot_; }
        [[nodiscard]] FrameRetireScheduler::OwnerToken retireOwnerToken() const noexcept
        {
            return retire_owner_token_;
        }

        // ================================================================
        //  Primitive Registry
        // ================================================================

        // ================================================================
        //  Scene Resource Registry
        // ================================================================

        [[nodiscard]] SceneResourceRegistry &sceneRegistry() noexcept { return scene_registry_; }
        [[nodiscard]] const SceneResourceRegistry &sceneRegistry() const noexcept { return scene_registry_; }

        /// Per-scene growable descriptor-pool chain backing this scene's
        /// persistent descriptor sets (light/scene/instance/vertex-pool/shadow/
        /// skinning). Layouts stay global; only set allocation is per-scene.
        [[nodiscard]] SceneDescriptorArena &descriptorArena() noexcept { return scene_descriptor_arena_; }
        /// Per-scene transfer scheduler — a feature registers its per-scene SSBO as a
        /// contributor (makeTransferContributor) so it flushes before draw. Used by
        /// LightFeature for its LightResources (the scene ctor no longer knows light).
        [[nodiscard]] TransferScheduler &transferScheduler() noexcept { return transfer_scheduler_; }
        [[nodiscard]] RenderObjectExtractor &renderObjectExtractor() noexcept { return render_object_extractor_; }
        [[nodiscard]] const RenderObjectExtractor &renderObjectExtractor() const noexcept { return render_object_extractor_; }

    private:
        RenderObjectExtractor render_object_extractor_{};
        void rebuildActiveViewCacheIfNeeded() const;
        void rebuildEnabledFeatureCacheIfNeeded() const;
        void markViewCacheDirty() noexcept { view_cache_dirty_ = true; }
        void markFeatureCacheDirty() noexcept
        {
            feature_cache_dirty_ = true;
        }

        FeatureHandle addFeatureImpl(std::unique_ptr<RenderFeature> feature, uint32_t extractor_type_id);

        // Core feature removal. check_reverse_deps=true (public removeFeature) refuses
        // to remove a feature another installed feature still requires; =false
        // (removeAllFeatures / shutdownFull) bypasses the guard for bulk teardown. (三-4)
        bool removeFeatureInternal(FeatureHandle feature_id, bool check_reverse_deps);

        // Set for the duration of a factory create_fn call (see FeatureInstallScope):
        // addFeatureImpl applies it to the feature BEFORE initAndAttachTo so the
        // descriptor is visible during attach. Null outside an install. (三-2)
        const FeatureDescriptor* pending_install_descriptor_{nullptr};

        // ── Per-(feature, view) state ownership (truth source) ───────────────
        // Records which (feature, view) pairs have had allocateViewState()
        // succeed, so deallocateViewState() runs exactly once — symmetrically on
        // view removal AND feature removal — instead of the old enabled-only,
        // never-on-feature-removal scheme that leaked. Keyed by feature_id; the
        // inner set holds the view ids that feature currently owns state for.
        // (Lifecycle events are rare and feature/view counts small, so a plain
        // map-of-set is fine; not a per-frame hot path.) See ensure/release below.
        std::unordered_map<uint32_t, std::unordered_set<uint32_t>> feature_view_states_;

        /// Allocate this feature's state for @p view_index if not already present.
        /// Idempotent. Returns false if allocateViewState() failed (nothing recorded).
        /// Keyed by the view's index (== ViewHandle::index); the generation is a
        /// scene-level concern validated at the views_ lookup boundary, so the
        /// feature-facing per-view id stays a bare uint32.
        bool ensureFeatureViewState(RenderFeature& feature, uint32_t view_index);

        /// Release this feature's state for @p view_index if present. Idempotent;
        /// calls deallocateViewState() exactly once for a recorded pair.
        void releaseFeatureViewState(RenderFeature& feature, uint32_t view_index) noexcept;

        struct RetiredGraph
        {
            std::unique_ptr<RGCompiledGraph> graph;
            uint64_t retire_frame{0};
        };

        /// Deferred per-view resource cleanup (physical images + record context).
        struct RetiredViewResources
        {
            std::unique_ptr<RGResourceState> state;
            uint64_t retire_frame{0};
            /// Graph description that was used to allocate these resources.
            /// Needed to return resources to pool via deallocateToPool().
            /// Lifetime safety: retired_view_resources_ is always processed BEFORE
            /// retired_graphs_ in endFrame(), so source_graph remains valid.
            const RGGraphDescription* source_graph{nullptr};
        };

        struct PendingDestroy
        {
            ViewHandle view_id;
            uint64_t   destroy_frame{0};
        };

        using FeatureSet = lux::cxx::SlotKeyAutoSparseSet<FeatureHandle, std::unique_ptr<RenderFeature>>;
        using ViewSet = lux::cxx::SlotKeyAutoSparseSet<ViewHandle, std::unique_ptr<View>>;

        std::shared_ptr<RenderContext> render_ctx_;
        Config config_;
        PipelineConfig pipeline_config_;

        // ── Per-scene descriptor-pool chain (growable; backs all per-scene
        //    persistent descriptor sets). Declared BEFORE scene_registry_ so it
        //    is destroyed AFTER it during member destruction (reverse order). ──
        SceneDescriptorArena scene_descriptor_arena_;

        // ── Per-scene resource registry ──────────────────────────────────────
        SceneResourceRegistry scene_registry_;

        FeatureSet features_;
        ViewSet views_;

        std::deque<PendingDestroy> pending_destroys_;
        MaterialPipeline material_pipeline_;
        std::unique_ptr<RGVulkanResourceAllocator> allocator_;
        std::unique_ptr<RGVulkanRecorder> recorder_;
        SceneGraphState graph_state_;
        SlotHandle scene_global_slot_{}; ///< SceneResources scene SoA slot
        FrameRetireScheduler::OwnerToken retire_owner_token_{0};

        // ── Deferred graph destruction ──────────────────────────────────

        std::deque<RetiredGraph> retired_graphs_;
        std::deque<RetiredViewResources> retired_view_resources_;

        // ── Unified transfer scheduler for scene-level uploads ──────────
        TransferScheduler transfer_scheduler_;

        // ── Stored scene-global time (set via SceneCtrlSetSceneTime) ────
        float scene_time_{0.0f};
        float scene_delta_time_{0.0f};
        uint64_t scene_total_frames_{0};
        FrameStamp current_stamp_{};
        // Domain-neutral instance cull-enable mask GPU address for the current
        // frame (0 = none). Set by a provider feature in onFrameBegin, reset to 0
        // at beginFrame start. See setInstanceCullMaskAddress().
        uint64_t   instance_cull_mask_addr_{0};

        bool initialized_{false};
        ERenderPath render_path_{ERenderPath::Forward};
        std::string debug_name_;

        mutable std::vector<View *> active_views_dense_{};
        mutable std::vector<RenderFeature *> enabled_features_dense_{};
        mutable bool view_cache_dirty_{true};
        mutable bool feature_cache_dirty_{true};
        bool recompile_suppressed_{false};
    };

    template <typename Fn>
    inline void forEachEnabledFeature(
        const std::vector<std::unique_ptr<RenderFeature>> &features, Fn &&fn)
    {
        for (auto &f : features)
            if (f && f->isEnabled())
                fn(*f);
    }

    template <typename Fn>
    inline void forEachFeature(
        const std::vector<std::unique_ptr<RenderFeature>> &features, Fn &&fn)
    {
        for (auto &f : features)
            if (f)
                fn(*f);
    }

} // namespace lux::render
