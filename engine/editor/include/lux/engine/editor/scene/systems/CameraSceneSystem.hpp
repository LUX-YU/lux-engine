#pragma once
/**
 * @file CameraSceneSystem.hpp
 * @brief Editor scene system: the UE-style viewport camera.
 *
 * Owns the UEEditorController and the per-frame camera → render-view push,
 * formerly hardcoded inline in EditorScene::tick. Two phases bracket World::tick:
 *   onPreWorldTick         — lazy-build + tick the controller (writes the camera
 *                            entity's TransformComponent from input) + auto-fit the
 *                            projection aspect, BEFORE World::tick recomputes
 *                            view/proj.
 *   onPostRenderableUpdate — push the freshly-computed view/proj/eye to the render
 *                            view (ViewCameraProxy), AFTER World::tick.
 *
 * wantsCursorCapture() is forwarded by EditorScene::cameraWantsCursorCapture so
 * the host can flip GLFW into captured-cursor mode while flying. The host keeps a
 * non-owning pointer to this system for exactly that forward.
 */

#include <lux/engine/editor/scene/EditorSceneSystem.hpp>
#include <lux/engine/render/renderer/features/view_camera/ViewCameraOperation.hpp>  // ViewCameraOperationIds

#include <memory>

namespace lux::gameplay::d3 { class UEEditorController; }

namespace lux::editor
{
    class CameraSceneSystem final : public EditorSceneSystem
    {
    public:
        CameraSceneSystem();
        ~CameraSceneSystem() override;   // out-of-line: UEEditorController is fwd-declared

        void onPreWorldTick(const SceneTickContext& ctx) override;
        void onPostRenderableUpdate(const SceneTickContext& ctx) override;

        /// True only while fly-mode is active (RMB held) — the host captures the
        /// cursor on this. Safe before the controller is built (returns false).
        [[nodiscard]] bool wantsCursorCapture() const noexcept;

    private:
        std::unique_ptr<lux::gameplay::d3::UEEditorController> controller_;
        lux::render::ViewCameraOperationIds                view_ops_{};
        bool                                               view_ops_resolved_{false};
    };

} // namespace lux::editor
