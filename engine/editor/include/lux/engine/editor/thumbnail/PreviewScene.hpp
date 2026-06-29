#pragma once
/**
 * @file PreviewScene.hpp
 * @brief Minimal resident offscreen scene used to render asset thumbnails.
 *
 * One `PreviewScene` is created (once) per editor process and reused for every
 * 3D thumbnail (meshes, material balls, …) — the editor-side analogue of UE's
 * `FPreviewScene`. It owns a dedicated render scene + ONE offscreen view (no
 * swapchain) + the minimal forward-lit feature set + a single key light + a
 * resident unit-sphere mesh (the material-preview ball).
 *
 * Architecture notes (see asset-pipeline-m4 plan):
 *   - It does NOT use the ECS `World` / `RenderableSystem`. A single static
 *     preview instance only needs the low-level session API; the ECS path is
 *     for dynamic scenes and is reserved for skeletal previews (deferred).
 *   - The server renders EVERY offscreen view each tick, so this scene
 *     coexists with the editor's main scene without an active-scene swap.
 *   - `setup()` and `capture()` BLOCK (they drive their own frames via the
 *     session). They must therefore run where no editor frame is being
 *     recorded — at startup, or at the editor loop's between-frames boundary.
 *     A non-blocking, frame-driven service can be layered on top later.
 */

#include <lux/engine/render/comm/client/RenderSession.hpp>
#include <lux/engine/render/renderer/features/light/LightOperation.hpp>  // LightOperationIds / LightProxy
#include <lux/engine/render/renderer/features/meshstack/MeshStackOperation.hpp>  // MeshStackOperationIds / MeshStackProxy
#include <lux/engine/render/renderer/features/material/MaterialOperation.hpp>  // MaterialOperationIds / MaterialProxy
#include <lux/engine/render/renderer/features/view_camera/ViewCameraOperation.hpp>  // ViewCameraOperationIds / ViewCameraProxy
#include <lux/engine/math/AABB.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace lux::rdesc  { struct ShaderInfo; }
namespace lux::render { struct GraphMaterialData; }
namespace lux::ui     { class  ImGuiProxy; }
namespace lux::exec   { class  EngineExecutor; }

namespace lux::editor
{
    class PreviewScene
    {
    public:
        struct CameraMatrices
        {
            float view[16];
            float proj[16];
            float eye[3];
        };

        // Defined in the .cpp (out-of-line) so the unique_ptr<Live> pimpl member
        // can be destroyed where `Live` is complete.
        //
        // @p exec drives the live-preview content swap as a sender pipeline on the
        // EngineExecutor (C4). It is OPTIONAL: only the editor's live material
        // preview passes one; the ThumbnailService's capture-only PreviewScene
        // passes nullptr (it never calls setGraphContent/tick → never swaps).
        explicit PreviewScene(lux::render::RenderSession& session,
                              lux::exec::EngineExecutor*  exec = nullptr) noexcept;

        PreviewScene(const PreviewScene&)            = delete;
        PreviewScene& operator=(const PreviewScene&) = delete;
        ~PreviewScene();

        /// Blocking, one-time bring-up: scene + offscreen view + forward-lit
        /// features + key light + resident sphere mesh. @p render_size is the
        /// square edge of the offscreen target (thumbnails are produced at or
        /// below this; larger requests are clamped to it). Returns false on
        /// failure (the scene is then unusable; callers should skip thumbnails).
        ///
        /// @p display_proxy selects the VIEW channel:
        ///   - nullptr  → a plain offscreen view (base addView). Required for the
        ///     readback/capture path (ThumbnailService): readback only searches
        ///     the base offscreen-view pool.
        ///   - non-null → a UI offscreen view (ImGuiProxy::addUIView), registered
        ///     in the UI server's scene-view index so a SceneViewElement sentinel
        ///     (the LIVE material preview) resolves to the rendered image. Without
        ///     this the sentinel resolves to VK_NULL_HANDLE and the pane shows
        ///     uninitialized GPU memory (noise).
        [[nodiscard]] bool setup(std::uint32_t       render_size   = 256,
                                 lux::ui::ImGuiProxy* display_proxy = nullptr);

        [[nodiscard]] bool ready() const noexcept { return ready_; }
        [[nodiscard]] lux::render::RenderSceneId sceneId() const noexcept { return scene_id_; }
        [[nodiscard]] lux::render::ViewHandle    view()    const noexcept { return view_; }
        /// Feature-scoped mesh-stack ops for this preview scene (for MeshStackProxy).
        /// ThumbnailService addresses its instance commands through these.
        [[nodiscard]] lux::render::MeshStackOperationIds meshStackOps() const noexcept { return mesh_stack_ops_; }
        /// Feature-scoped material ops for this preview scene (for MaterialProxy).
        /// ThumbnailService addresses its material upload/destroy through these.
        [[nodiscard]] lux::render::MaterialOperationIds materialOps() const noexcept { return material_ops_; }
        /// Feature-scoped per-view camera ops for this preview scene (for ViewCameraProxy).
        [[nodiscard]] lux::render::ViewCameraOperationIds viewCameraOps() const noexcept { return view_camera_ops_; }
        [[nodiscard]] std::uint32_t              renderSize() const noexcept { return render_size_; }
        [[nodiscard]] lux::render::RMeshHandle   sphereMesh() const noexcept { return sphere_mesh_; }
        /// Resident neutral-grey PBR material — used to preview a bare mesh that
        /// carries no material of its own.
        [[nodiscard]] lux::render::RMaterialHandle defaultMaterial() const noexcept { return default_material_; }

