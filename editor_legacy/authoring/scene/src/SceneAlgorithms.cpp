#include <lux/engine/editor/scene/AuthoringFacts.hpp>
#include <lux/engine/editor/scene/SceneAlgorithms.hpp>
#include <random>
#include <unordered_map>
#include <lux/engine/simulation/ecs/Parent.hpp>

namespace lux::editor::scene
{
    editing::EditResult<SceneObjectData> makeSceneObject(
        world::WorldObjectId id,
        partition::PartitionOrdinal partition,
        EObjectSpace space,
        bool hierarchy,
        const simulation::ecs::ComponentSchemaSet& schemas
    )
    {
        namespace ecs = simulation::ecs;
        const bool is_valid_space = space == EObjectSpace::NONE || space == EObjectSpace::SPACE_2D ||
            space == EObjectSpace::SPACE_3D;
        const bool is_invalid_request = !id.valid() || !is_valid_space;
        if (is_invalid_request)
            return cxx::unexpected(editing::makeEditFailure(editing::EEditError::INVALID_ARGUMENT));
        try
        {
            SceneObjectData result{id, partition, {}};
            const ecs::WorldEntityMap identities;
            const auto append = [&](const auto& value) -> editing::EditResult<void> {
                auto encoded = encodeSceneValue(value, schemas, identities, 16 * 1024 * 1024);
                if (!encoded)
                    return cxx::unexpected(encoded.error());
                result.components.push_back(std::move(*encoded));
                return {};
            };
            editing::EditResult<void> encoded;
            if (space == EObjectSpace::SPACE_2D)
                encoded = append(ecs::Transform2D{});
            else if (space == EObjectSpace::SPACE_3D)
                encoded = append(ecs::Transform3D{});
            if (encoded && hierarchy)
                encoded = append(ecs::Parent{ecs::NullEntity});
            if (!encoded)
                return cxx::unexpected(encoded.error());
            return result;
        }
        catch (const std::bad_alloc&)
        {
            std::terminate();
        }
        catch (...)
        {
            return cxx::unexpected(editing::makeEditFailure(
                editing::EEditError::PRECONDITION_FAILED,
                static_cast<std::uint64_t>(ESceneStructureError::CODEC_FAILURE),
                "Object default codec failed"
            ));
        }
    }
    editing::EditResult<std::vector<SceneModelObject>> expandSceneModel(
        const asset::ModelAsset& asset,
        const Eigen::Vector3d& position,
        bool hierarchy
    )
    {
        namespace ecs = simulation::ecs;
        const auto rejected = [](EModelCreationError code, std::string_view message) {
            return lux::cxx::unexpected(editing::makeEditFailure(
                editing::EEditError::PRECONDITION_FAILED,
                static_cast<std::uint64_t>(code),
                message
            ));
        };
        if (!position.allFinite())
        {
            return rejected(EModelCreationError::INVALID_TRANSFORM, "The placement position must be finite");
        }
        const auto& model = asset.data();
        if (model.skeleton || !model.animations.empty())
        {
            return rejected(
                EModelCreationError::UNSUPPORTED_DEFORMATION,
                "Skinned model placement requires a deformation author provider"
            );
        }
        if (model.nodes.size() > 4096 || model.primitives.size() > 4096)
        {
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::STAGING_LIMIT));
        }
        std::random_device entropy;
        std::seed_seq seed{entropy(), entropy(), entropy(), entropy(), entropy(), entropy(), entropy(), entropy()};
        std::mt19937 random(seed);
        uuids::uuid_random_generator generate(random);
        std::vector<SceneModelObject> objects;
        struct Mesh
        {
            world::WorldObjectId object;
            ecs::Mesh3D value;
        };
        std::vector<Mesh> meshes;
        std::vector<lux::world::WorldObjectId> groups(model.nodes.size());
        std::vector<std::uint32_t> parents(model.nodes.size(), UINT32_MAX);
        std::vector<Eigen::Affine3d> transforms(model.nodes.size(), Eigen::Affine3d::Identity());
        const auto decompose = [](const Eigen::Affine3d& matrix, ecs::Transform3D& value) {
            value.translation = matrix.translation();
            Eigen::Matrix3d rotation = matrix.linear();
            for (int column = 0; column < 3; ++column)
            {
                value.scale[column] = rotation.col(column).norm();
                if (value.scale[column] <= 1e-12)
                {
                    return false;
                }
                rotation.col(column) /= value.scale[column];
            }
            if (rotation.determinant() < 0)
            {
                rotation.col(0) *= -1;
                value.scale[0] *= -1;
            }
            if (!(rotation.transpose() * rotation).isApprox(Eigen::Matrix3d::Identity(), 1e-5))
            {
                return false;
            }
            value.rotation = Eigen::Quaterniond(rotation).normalized();
            return value.translation.allFinite() && value.scale.allFinite() && value.rotation.coeffs().allFinite();
        };
        objects.reserve(std::min<std::size_t>(4096, model.nodes.size() + model.primitives.size()));
        meshes.reserve(model.primitives.size());
        const auto append = [&](lux::world::WorldObjectId parent, const Eigen::Affine3d& matrix) {
            SceneModelObject object{{generate()}, parent, {}, {}};
            if (!decompose(matrix, object.transform))
            {
                return false;
            }
            objects.push_back(std::move(object));
            return true;
        };
        // ModelAsset validates a rooted tree whose children follow their parents.
        for (std::size_t index{}; index < model.nodes.size(); ++index)
        {
            const auto& node = model.nodes[index];
            Eigen::Affine3d local = node.local_transform.cast<double>();
            if (index == model.root_node)
            {
                local.translation() += position;
            }
            const auto parent_index = parents[index];
            transforms[index] = parent_index == UINT32_MAX ? local : transforms[parent_index] * local;
            for (const auto child : node.children)
            {
                parents[child] = static_cast<std::uint32_t>(index);
            }
            if (hierarchy)
            {
                const auto parent = parent_index == UINT32_MAX ? lux::world::WorldObjectId{} : groups[parent_index];
                if (!append(parent, local))
                {
                    return rejected(
                        EModelCreationError::NON_TRS_TRANSFORM,
                        "A model node contains shear or a singular transform"
                    );
                }
                groups[index] = objects.back().object;
            }
            for (const auto primitive_index : node.primitives)
            {
                const auto& primitive = model.primitives[primitive_index];
                if (hierarchy && node.primitives.size() == 1)
                {
                    meshes.push_back({groups[index], {{primitive.mesh, primitive.material}}});
                    continue;
                }
                if (!append(
                        hierarchy ? groups[index] : lux::world::WorldObjectId{},
                        hierarchy ? Eigen::Affine3d::Identity() : transforms[index]
                    ))
                {
                    return rejected(
                        EModelCreationError::NON_TRS_TRANSFORM,
                        "This flat Scene cannot represent the composed model transform"
                    );
                }
                meshes.push_back({objects.back().object, {{primitive.mesh, primitive.material}}});
            }
        }
        if (objects.empty() || objects.size() > 4096)
        {
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::STAGING_LIMIT));
        }

        std::unordered_map<world::WorldObjectId, std::size_t, world::WorldObjectIdHash> positions;
        for (std::size_t index{}; index < objects.size(); ++index)
            positions.emplace(objects[index].object, index);
        for (const auto& mesh : meshes)
            objects[positions.at(mesh.object)].mesh = mesh.value;
        return objects;
    }

    editing::EditResult<std::vector<SceneObjectData>> makeSceneModelObjects(
        const asset::ModelAsset& asset,
        const Eigen::Vector3d& position,
        partition::PartitionOrdinal partition,
        const world::WorldDescription& world,
        const simulation::ecs::ComponentSchemaSet& schemas
    )
    {
        namespace ecs = simulation::ecs;
        const auto supports = [&]<class Component>() {
            const auto* schema = schemas.find(lux::cxx::typeToken<Component>());
            if (!schema || !schema->capture_value)
                return false;
            const std::string_view names[]{schema->id.name};
            return queryApplicability(authoringFacts(world, schemas), {names, false, true}).supported();
        };
        const bool is_unsupported =
            !supports.template operator()<ecs::Transform3D>() || !supports.template operator()<ecs::Mesh3D>();
        if (is_unsupported)
            return lux::cxx::unexpected(editing::makeEditFailure(
                editing::EEditError::PRECONDITION_FAILED,
                static_cast<std::uint64_t>(EModelCreationError::UNSUPPORTED_SCENE),
                "This Scene does not declare 3D transform and mesh author schemas"
            ));
        if (partition.value >= world.partitionCount())
            return lux::cxx::unexpected(editing::makeEditFailure(
                editing::EEditError::PRECONDITION_FAILED,
                static_cast<std::uint64_t>(EModelCreationError::INVALID_PARTITION),
                "Choose an existing World partition"
            ));
        const bool hierarchy = supports.template operator()<ecs::Parent>();
        auto expanded = expandSceneModel(asset, position, hierarchy);
        if (!expanded)
            return lux::cxx::unexpected(expanded.error());
        ecs::Registry registry;
        ecs::WorldEntityMap identities;
        for (const auto& object : *expanded)
            if (!identities.bind(object.object, registry.create()))
                std::terminate();
        std::vector<SceneObjectData> result;
        result.reserve(expanded->size());
        for (const auto& object : *expanded)
        {
            SceneObjectData content{object.object, partition, {}};
            const auto append = [&](const auto& value) -> editing::EditResult<void> {
                auto encoded = encodeSceneValue(value, schemas, identities, 16 * 1024 * 1024);
                if (!encoded)
                    return lux::cxx::unexpected(encoded.error());
                content.components.push_back(std::move(*encoded));
                return {};
            };
            auto encoded = append(object.transform);
            if (encoded && hierarchy)
                encoded = append(ecs::Parent{identities.entity(object.parent)});
            if (encoded && object.mesh)
                encoded = append(*object.mesh);
            if (!encoded)
                return lux::cxx::unexpected(encoded.error());
            result.push_back(std::move(content));
        }
        return result;
    }

    editing::EditResult<SceneObjectData> makeSceneCameraObject(
        world::WorldObjectId id,
        partition::PartitionOrdinal partition,
        const lux::scene::Camera& camera,
        const simulation::ecs::Transform3D& transform,
        const simulation::ecs::ComponentSchemaSet& schemas
    )
    {
        if (!id.valid() || !lux::scene::cameraProjection(camera, 1.0))
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::INVALID_ARGUMENT));
        const simulation::ecs::WorldEntityMap identities;
        auto spatial = encodeSceneValue(transform, schemas, identities, 16 * 1024 * 1024);
        if (!spatial)
            return lux::cxx::unexpected(spatial.error());
        auto encoded = encodeSceneValue(camera, schemas, identities, 16 * 1024 * 1024);
        if (!encoded)
            return lux::cxx::unexpected(encoded.error());
        SceneObjectData result{id, partition, {}};
        result.components.push_back(std::move(*spatial));
        result.components.push_back(std::move(*encoded));
        return result;
    }
}
