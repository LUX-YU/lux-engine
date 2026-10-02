#pragma once
#include <lux/engine/editor/editing/EditTypes.hpp>
#include <lux/engine/resource/asset/model/ModelAsset.hpp>
#include <lux/engine/simulation/ecs/Transform.hpp>
#include <lux/engine/simulation/ecs/Visual.hpp>
#include <lux/engine/world/WorldObjectId.hpp>
#include <lux/engine/scene/Camera.hpp>
#include <optional>
#include <lux/engine/editor/scene/SceneSource.hpp>
#include <lux/engine/simulation/ecs/WorldEntityMap.hpp>

namespace lux::editor::scene
{
    [[nodiscard]] editing::EditResult<SceneObjectData> makeSceneObject(
        world::WorldObjectId, partition::PartitionOrdinal, EObjectSpace, bool hierarchy,
        const simulation::ecs::ComponentSchemaSet&
    );
    struct SceneModelObject final
    {
        world::WorldObjectId object, parent;
        simulation::ecs::Transform3D transform;
        std::optional<simulation::ecs::Mesh3D> mesh;
    };
    [[nodiscard]] editing::EditResult<std::vector<SceneModelObject>> expandSceneModel(
        const asset::ModelAsset& asset,
        const Eigen::Vector3d& position,
        bool hierarchy
    );

    // Shared author codec entry for formal content producers.
    template <class Component>
    [[nodiscard]] editing::EditResult<SceneComponentData> encodeSceneValue(
        const Component& value,
        const simulation::ecs::ComponentSchemaSet& schemas,
        const simulation::ecs::WorldEntityMap& identities,
        std::size_t byte_limit
    )
    {
        const auto* schema = schemas.find(lux::cxx::typeToken<Component>());
        if (!schema || !schema->decode_value || !schema->capture_value)
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::UNSUPPORTED_OPERATION));
        auto encoded = schema->capture_value(&value, schema->code_lifetime).encode(identities, byte_limit);
        if (!encoded)
            return lux::cxx::unexpected(editing::makeEditFailure(
                editing::EEditError::PRECONDITION_FAILED,
                static_cast<std::uint64_t>(ESceneStructureError::CODEC_FAILURE),
                schema->id.name
            ));
        return SceneComponentData{schema->id, schema->version, std::move(*encoded)};
    }

    [[nodiscard]] editing::EditResult<std::vector<SceneObjectData>> makeSceneModelObjects(
        const asset::ModelAsset& asset,
        const Eigen::Vector3d& position,
        partition::PartitionOrdinal partition,
        const world::WorldDescription& world,
        const simulation::ecs::ComponentSchemaSet& schemas
    );
    [[nodiscard]] editing::EditResult<SceneObjectData> makeSceneCameraObject(
        world::WorldObjectId object,
        partition::PartitionOrdinal partition,
        const lux::scene::Camera& camera,
        const simulation::ecs::Transform3D& transform,
        const simulation::ecs::ComponentSchemaSet& schemas
    );

    // Walk stable author identities, independent of Registry/runtime/selection ownership.
    template <class ParentOf>
    [[nodiscard]] bool createsParentCycle(
        world::WorldObjectId object,
        world::WorldObjectId parent,
        std::size_t object_count,
        ParentOf&& parent_of
    ) noexcept
    {
        std::size_t visited{};
        while (parent.valid())
        {
            if (parent == object || ++visited > object_count)
                return true;
            parent = parent_of(parent);
        }
        return false;
    }
}
