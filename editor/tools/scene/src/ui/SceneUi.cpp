#include <lux/engine/ui/Root.hpp>
#include <lux/engine/editor/scene/detail/SceneEditorImpl.hpp>
#include <lux/engine/editor/ui/scene/InspectorPane.hpp>
#include <lux/engine/editor/ui/scene/OutlinerPane.hpp>
#include <lux/engine/editor/ui/scene/ResourcePane.hpp>
#include <lux/engine/editor/ui/scene/SceneContentElement.hpp>

namespace lux::editor::scene
{
    void SceneEditor::Impl::createContent(
        EditorResult<void>& status,
        assets::AssetImporter& importer,
        std::span<const ui::SpatialInteractionRegistration> viewports
    )
    {
        content_ = std::make_unique<ui::SceneContentElement>(
            *this,
            runtime_,
            source,
            this->editor_context_.renderResources(),
            "scene-content",
            status,
            viewports
        );
        editor->setContent(*content_);
        const auto prefix = std::string(editor->id().name()) + "/";
        inspector_ = std::make_unique<ui::InspectorPane>(
            *editor,
            lux::ui::PaneId{prefix + "inspector"},
            editor_context_.sceneRegistrations().components,
            this->editor_context_.componentEditors(),
            status,
            &editor_context_.project().catalogModel()
        );
        outliner_ = std::make_unique<ui::OutlinerPane>(*this, runtime_, lux::ui::PaneId{prefix + "outliner"}, status);
        resources_ = std::make_unique<ui::ResourcePane>(*this, importer, lux::ui::PaneId{prefix + "resources"}, status);
        if (!status)
            return;
        if (editor_context_.panes().findFirst(lux::ui::PaneTypeIdView{kSceneEditorType}) == nullptr)
            editor->root().setDockLayout(
                {std::string(outliner_->id().name()),
                 std::string(editor->id().name()),
                 std::string(inspector_->id().name()),
                 std::string(resources_->id().name()),
                 260,
                 350,
                 200,
                 editor->root().findPane(lux::ui::PaneIdView{"project"}) ? "project" : ""}
            );
        syncInspector();
    }

    EditorResult<void> SceneEditor::Impl::finishContentEditing()
    {
        if (inspector_)
            if (auto finished = inspector_->finishEditing(); !finished)
                return finished;
        const auto finished = finishFieldEdits();
        if (!finished)
            return lux::cxx::unexpected(EditorFailure{EEditorError::BUSY, "scene.field", 0, {}, finished.error()});
        return {};
    }

    void SceneEditor::Impl::syncInspector()
    {
        if (!inspector_ || !scene_editing || replacing_)
            return;
        const bool stopping = this->run_status.state == EPlaybackState::STOPPING;
        const auto result = stopping ? inspector_->content().setTarget(*this->scene_editing, this->selection_.object)
                                     : inspector_->content().setTarget(editing(), selection().object);
        if (!result)
            this->failure = result.error();
    }

}
