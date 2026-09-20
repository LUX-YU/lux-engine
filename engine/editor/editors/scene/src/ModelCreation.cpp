#include <lux/engine/editor/scene/detail/ModelCreation.hpp>
#include <lux/engine/resource/asset/material/MaterialAssets.hpp>
#include <lux/engine/resource/asset/mesh/MeshAsset.hpp>
#include <lux/engine/simulation/ecs/EntityCreationPlan.hpp>
#include <lux/engine/simulation/ecs/Parent.hpp>
#include <lux/engine/simulation/ecs/Visual.hpp>
#include <random>
#include <unordered_map>

namespace lux::editor::scene::detail
{
    namespace
    {
        struct ModelTransform final
        {
            lux::world::WorldObjectId object, parent;
            lux::simulation::ecs::Transform3D value;
            std::string label;
        };

        struct ModelMesh final
        {
            lux::world::WorldObjectId object;
            lux::simulation::ecs::Mesh3D value;
        };
    } // namespace

    editing::EditResult<std::vector<ObjectContent>>
    prepareModelCreation(const SceneObjects &owner, const Project &project, const lux::asset::ModelAsset &asset,
                         const Eigen::Vector3d &position, lux::partition::PartitionOrdinal partition)
    {
        namespace ecs = lux::simulation::ecs;
        const auto rejected = [](EModelCreationError code, std::string_view message)
        {
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::PRECONDITION_FAILED,
                                                                 static_cast<std::uint64_t>(code), message));
        };
        if (!position.allFinite())
        {
            return rejected(EModelCreationError::INVALID_TRANSFORM, "The placement position must be finite");
        }
        const auto &world = owner.source.world->data();
        const auto supports = [&]<class Component>()
        {
            const auto *schema = owner.metadata.getComponentMeta(lux::cxx::typeToken<Component>());
            return schema && schema->capture &&
                   std::ranges::find(world.schemas(), schema->id.name, &lux::world::WorldDataSchemaId::name) !=
                       world.schemas().end();
        };
        if (!supports.template operator()<ecs::Transform3D>() || !supports.template operator()<ecs::Mesh3D>())
        {
            return rejected(EModelCreationError::UNSUPPORTED_SCENE,
                            "This Scene does not declare 3D transform and mesh author schemas");
        }
        if (partition.value >= owner.source.partitions.size())
        {
            return rejected(EModelCreationError::INVALID_PARTITION, "Choose an existing World partition");
        }
        const auto &model = asset.data();
        if (model.skeleton || !model.animations.empty())
        {
            return rejected(EModelCreationError::UNSUPPORTED_DEFORMATION,
                            "Skinned model placement requires a deformation author provider");
        }
        if (model.nodes.size() > 4096 || model.primitives.size() > 4096)
        {
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::STAGING_LIMIT));
        }
        for (const auto &primitive : model.primitives)
        {
            const auto mesh =
                project.resolveReference(project.reference(primitive.mesh), lux::asset::MeshAsset::primary_magic);
            const auto material = project.resolveReference(project.reference(primitive.material),
                                                           lux::asset::MaterialAsset::primary_magic);
            if (!mesh || !material)
            {
                return rejected(EModelCreationError::MISSING_DEPENDENCY,
                                "The model references a mesh or material outside the "
                                "project catalog");
            }
        }
        const bool hierarchy = supports.template operator()<ecs::Parent>();
        std::random_device entropy;
        std::seed_seq seed{entropy(), entropy(), entropy(), entropy(), entropy(), entropy(), entropy(), entropy()};
        std::mt19937 random(seed);
        uuids::uuid_random_generator generate(random);
        std::vector<ModelTransform> objects;
        std::vector<ModelMesh> meshes;
        std::vector<lux::world::WorldObjectId> groups(model.nodes.size());
        std::vector<std::uint32_t> parents(model.nodes.size(), UINT32_MAX);
        std::vector<Eigen::Affine3d> transforms(model.nodes.size(), Eigen::Affine3d::Identity());
        const auto decompose = [](const Eigen::Affine3d &matrix, ecs::Transform3D &value)
        {
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
        const auto append = [&](lux::world::WorldObjectId parent, const Eigen::Affine3d &matrix, std::string label)
        {
            ModelTransform object{{generate()}, parent, {}, std::move(label)};
            if (!decompose(matrix, object.value))
            {
                return false;
            }
            objects.push_back(std::move(object));
            return true;
        };
        // ModelAsset validates a rooted tree whose children follow their parents.
        for (std::size_t index{}; index < model.nodes.size(); ++index)
        {
            const auto &node = model.nodes[index];
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
                if (!append(parent, local, "Model node " + std::to_string(index + 1)))
                {
                    return rejected(EModelCreationError::NON_TRS_TRANSFORM,
                                    "A model node contains shear or a singular transform");
                }
                groups[index] = objects.back().object;
            }
            for (const auto primitive_index : node.primitives)
            {
                const auto &primitive = model.primitives[primitive_index];
                if (hierarchy && node.primitives.size() == 1)
                {
                    meshes.push_back({groups[index], {{primitive.mesh, primitive.material}}});
                    continue;
                }
                if (!append(hierarchy ? groups[index] : lux::world::WorldObjectId{},
                            hierarchy ? Eigen::Affine3d::Identity() : transforms[index],
                            "Mesh " + std::to_string(primitive_index + 1)))
                {
                    return rejected(EModelCreationError::NON_TRS_TRANSFORM,
                                    "This flat Scene cannot represent the composed model transform");
                }
                meshes.push_back({objects.back().object, {{primitive.mesh, primitive.material}}});
            }
        }
        if (objects.empty() || objects.size() > 4096)
        {
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::STAGING_LIMIT));
        }
        auto planned = ecs::planEntityCreation(owner.registry, objects.size());
        if (!planned)
        {
            return lux::cxx::unexpected(editing::makeEditFailure(editing::EEditError::ID_EXHAUSTED));
        }
        ecs::WorldEntityMap identities;
        identities.reserve(objects.size());
        for (std::size_t index{}; index < objects.size(); ++index)
        {
            if (!identities.bind(objects[index].object, planned->entities()[index]))
            {
                return rejected(EModelCreationError::NON_TRS_TRANSFORM, "Duplicate model identity");
            }
        }
        std::unordered_map<lux::world::WorldObjectId, const ModelMesh *, lux::world::WorldObjectIdHash> mesh_by_object;
        mesh_by_object.reserve(meshes.size());
        for (const auto &mesh : meshes)
        {
            mesh_by_object.emplace(mesh.object, &mesh);
        }
        std::vector<detail::ObjectContent> content;
        content.reserve(objects.size());
        for (auto &object : objects)
        {
            detail::ObjectContent captured{{object.object, object.parent, std::move(object.label), partition}, {}};
            const auto append = [&](const auto &value) -> editing::EditResult<void>
            {
                auto encoded = owner.encodeComponent(value, identities);
                if (!encoded)
                {
                    return lux::cxx::unexpected(encoded.error());
                }
                captured.components.push_back(std::move(*encoded));
                return {};
            };
            auto encoded = append(object.value);
            if (encoded && hierarchy)
            {
                encoded = append(ecs::Parent{identities.entity(object.parent)});
            }
            const auto mesh = mesh_by_object.find(object.object);
            if (encoded && mesh != mesh_by_object.end())
            {
                encoded = append(mesh->second->value);
            }
            if (!encoded)
            {
                return lux::cxx::unexpected(encoded.error());
            }
            content.push_back(std::move(captured));
        }
        return content;
    }
} // namespace lux::editor::scene::detail
