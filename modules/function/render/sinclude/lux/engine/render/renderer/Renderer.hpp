#pragma once

#include <lux/engine/function/visibility.h>
#include <lux/engine/render/renderer/FrameContext.hpp>
#include <lux/engine/render/FrameStamp.hpp>
#include <lux/engine/render/scene/RenderScene.hpp>
#include <lux/engine/render/transfer/TransferScheduler.hpp>
#include <lux/engine/render/core/RenderSceneId.hpp>
#include <lux/engine/render/core/Errors.hpp>
#include <lux/engine/render/renderer/FeatureTypeRegistry.hpp>   // sunk feature-type registry (阶段 3)
#include <lux/cxx/container/SparseSet.hpp>
#include <lux/cxx/container/BasicSparseSet.hpp>   // SlotKeyAutoSparseSet (generational)

#include <cstdint>
#include <memory>
#include <vector>

namespace lux::render
{

    class RenderContext;
    class View;
    struct SceneGraphState;

    /// Result of creating a scene via Renderer::addScene().
    struct AddSceneResult
    {
        RenderSceneId scene_id{};
        std::vector<ViewHandle> view_handles;
    };

    // ─────────────────────────────────────────────────────────────────────
    //  Renderer — owns all RenderScene instances + scene lifecycle queue
    // ─────────────────────────────────────────────────────────────────────

    /// @brief Top-level renderer — scene owner and frame orchestrator.
    ///
    ///   - addScene: blocking creation (direct render-thread call)
    ///   - beginFrame / render / endFrame: frame orchestration
    ///
    /// Communication is handled entirely by RenderServer (comm layer).
    class LUX_FUNCTION_PUBLIC Renderer
    {
    public:
        explicit Renderer(std::shared_ptr<RenderContext> ctx);
        ~Renderer();

        Renderer(const Renderer &) = delete;
        Renderer &operator=(const Renderer &) = delete;

        // ================================================================
        //  Scene lifecycle
        // ================================================================

        /// Blocking convenience: create a scene directly on the render thread.
        [[nodiscard]] AddSceneResult addScene(RenderScene::Config config);

        /// Remove a scene by id (render thread).
        void removeScene(RenderSceneId scene_id);

        // ================================================================
        //  Frame lifecycle (render thread)
        // ================================================================

        /// Advance per-frame state across all owned scenes.
        void beginFrame(const FrameStamp& stamp);

        /// Run all CPU→GPU transfers and uploads for the current tick.
        /// Must be called exactly once per tick, after beginFrame() and
        /// before any renderSingleView() calls.
        void runUploadPhase(VkCommandBuffer cmd);

        /// Records global uploads once per tick.
        void record(VkCommandBuffer cmd, const FrameStamp& stamp);

        /// Conclude the frame for all scenes.
        void endFrame(uint64_t frame_serial);

        /// Render a single view of a scene to a specific target binding.
        /// Use when multiple views of the same scene need different targets.
        /// The binding fully describes the target — including the layout the graph
        /// compiles against (binding.layout); a binding without a layout is skipped.
        /// @param cross_view_index  0 for the first view of a scene in the
        ///        current frame; >0 for subsequent views.  The recorder uses
        ///        this to patch imported-resource first-touch barriers.
        void renderSingleView(
            RenderScene &scene, View &view,
            const RenderTargetBinding &binding,
            FrameRuntime &rt,
            uint32_t cross_view_index = 0);

        // ================================================================
        //  Accessors
        // ================================================================

        [[nodiscard]] RenderContext &renderContext() noexcept;
        [[nodiscard]] uint32_t framesInFlight() const noexcept;

        /// Direct access to a scene by ID (render thread only).
        [[nodiscard]] RenderScene *getScene(RenderSceneId id) noexcept;

        /// Registry of feature TYPES (factories + descriptors). The comm layer
        /// registers types into it; dependency resolution resolves declared deps
        /// against it (FeatureTypeRegistry::findByStableType). Render thread only.
        [[nodiscard]] FeatureTypeRegistry &featureTypeRegistry() noexcept { return feature_type_registry_; }

        /// Iterate all live scenes (render thread only).
        template<typename Fn>
        void forEachScene(Fn&& fn)
        {
            for (auto& scene_ptr : scenes_.values())
            {
                if (scene_ptr)
                    fn(*scene_ptr);
            }
        }

        template<typename Fn>
        void forEachScene(Fn&& fn) const
        {
            for (const auto& scene_ptr : scenes_.values())
            {
                if (scene_ptr)
                    fn(*scene_ptr);
            }
        }

    private:
        /// Ensure graph is compiled + sized, upload pending CPU→GPU data.
        /// Returns false if rendering should be skipped (graph invalid, etc.).
        bool prepareSceneForRender(
            RenderScene &scene, const RenderTargetLayout &layout);

        /// Ensure the view's per-view GPU resources are allocated and correctly sized.
        /// Returns false if allocation failed (rendering should be skipped).
        bool prepareViewForRender(
            RenderScene &scene, View &view,
            const RenderTargetBinding &binding, FrameRuntime &rt);

        /// Record graph for a single view.
        void renderView(RenderScene &scene, SceneGraphState &gs, View &view,
                        const DrawRequest &req, FrameRuntime &rt,
                        uint32_t cross_view_index = 0);

        /// Reclaim scenes retired via removeScene() whose GPU work has fully
        /// drained (frame_serial - retire_serial >= fif). Called once per endFrame().
        void collectRetiredScenes(uint64_t frame_serial);

        FrameStamp current_stamp_{}; ///< Authoritative stamp set by beginFrame().

        std::shared_ptr<RenderContext> ctx_;
        // ── Global transfer scheduler ──
        TransferScheduler global_transfer_scheduler_;
        bool global_scheduler_initialized_{false};
        // MeshResources + MaterialResources are built LAZILY now (StandardMeshStack /
        // StandardMaterial attach, or first upload) — they may appear AFTER the
        // scheduler was initialized, so their transfer contributors are registered
        // the frame each first exists, not only at scheduler init (Texture is
        // init-time, registered eagerly).
        bool mesh_contributor_added_{false};
        bool material_contributor_added_{false};
        // ── Feature-type registry (sunk from comm Impl, 阶段 3) ──────
        FeatureTypeRegistry feature_type_registry_;

        // ── Scene storage (generational: rejects stale RenderSceneId) ──
        lux::cxx::SlotKeyAutoSparseSet<RenderSceneId, std::unique_ptr<RenderScene>> scenes_;

        // ── Async scene retirement ──────────────────────────────────
        // removeScene() does NOT stall the whole device (waitIdle). It moves the
        // scene here, tagged with the serial of the last frame that could still
        // reference its GPU resources; endFrame() reclaims it once the GPU has
        // drained that frame (frame_serial - retire_serial >= fif). This mirrors
        // the per-view / per-graph deferred-destroy already used inside RenderScene
        // and keeps destroying one scene from blocking every other scene's frames.
        struct RetiredScene
        {
            std::unique_ptr<RenderScene> scene;
            uint64_t                     retire_serial{0};
        };
        std::vector<RetiredScene> retired_scenes_;
    };

} // namespace lux::render
