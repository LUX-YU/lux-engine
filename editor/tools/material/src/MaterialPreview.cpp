#include <lux/engine/EngineContext.hpp>
#include <lux/engine/editor/material/MaterialEditorImpl.hpp>
namespace lux::editor::material
{
    void MaterialEditor::Impl::createPreview()
    {
        if (preview_)
            return;
        const auto& registrations = editor_context_.sceneRegistrations();
        MaterialPreviewEnvironment environment;
        environment.scene.components = registrations.components;
        environment.scene.simulation_systems = registrations.simulation_systems;
        environment.scene.scene_systems = registrations.scene_systems;
        environment.scene.render_bindings = registrations.render_bindings;
        environment.scene.renderer = &editor_context_.renderRuntime();
        environment.scene.resources = &editor_context_.renderResources();
        environment.features = registrations.features;
        preview_ =
            std::make_unique<MaterialPreviewStore>(editor_context_.engine().sceneRuntime(), std::move(environment));
    }
    EditorResult<lux::scene::RenderAssetInput> MaterialEditor::Impl::capturePreviewAssets()
    {
        auto reads = project().captureAssetReads();
        if (!reads)
            return lux::cxx::unexpected(reads.error());
        const auto identity = project().reference({});
        return lux::scene::RenderAssetInput{
            {identity.project_instance, 0},
            identity.catalog_revision,
            std::move(*reads)
        };
    }
    void MaterialEditor::Impl::maintainPreview() noexcept
    {
        if (!preview_)
            return;
        MaterialCompileKey desired{{}, {}, 1, preview_->target()};
        if (history_)
        {
            auto view = history_->view();
            if (view)
                desired.content.state = view->snapshot.current;
        }
        preview_->setDesired(desired);
        preview_->update();
    }
    EditorResult<void> MaterialEditor::Impl::resetPreview()
    {
        if (!preview_)
            return {};
        auto base = capturePreviewAssets();
        if (!base)
            return lux::cxx::unexpected(base.error());
        auto result = preview_->reset(std::move(*base));
        if (!result)
            return lux::cxx::unexpected(transition::MaterialCompilationAccess::failure(result.error()));
        return {};
    }
    lux::simulation::ecs::Entity MaterialEditor::Impl::previewCamera() const noexcept
    {
        return preview_ ? preview_->camera() : lux::simulation::ecs::NullEntity;
    }
    lux::scene::SceneInstanceId MaterialEditor::Impl::previewInstance() const noexcept
    {
        return preview_ ? preview_->instance() : lux::scene::SceneInstanceId{};
    }
    std::string MaterialEditor::Impl::previewStatus() const
    {
        if (!preview_)
            return "Not compiled";
        const auto status = preview_->status();
        if (!status.diagnostic.empty())
            return status.diagnostic;
        if (status.prepared)
            return "Preparing compiled material...";
        if (status.stale)
            return "Recompile to update preview";
        return status.accepted ? "Static preview" : "Not compiled";
    }
    EditorResult<void> MaterialEditor::Impl::navigatePreview(
        const simulation::ecs::Transform3D& pose,
        const lux::scene::Camera& camera
    )
    {
        if (!preview_)
            return lux::cxx::unexpected(EditorFailure{EEditorError::BUSY, "preview.camera"});
        auto result = preview_->navigate(pose, camera);
        if (!result)
            return lux::cxx::unexpected(transition::MaterialCompilationAccess::failure(result.error()));
        return {};
    }
}
