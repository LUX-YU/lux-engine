#include "SceneEditorTestAccess.hpp"
#include <lux/engine/editor/scene/detail/SceneEditorImpl.hpp>

namespace lux::editor::scene
{
    bool SceneEditorTestAccess::persistencePending() const noexcept
    {
        return tool_.impl_->persistence_.pending();
    }
    editing::EditHistory& SceneEditorTestAccess::history() const noexcept
    {
        return tool_.impl_->inspectedHistory();
    }
    EditorResult<void> SceneEditorTestAccess::selectRenderSystem(lux::system::SystemInstanceId id)
    {
        return tool_.impl_->selectRenderSystem(id);
    }
    lux::system::SystemInstanceId SceneEditorTestAccess::selectedRenderSystem() const noexcept
    {
        return tool_.impl_->viewport_system_;
    }

    SelectionNotice SceneEditorTestAccess::selection() const noexcept
    {
        return tool_.impl_->selection();
    }

    EditorResult<void> SceneEditorTestAccess::select(lux::simulation::ecs::Entity id)
    {
        return tool_.impl_->select(std::move(id));
    }

    lux::scene::SceneInstanceId SceneEditorTestAccess::instance() const noexcept
    {
        return tool_.impl_->instance();
    }

    lux::scene::QueryResult<bool> SceneEditorTestAccess::raycastNearest(
        lux::scene::SceneInstanceId instance,
        const lux::math::Ray3d& ray,
        double maximum_distance,
        lux::scene::RayHit3D& hit,
        lux::scene::MeshQueryWork* work
    ) const
    {
        return tool_.impl_->raycastNearest(std::move(instance), ray, std::move(maximum_distance), hit, work);
    }

    EditorResult<lux::simulation::ecs::Entity> SceneEditorTestAccess::viewportCamera()
    {
        return tool_.impl_->viewportCamera();
    }

    EditorResult<void> SceneEditorTestAccess::setWorkPlaneHeight(double height)
    {
        return tool_.impl_->setWorkPlaneHeight(std::move(height));
    }

    EditorResult<void> SceneEditorTestAccess::navigateCamera(
        lux::simulation::ecs::Entity ref,
        const lux::simulation::ecs::Transform3D& pose,
        const lux::scene::Camera& projection
    )
    {
        return tool_.impl_->navigateCamera(std::move(ref), pose, projection);
    }

    editing::EditResult<lux::simulation::ecs::Entity> SceneEditorTestAccess::createCameraFromView(
        lux::simulation::ecs::Entity source,
        editing::StateId base,
        lux::partition::PartitionOrdinal partition
    )
    {
        return tool_.impl_->createCameraFromView(std::move(source), std::move(base), std::move(partition));
    }

    const void* SceneEditorTestAccess::component(lux::simulation::ecs::Entity object, lux::cxx::TypeToken type)
        const noexcept
    {
        return tool_.impl_->component(std::move(object), std::move(type));
    }

    std::uint64_t SceneEditorTestAccess::componentVersion(lux::simulation::ecs::Entity object, lux::cxx::TypeToken type)
        const noexcept
    {
        return tool_.impl_->componentVersion(std::move(object), std::move(type));
    }

    editing::EditResult<SceneWriteTarget> SceneEditorTestAccess::writeTarget(lux::simulation::ecs::Entity object
    ) const noexcept
    {
        return tool_.impl_->writeTarget(std::move(object));
    }

    bool SceneEditorTestAccess::supportsObjectSpace(EObjectSpace space) const noexcept
    {
        return tool_.impl_->supportsObjectSpace(std::move(space));
    }

    bool SceneEditorTestAccess::supportsHierarchy() const noexcept
    {
        return tool_.impl_->supportsHierarchy();
    }

    editing::EditResult<lux::simulation::ecs::Entity> SceneEditorTestAccess::createObject(
        editing::StateId base,
        lux::partition::PartitionOrdinal partition,
        EObjectSpace space
    )
    {
        return tool_.impl_->createObject(std::move(base), std::move(partition), std::move(space));
    }

