#pragma once
#include <lux/engine/ui/Root.hpp>
#include <lux/engine/editor/ui/ComponentEditors.hpp>
#include <lux/engine/editor/ui/SpatialInteraction.hpp>
#include <lux/engine/ui/Element.hpp>
#include <lux/engine/scene/Camera.hpp>
#include <lux/engine/scene/MeshQuery.hpp>
#include <lux/engine/simulation/ecs/Transform.hpp>
#include <lux/engine/editor/storage/ProjectStorage.hpp>
#include <lux/engine/editor/editing/scene/FieldEdit.hpp>
#include <lux/engine/editor/metadata/SceneRegistrations.hpp>
#include <lux/engine/function/render/client/core/RenderSceneId.hpp>
#include <lux/engine/object/LuxObject.hpp>
#include <lux/engine/resource/asset/model/ModelAsset.hpp>
#include <lux/engine/world/WorldObjectId.hpp>
#include <lux/engine/editor/scene/SceneEditor.hpp>

namespace lux::editor::scene
{
    class LUX_EDITOR_SCENE_PUBLIC SceneEditorTestAccess final
    {
    public:
        explicit SceneEditorTestAccess(SceneEditor& tool) noexcept : tool_(tool) {}
        [[nodiscard]] EditorResult<void> selectRenderSystem(lux::system::SystemInstanceId);
        [[nodiscard]] lux::system::SystemInstanceId selectedRenderSystem() const noexcept;
        [[nodiscard]] double runCoordinatePageSize() const noexcept;
        [[nodiscard]] SelectionNotice selection() const noexcept;
        [[nodiscard]] EditorResult<void> select(lux::simulation::ecs::Entity);
        [[nodiscard]] lux::scene::SceneInstanceId instance() const noexcept;
        [[nodiscard]] lux::scene::QueryResult<bool> raycastNearest(
            lux::scene::SceneInstanceId,
            const lux::math::Ray3d&,
            double maximum_distance,
            lux::scene::RayHit3D&,
            lux::scene::MeshQueryWork* = nullptr
        ) const;
        [[nodiscard]] EditorResult<lux::simulation::ecs::Entity> viewportCamera();
        [[nodiscard]] EditorResult<void> setWorkPlaneHeight(double height);
        [[nodiscard]] EditorResult<void>
        navigateCamera(lux::simulation::ecs::Entity, const lux::simulation::ecs::Transform3D&, const lux::scene::Camera&);
        [[nodiscard]] editing::EditResult<lux::simulation::ecs::Entity> createCameraFromView(
            lux::simulation::ecs::Entity,
            editing::StateId,
            lux::partition::PartitionOrdinal
        );
        [[nodiscard]] const void* component(lux::simulation::ecs::Entity, lux::cxx::TypeToken) const noexcept;
        [[nodiscard]] std::uint64_t componentVersion(lux::simulation::ecs::Entity, lux::cxx::TypeToken) const noexcept;
        [[nodiscard]] editing::EditResult<SceneWriteTarget> writeTarget(lux::simulation::ecs::Entity) const noexcept;
        [[nodiscard]] bool supportsObjectSpace(EObjectSpace) const noexcept;
        [[nodiscard]] bool supportsHierarchy() const noexcept;
        [[nodiscard]] editing::EditResult<lux::simulation::ecs::Entity> createObject(
            editing::StateId,
            lux::partition::PartitionOrdinal,
            EObjectSpace
        );
        [[nodiscard]] editing::EditResult<
            editing::ApplyResult> eraseObjects(editing::StateId, std::span<const lux::simulation::ecs::Entity>);
        [[nodiscard]] editing::EditResult<editing::ApplyResult> reparent(
            SceneWriteTarget,
            lux::simulation::ecs::Entity
        );
        [[nodiscard]] editing::EditResult<std::vector<lux::simulation::ecs::Entity>> createEntitiesFromModel(
            editing::StateId,
            const lux::asset::ModelAsset&,
            const Eigen::Vector3d&,
            lux::partition::PartitionOrdinal
        );
        [[nodiscard]] std::size_t partitionCount() const noexcept;
        [[nodiscard]] EditorResult<ModelCreationId> requestModelCreation(
            AssetReference,
            const Eigen::Vector3d&,
            lux::partition::PartitionOrdinal
        );
        [[nodiscard]] EditorResult<VModelCreationStatus> modelCreationStatus(ModelCreationId) const;
        [[nodiscard]] EditorResult<void> retryModelCreation(ModelCreationId, editing::StateId);
        [[nodiscard]] EditorResult<void> cancelModelCreation(ModelCreationId);
        [[nodiscard]] EditorResult<void> acknowledgeModelCreation(ModelCreationId);
        [[nodiscard]] SceneEditing& editing() const noexcept;
        [[nodiscard]] editing::EditHistory& history() const noexcept;
        [[nodiscard]] bool fieldEditWritable(const FieldEditToken&) const noexcept;
        [[nodiscard]] editing::EditResult<void> fieldEdited(const FieldEditToken&);
        [[nodiscard]] editing::EditResult<editing::ApplyResult> finishFieldEdit(const FieldEditToken&);
        [[nodiscard]] editing::EditResult<void> finishFieldEdits();
        [[nodiscard]] std::shared_ptr<const SceneResourceSnapshot> resources() const noexcept;
        [[nodiscard]] EditorResult<void> retryResource(const lux::scene::RenderAssetKey&);
        [[nodiscard]] const ProjectStorage& project() const noexcept;
        [[nodiscard]] ProjectStorage& project() noexcept;
        [[nodiscard]] std::string diagnostic() const;
        [[nodiscard]] EditorResult<lux::render::RenderSceneId> renderScene() const;
        [[nodiscard]] double coordinatePageSize() const noexcept;
        template <class Component, class Value, class Access>
        [[nodiscard]] editing::EditResult<editing::ApplyResult> setField(
            SceneWriteTarget target,
            std::string_view field,
            std::string_view label,
            Access access,
            const Value& value
        )
        {
            return editing().setField<Component, Value>(target, field, label, std::move(access), value);
        }

        template <class Component, class Value, class Access>
        [[nodiscard]] editing::EditResult<FieldEditToken> beginFieldEdit(
            SceneWriteTarget target,
            std::string origin,
            std::string_view field,
            std::string_view label,
            Access access
        )
        {
            return editing()
                .beginFieldEdit<Component, Value>(target, std::move(origin), field, label, std::move(access));
        }

    private:
        SceneEditor& tool_;
    };
}
inline auto toolTest(lux::editor::scene::SceneEditor& tool) noexcept
{
    return lux::editor::scene::SceneEditorTestAccess{tool};
}
inline auto toolTest(const lux::editor::scene::SceneEditor& tool) noexcept
{
    return lux::editor::scene::SceneEditorTestAccess{const_cast<lux::editor::scene::SceneEditor&>(tool)};
}
