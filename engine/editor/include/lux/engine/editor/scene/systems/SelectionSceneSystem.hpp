#pragma once
/**
 * @file SelectionSceneSystem.hpp
 * @brief Editor scene system: selection highlight publish + the per-frame gizmo
 *        / debug line-list upload.
 *
 * Both halves were hardcoded inline in EditorScene::tick. They bracket the
 * renderable-update anchor:
 *   onPreRenderableUpdate  — publish the selected object (root + descendant mesh
 *                            entities) as the render highlight set, so the mesh
 *                            adapters fold kInstanceFlagHighlight into the
 *                            per-instance flags THIS frame (BEFORE the bridge).
 *   onPostRenderableUpdate — THE editor's single line-list upload per frame
 *                            (script debug lines). The transient buffer is
 *                            last-writer-wins, so this is the one place any line
 *                            source must merge; an empty upload clears it.
 */

#include <lux/engine/editor/scene/EditorSceneSystem.hpp>

#include <memory>

namespace lux::render { class LineListProxy; }

namespace lux::editor
{
    class SelectionSceneSystem final : public EditorSceneSystem
    {
    public:
        SelectionSceneSystem();
        ~SelectionSceneSystem() override;   // out-of-line: LineListProxy is fwd-declared

        void onPreRenderableUpdate(const SceneTickContext& ctx) override;
        void onPostRenderableUpdate(const SceneTickContext& ctx) override;

    private:
        std::unique_ptr<lux::render::LineListProxy> line_list_proxy_;
    };

} // namespace lux::editor
