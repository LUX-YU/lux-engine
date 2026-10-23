#include <lux/engine/editor/ModelPlacement.hpp>
#include <lux/engine/editor/SceneDocument.hpp>
#include <lux/engine/resource/asset/model/ModelAsset.hpp>
#include <lux/engine/simulation/ecs/Parent.hpp>
#include <lux/engine/simulation/ecs/Transform.hpp>
#include <lux/engine/simulation/ecs/Visual.hpp>
#include <lux/engine/simulation/ecs/WorldEntityMap.hpp>

#include <random>

namespace lux::editor
{
    SceneEditResult<SceneEdit> prepareModelPlacement(
        const SceneDocument& document,
        const asset::ModelAsset& asset,
        const Eigen::Affine3d& placement,
        partition::PartitionOrdinal partition
    ) noexcept
    {
        namespace ecs = simulation::ecs;
        const auto& model = asset.data(); // ModelAsset construction already validated its indices and graph.
        const auto* transforms = document.schemas().find(cxx::typeToken<ecs::Transform3D>());
        const auto* parents = document.schemas().find(cxx::typeToken<ecs::Parent>());
        const auto* meshes = document.schemas().find(cxx::typeToken<ecs::Mesh3D>());
        const bool missing_schema = !transforms || !parents || !meshes;
        if (missing_schema || !placement.matrix().allFinite())
        {
            return cxx::unexpected(VSceneEditFailure{ESceneEditError::INVALID_COMPONENT});
        }
        std::mt19937 random{std::random_device{}()};
        uuids::uuid_random_generator generate{random};
        std::vector<world::WorldObjectId> node_ids;
        node_ids.reserve(model.nodes.size());
        ecs::WorldEntityMap references;
        references.reserve(model.nodes.size());
        for (std::size_t i = 0; i < model.nodes.size(); ++i)
        {
            node_ids.push_back({generate()});
            // Temporary codec-local ordinals, not runtime entities. Encoding converts every Parent
            // to its stable WorldObjectId; the edit's real admission resolves actual entity generations.
            if (!references.bind(node_ids.back(), static_cast<ecs::Entity>(i)))
            {
                std::terminate();
            }
        }
        std::vector<std::uint32_t> parent(model.nodes.size(), UINT32_MAX);
        for (std::uint32_t i = 0; i < model.nodes.size(); ++i)
        {
            for (const auto child : model.nodes[i].children)
            {
                if (parent[child] != UINT32_MAX)
                {
                    return cxx::unexpected(VSceneEditFailure{ESceneEditError::HIERARCHY_CYCLE});
                }
                parent[child] = i;
            }
        }
        const auto append = [&](SceneObjectData& object, const ecs::ComponentSchema& schema, const void* value
                            ) -> SceneEditResult<void>
        {
            auto capture = schema.capture_value(value, schema.code_lifetime);
            auto bytes = capture.encode(references, 4096);
            if (!bytes)
            {
                return cxx::unexpected(VSceneEditFailure{bytes.error()});
            }
            object.components.push_back({schema.id, schema.version, std::move(*bytes)});
            return {};
        };
        SceneCreateObjects result;
        for (std::uint32_t i = 0; i < model.nodes.size(); ++i)
        {
            const auto& node = model.nodes[i];
            Eigen::Affine3d local = node.local_transform.cast<double>();
            if (i == model.root_node)
            {
                local = placement * local;
            }
            Eigen::Matrix3d rotation, scaling;
            local.computeRotationScaling(&rotation, &scaling);
            // Transform3D is TRS. Do not silently discard imported shear.
            const Eigen::Matrix3d diagonal = scaling.diagonal().asDiagonal();
            if (!scaling.isApprox(diagonal, 1e-5))
            {
                return cxx::unexpected(VSceneEditFailure{ESceneEditError::INVALID_COMPONENT});
            }
            ecs::Transform3D transform{local.translation(), Eigen::Quaterniond{rotation}, scaling.diagonal()};
            SceneObjectData object{node_ids[i], partition};
            if (auto encoded = append(object, *transforms, &transform); !encoded)
            {
                return cxx::unexpected(encoded.error());
            }
            if (parent[i] != UINT32_MAX)
            {
                const ecs::Parent value{static_cast<ecs::Entity>(parent[i])};
                if (auto encoded = append(object, *parents, &value); !encoded)
                {
                    return cxx::unexpected(encoded.error());
                }
            }
            result.objects.push_back(std::move(object));
            for (auto index : node.primitives)
            {
                const auto& primitive = model.primitives[index];
                SceneObjectData mesh{world::WorldObjectId{generate()}, partition};
                const ecs::Transform3D identity;
                const ecs::Parent value{static_cast<ecs::Entity>(i)};
                const ecs::Mesh3D visual{rdesc::MeshVisualDescription{primitive.mesh, primitive.material}};
                for (auto encoded :
                     {append(mesh, *transforms, &identity),
                      append(mesh, *parents, &value),
                      append(mesh, *meshes, &visual)})
                {
                    if (!encoded)
                    {
                        return cxx::unexpected(encoded.error());
                    }
                }
                result.objects.push_back(std::move(mesh));
            }
        }
        return SceneEdit{document.revision(), std::move(result)};
    }
} // namespace lux::editor