    editing::EditResult<editing::ApplyResult> SceneEditorTestAccess::eraseObjects(
        editing::StateId base,
        std::span<const lux::simulation::ecs::Entity> input
    )
    {
        return tool_.impl_->eraseObjects(std::move(base), std::move(input));
    }

    editing::EditResult<editing::ApplyResult> SceneEditorTestAccess::reparent(
        SceneWriteTarget target,
        lux::simulation::ecs::Entity parent
    )
    {
        return tool_.impl_->reparent(std::move(target), std::move(parent));
    }

    editing::EditResult<std::vector<lux::simulation::ecs::Entity>> SceneEditorTestAccess::createEntitiesFromModel(
        editing::StateId base,
        const lux::asset::ModelAsset& asset,
        const Eigen::Vector3d& position,
        lux::partition::PartitionOrdinal partition
    )
    {
        return tool_.impl_->createEntitiesFromModel(std::move(base), asset, position, std::move(partition));
    }

    std::size_t SceneEditorTestAccess::partitionCount() const noexcept
    {
        return tool_.impl_->partitionCount();
    }

    EditorResult<ModelCreationId> SceneEditorTestAccess::requestModelCreation(
        AssetReference reference,
        const Eigen::Vector3d& position,
        lux::partition::PartitionOrdinal partition
    )
    {
        return tool_.impl_->requestModelCreation(std::move(reference), position, std::move(partition));
    }

    EditorResult<VModelCreationStatus> SceneEditorTestAccess::modelCreationStatus(ModelCreationId id) const
    {
        return tool_.impl_->modelCreationStatus(std::move(id));
    }

    EditorResult<void> SceneEditorTestAccess::retryModelCreation(ModelCreationId id, editing::StateId base)
    {
        return tool_.impl_->retryModelCreation(std::move(id), std::move(base));
    }

    EditorResult<void> SceneEditorTestAccess::cancelModelCreation(ModelCreationId id)
    {
        return tool_.impl_->cancelModelCreation(std::move(id));
    }

    EditorResult<void> SceneEditorTestAccess::acknowledgeModelCreation(ModelCreationId id)
    {
        return tool_.impl_->acknowledgeModelCreation(std::move(id));
    }

    SceneEditing& SceneEditorTestAccess::editing() const noexcept
    {
        return tool_.impl_->editing();
    }

    bool SceneEditorTestAccess::fieldEditWritable(const FieldEditToken& token) const noexcept
    {
        return tool_.impl_->fieldEditWritable(token);
    }

    editing::EditResult<void> SceneEditorTestAccess::fieldEdited(const FieldEditToken& token)
    {
        return tool_.impl_->fieldEdited(token);
    }

    editing::EditResult<editing::ApplyResult> SceneEditorTestAccess::finishFieldEdit(const FieldEditToken& token)
    {
        return tool_.impl_->finishFieldEdit(token);
    }

    editing::EditResult<void> SceneEditorTestAccess::finishFieldEdits()
    {
        return tool_.impl_->finishFieldEdits();
    }

    std::shared_ptr<const SceneResourceSnapshot> SceneEditorTestAccess::resources() const noexcept
    {
        return tool_.impl_->resources();
    }

    EditorResult<void> SceneEditorTestAccess::retryResource(const lux::scene::RenderAssetKey& key)
    {
        return tool_.impl_->retryResource(key);
    }

    const ProjectStorage& SceneEditorTestAccess::project() const noexcept
    {
        return tool_.impl_->project();
    }

    ProjectStorage& SceneEditorTestAccess::project() noexcept
    {
        return tool_.impl_->project();
    }

    std::string SceneEditorTestAccess::diagnostic() const
    {
        return tool_.impl_->diagnostic();
    }

    EditorResult<lux::render::RenderSceneId> SceneEditorTestAccess::renderScene() const
    {
        return tool_.impl_->renderScene();
    }

    double SceneEditorTestAccess::coordinatePageSize() const noexcept
    {
        return tool_.impl_->coordinatePageSize();
    }

    double SceneEditorTestAccess::runCoordinatePageSize() const noexcept
    {
        return tool_.impl_->runCoordinatePageSize();
    }
}
