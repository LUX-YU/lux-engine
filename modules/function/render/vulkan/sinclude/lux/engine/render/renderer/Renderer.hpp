#pragma once

#include <lux/engine/function/visibility.h>
#include <lux/engine/render/renderer/FrameContext.hpp>
#include <lux/engine/function/render/client/core/FrameStamp.hpp>
#include <lux/engine/render/scene/RenderScene.hpp>
#include <lux/engine/function/render/client/core/RenderSceneId.hpp>
#include <lux/engine/function/render/client/core/Errors.hpp>
#include <lux/engine/render/renderer/FeatureTypeRegistry.hpp> // sunk feature-type registry
#include <lux/cxx/container/SparseSet.hpp>
#include <lux/cxx/container/BasicSparseSet.hpp> // SlotKeyAutoSparseSet (generational)

#include <cstdint>
#include <chrono>
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

        Renderer(const Renderer&) = delete;
        Renderer& operator=(const Renderer&) = delete;

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

        /// Feed the fence-proven GPU completion watermark (FrameDriver::
        /// gpuCompletedSerial) once per tick, BEFORE beginFrame. All deferred
        /// reclamation (retired scenes, DeferredDestroyQueue, retire scheduler)
        /// gates on this instead of "serial - fif" arithmetic — serials advance
        /// on non-submitting ticks, so arithmetic over-claims completion.
        void setGpuCompletedSerial(uint64_t serial) noexcept
        {
            gpu_completed_serial_ = serial;
            has_gpu_completed_serial_ = true;
        }

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
            RenderScene& scene,
            View& view,
            const RenderTargetBinding& binding,
            FrameRuntime& rt,
            uint32_t cross_view_index = 0
        );

        // ================================================================
        //  Accessors
        // ================================================================

        [[nodiscard]] RenderContext& renderContext() noexcept;
        [[nodiscard]] uint32_t framesInFlight() const noexcept;

        /// Direct access to a scene by ID (render thread only).
        [[nodiscard]] RenderScene* getScene(RenderSceneId id) noexcept;

        /// Registry of feature TYPES (factories + descriptors). The comm layer
        /// registers types into it; dependency resolution resolves declared deps
        /// against it (FeatureTypeRegistry::findByStableType). Render thread only.
        [[nodiscard]] FeatureTypeRegistry& featureTypeRegistry() noexcept
        {
            return feature_type_registry_;
        }

        /// Iterate all live scenes (render thread only).
        template <typename Fn> void forEachScene(Fn&& fn)
        {
            for (auto& scene_ptr : scenes_.values())
            {
                if (scene_ptr)
                    fn(*scene_ptr);
            }
        }

        template <typename Fn> void forEachScene(Fn&& fn) const
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
        bool prepareSceneForRender(RenderScene& scene, const RenderTargetLayout& layout);

        /// Ensure the view's per-view GPU resources are allocated and correctly sized.
        /// Returns false if allocation failed (rendering should be skipped).
        bool prepareViewForRender(RenderScene& scene, View& view, const RenderTargetBinding& binding, FrameRuntime& rt);

        /// Record graph for a single view.
        void renderView(
            RenderScene& scene,
            SceneGraphState& gs,
            View& view,
            const DrawRequest& req,
            FrameRuntime& rt,
            uint32_t cross_view_index = 0
        );

        /// Reclaim scenes retired via removeScene() whose GPU work is
        /// FENCE-PROVEN drained (retire_serial <= the completion watermark fed
        /// via setGpuCompletedSerial; driverless fallback = serial arithmetic).
        /// Called once per endFrame().
        void collectRetiredScenes(uint64_t frame_serial);

        FrameStamp current_stamp_{}; ///< Authoritative stamp set by beginFrame().
        std::chrono::steady_clock::time_point scene_clock_start_{std::chrono::steady_clock::now()};
        std::chrono::steady_clock::time_point scene_clock_previous_{scene_clock_start_};

        std::shared_ptr<RenderContext> ctx_;
        // ── Feature-type registry (sunk from comm Impl) ──────
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
            uint64_t retire_serial{0};
        };
        std::vector<RetiredScene> retired_scenes_;

        // Fence-proven completion watermark (setGpuCompletedSerial). The bool
        // gates the driverless fallback: headless no-driver ticks never submit
        // GPU work, so the old arithmetic is vacuously safe there.
        uint64_t gpu_completed_serial_{0};
        bool has_gpu_completed_serial_{false};

        [[nodiscard]] uint64_t completedSerialOr(uint64_t fallback) const noexcept
        {
            return has_gpu_completed_serial_ ? gpu_completed_serial_ : fallback;
        }
    };

} // namespace lux::render