        /// Compute a look-at + perspective camera that frames @p bounds.
        [[nodiscard]] CameraMatrices frameBounds(const lux::math::AABB& bounds) const;

        /// One mesh + material to render in a capture (a model may have several).
        struct PreviewInstance
        {
            lux::render::RMeshHandle     mesh{};
            lux::render::RMaterialHandle material{};
        };

        /// Blocking capture: add @p instances as temporary instances, frame
        /// them with @p bounds, render @p warmup_frames, read back the offscreen
        /// color, then remove the instances. Returns tightly-packed BGRA8
        /// (renderSize × renderSize), or empty on failure.
        ///
        /// Contract: assumes NO frame is being recorded on entry and leaves
        /// NONE open on exit (safe at the editor's between-frames boundary).
        [[nodiscard]] std::vector<std::byte> capture(
            std::span<const PreviewInstance> instances,
            const lux::math::AABB&           bounds,
            std::uint32_t                    warmup_frames = 6);

        /// Convenience single-instance overload.
        [[nodiscard]] std::vector<std::byte> capture(
            lux::render::RMeshHandle     mesh,
            lux::render::RMaterialHandle material,
            const lux::math::AABB&       bounds,
            std::uint32_t                warmup_frames = 6)
        {
            const PreviewInstance one{mesh, material};
            return capture(std::span<const PreviewInstance>{&one, 1}, bounds, warmup_frames);
        }

        // ── Live preview (non-blocking, frame-OPEN tick) ─────────────────────
        // A second usage mode of the same managed scene: drive ONE live
        // material-graph preview (forward graph-frag override) + an orbit camera.
        // setGraphContent/updateGraphParams/orbit/requestResize are RECORD-ONLY
        // (safe from a frame-CLOSED ImGui pass); tick() does ALL the session I/O.

        /// Set/replace the live graph material (a compiled FORWARD frag + params).
        /// Recorded; applied across the next tick()s (compile -> feature swap ->
        /// upload -> instance). Re-callable on each material edit.
        void setGraphContent(std::span<const std::uint32_t>        spirv,
                             const lux::rdesc::ShaderInfo&         info,
                             const lux::render::GraphMaterialData& params);

        /// Cheap param-only update (no shader/feature swap) — recorded, pushed as
        /// modifyGraphMaterial by tick().
        void updateGraphParams(const lux::render::GraphMaterialData& params);

        /// Accumulate an orbit-camera delta (yaw rad, pitch rad, zoom factor).
        void orbit(float d_yaw, float d_pitch, float d_zoom);

        /// Record a new offscreen target size (applied by tick() via resizeView).
        void requestResize(std::uint32_t width, std::uint32_t height);

        /// Frame-OPEN, non-blocking. Spawns the content-swap sender pipeline (when a
        /// new graph is pending and none is in flight), applies a param-only refresh,
        /// a pending resize, and pushes the orbit camera. The swap itself now
        /// progresses via the EngineExecutor's drainMain (C4) rather than a per-tick
        /// switch. MUST run with the host's render frame OPEN.
        void tick();

        /// Drain the in-flight content swap to quiescence. MUST be called while the
        /// session can still pumpReplies and the executor is alive (before either is
        /// torn down) — the swap's asSender stages are not cancellable, so a parked
        /// swap left on the executor's scope would hang its on_empty() at shutdown.
        /// No-op for a capture-only (null-executor) PreviewScene.
        void shutdown();

        /// Tear down the resident GPU resources (scene/view/sphere/material/light)
        /// INSIDE a frame. MUST be called while the render thread is still serving
        /// (e.g. before the editor stops it) — the destructor runs after the thread
        /// joins, where RenderClient::builder() would assert (no live frame). Idempotent.
        void releaseGpu();

    private:
        // Spawns the content-swap pipeline on exec_ (single-flight). Defined in .cpp
        // (returns/uses stdexec senders). endSwap() clears the in-flight flag.
        void startSwap();
        void endSwap();

        lux::render::RenderSession*  session_{nullptr};
        lux::exec::EngineExecutor*   exec_{nullptr};   ///< live-swap pipeline host (may be null)
        lux::render::RenderSceneId   scene_id_{};
        lux::render::ViewHandle      view_{};
        lux::render::RMeshHandle     sphere_mesh_{};
        lux::render::RMaterialHandle default_material_{};
        lux::render::RLightHandle    key_light_{};   ///< resident per-scene key light (Plan A)
        lux::render::LightOperationIds light_ops_{}; ///< feature-scoped light ops (for LightProxy create/destroy)
        lux::render::MeshStackOperationIds mesh_stack_ops_{}; ///< feature-scoped mesh-stack ops (for MeshStackProxy)
        lux::render::MaterialOperationIds material_ops_{};   ///< feature-scoped material ops (for MaterialProxy)
        lux::render::ViewCameraOperationIds view_camera_ops_{}; ///< feature-scoped per-view camera ops (for ViewCameraProxy)
        std::uint32_t                render_size_{256};
        bool                         ready_{false};

        // Forward feature handle/type — kept so the live path can swap the
        // graph-frag override (removeFeature + addFeature). Inert for thumbnails.
        std::uint32_t                forward_type_id_{0};
        lux::render::FeatureHandle   forward_feature_{};

        // Live single-content preview state (null until setup()); defined in the
        // .cpp to keep the RenderRequest menagerie out of this header.
        struct Live;
        std::unique_ptr<Live>        live_;
    };

} // namespace lux::editor
